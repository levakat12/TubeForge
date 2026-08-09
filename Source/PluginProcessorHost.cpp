// What the host and the project file see: parameters, programs, state and the
// editor handle. None of it runs on the audio thread.

#include "PluginProcessorInternal.h"

juce::AudioProcessorEditor* TubeForgeAudioProcessor::createEditor()
{
    return new TubeForgeAudioProcessorEditor(*this);
}

juce::AudioProcessorParameter* TubeForgeAudioProcessor::getBypassParameter() const
{
    return parameterState.getParameter(ParameterIds::bypass);
}

bool TubeForgeAudioProcessor::hasEditor() const { return true; }

const juce::String TubeForgeAudioProcessor::getName() const { return JucePlugin_Name; }

// MIDI is accepted for program change only: switching rigs from a foot controller is
// ordinary practice on stage, and it is the one thing a guitarist cannot do from the mouse.
bool TubeForgeAudioProcessor::acceptsMidi() const { return true; }

bool TubeForgeAudioProcessor::producesMidi() const { return false; }

bool TubeForgeAudioProcessor::isMidiEffect() const { return false; }

int TubeForgeAudioProcessor::getNumPrograms() { return factoryProgramCount; }

int TubeForgeAudioProcessor::getCurrentProgram()
{
    // Programs are not stored separately: they are the two parameters that pick a factory
    // voice, so the current program is whatever those two say. That keeps a program change
    // and a manual change to the same controls from disagreeing.
    return static_cast<int>(factoryAmpIndex(
        static_cast<int>(std::lround(parameterOf(Param::instrument))),
        static_cast<int>(std::lround(parameterOf(Param::topology)))));
}

void TubeForgeAudioProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= factoryProgramCount) return;
    // Through the parameters rather than into the amplifier, so the change is automatable,
    // saved with the project, and picked up by the traditional amp's existing preset
    // crossfade rather than switching under the signal.
    // The inverse of factoryAmpIndex. Kept next to setCurrentProgram rather than beside it in
    // the header, because this is the only place that has to undo the packing.
    const auto topologies = static_cast<int>(nts::amp::topologyCount);
    // A foot controller selecting a factory voice replaces the amplifier, so a matched rig is no
    // longer what is playing and Auto Match stops holding rather than warning about a change the
    // player made with their foot in the middle of a song.
    const AutoWriteScope autoWrite(*this);
    setParameterValue(parameterState, ParameterIds::instrument, static_cast<float>(index / topologies));
    setParameterValue(parameterState, ParameterIds::topology, static_cast<float>(index % topologies));
    clearAutoMatchHold();
}

const juce::String TubeForgeAudioProcessor::getProgramName(int index)
{
    if (index < 0 || index >= factoryProgramCount) return {};
    const auto topologies = static_cast<int>(nts::amp::topologyCount);
    const auto topology = static_cast<nts::amp::Topology>(index % topologies);
    return juce::String(std::string(nts::amp::topologyName(topology)))
         + (index / topologies == 1 ? " Bass" : " Guitar");
}

void TubeForgeAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void TubeForgeAudioProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    const auto json = nts::state::serialize(makeProjectState(), false);
    destinationData.replaceAll(json.data(), json.size());
}

void TubeForgeAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    const auto parsed = nts::state::deserialize(
        std::string_view(static_cast<const char*>(data), static_cast<std::size_t>(sizeInBytes)));
    if (! parsed || ! applyProjectState(*parsed.state))
    {
        logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::warning,
                     "state", "state-rejected", "Plugin state was rejected",
                     parsed ? "validation or parameter activation failed" : parsed.error });
        return;
    }
}

juce::Result TubeForgeAudioProcessor::saveProject(const juce::File& file) const
{
    if (file.replaceWithText(nts::state::serialize(makeProjectState(), true)))
        return juce::Result::ok();
    return juce::Result::fail("Unable to write the project file");
}

juce::Result TubeForgeAudioProcessor::loadProject(const juce::File& file)
{
    if (! file.existsAsFile())
        return juce::Result::fail("Project file does not exist");

    const auto parsed = nts::state::deserialize(file.loadFileAsString().toStdString());
    if (! parsed)
        return juce::Result::fail(parsed.error);
    if (! applyProjectState(*parsed.state))
        return juce::Result::fail("Project parameters could not be applied");
    return juce::Result::ok();
}

nts::diagnostics::DiagnosticsSnapshot TubeForgeAudioProcessor::diagnosticsSnapshot() const noexcept
{
    return diagnostics.snapshot();
}

juce::String TubeForgeAudioProcessor::modeName() const
{
    if (standaloneApplicationMode)
        return "Standalone";

    switch (wrapperType)
    {
        case wrapperType_Standalone: return "Standalone";
        case wrapperType_VST3: return "VST3";
        default: return "Host plugin";
    }
}

juce::String TubeForgeAudioProcessor::deviceStatusText() const
{
    if (standaloneApplicationMode || wrapperType == wrapperType_Standalone)
        return "Device managed by the embedded Audio settings panel";
    return "Audio device managed by the host";
}

void TubeForgeAudioProcessor::refreshNonRealtimeDiagnostics() noexcept
{
    const auto memory = nts::diagnostics::sampleProcessMemory();
    diagnostics.setProcessMemory(memory.workingSetBytes, memory.privateBytes);
}

void TubeForgeAudioProcessor::loadVoicingDefaults()
{
    /* Put every control back to what the selected amplifier actually specifies.

       This is the "default" the interface had no way to reach. A voicing supplies a full set of
       values, but only the ones with no host control -- stage low cuts, the tone-stack topology,
       the phase inverter, the waveshapes -- ever reach the engine from it. Everything a knob owns
       is taken from the knob, so selecting Valve Flagship on a rig left at a guitar session's
       settings gives its stage layout with the previous amplifier's gain, tightness and cuts.

       That is not a cosmetic gap. `tightness` sets a high-pass floor of `35 + 310 * t^2` Hz, so
       the parameter default of 7.2 puts a **196 Hz** cut on everything -- fine on a guitar and
       ruinous on a bass, where it removes the fundamental of every note below G3 and leaves the
       harmonics to be distorted on their own. Measured against a 55 Hz note, a voicing's own
       values give 10 dB less high-frequency hash and a fundamental twice as loud as the parameter
       defaults do.

       Deliberately **not** automatic on a voicing change: overwriting a player's edits because
       they auditioned another amplifier is its own kind of broken. It is an action they ask for.

       Shares `applyRecoveredRig`'s body rather than repeating the inverse mapping. There are
       thirty-odd assignments in it and two copies would be two things to keep in step -- which is
       exactly how `gain` came to be missing from the one that already existed.
    */
    const auto instrument = static_cast<int>(std::lround(parameterOf(Param::instrument)));
    const auto topology = static_cast<int>(std::lround(parameterOf(Param::topology)));
    applyRecoveredRig(factoryAmpParameters[factoryAmpIndex(instrument, topology)], false);
    // The voicing has no opinion on these, so they go to the positions a fresh instance has.
    setParameterValue(parameterState, ParameterIds::panelSwitch, 0.0f);
    setParameterValue(parameterState, ParameterIds::output, -6.0f);
}

void TubeForgeAudioProcessor::applyRecoveredRig(const nts::amp::AmpParameters& p,
                                                bool isolateChain)
{
    // Reached through a callback from StudioServices rather than called by it directly, so the
    // dependency runs one way: the processor knows about the studio, not the reverse.
    //
    // Every write below is inside this scope, so the Auto Match guard reads them as the rig
    // being applied rather than as a player reaching for forty-seven controls at once --
    // setParameterValue issues real change gestures, which is otherwise indistinguishable.
    const AutoWriteScope autoWrite(*this);

    setParameterValue(parameterState, ParameterIds::engineMode, 0.0f);
    /* The Gain knob is a macro over the four stage drives, not a knob beside them:
       `currentAmpParameters` adds `(gain - 5) * 3 dB` to every stage. Leaving it where the user
       had it therefore applied up to +/-15 dB on top of the stage drives written below, so an
       applied candidate was never the candidate that won -- and the error landed on the one
       axis the search spends five of its twelve pool slots exploring. Neutral means 5. */
    setParameterValue(parameterState, ParameterIds::gain, 5.0f);
    /* `applyPanelSwitch` runs last in `currentAmpParameters` and rewrites the parameter block,
       so a Bright or Deep switch left over from a previous voicing was applied on top of the
       matched rig. The candidate carries no switch, so the answer is none. */
    setParameterValue(parameterState, ParameterIds::panelSwitch, 0.0f);
    /* Candidates are rendered at 4x or higher -- see the oversampling note in
       `setCandidateParameters` -- because at 1x the analyser measures the render's aliasing
       rather than the amplifier. Playing the winner back at 1x is playing back exactly the
       fold-back the search excluded from scoring. Auto follows gain and reaches 4x or 8x where
       it matters; the performance tier still caps it, and `reconstructionApplyWarnings` says so
       when it does rather than this overruling a choice the user made about their machine. */
    setParameterValue(parameterState, ParameterIds::oversampling,
                      static_cast<float>(automaticOversamplingIndex));
    setParameterValue(parameterState, ParameterIds::instrument,
                      p.instrument == nts::amp::Instrument::bass ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::topology,
                      static_cast<float>(static_cast<int>(p.topology)));
    setParameterValue(parameterState, ParameterIds::input, p.manualInputTrimDb);
    /* Gain back to its neutral centre, and this was missing.

       `currentAmpParameters` adds `(gain - 5) * 3` dB to **every** stage drive, so writing the
       stage drives absolutely -- which the loop below does -- while leaving Gain wherever it
       happened to sit displaces all four by up to 15 dB. A rig recovered from a match could
       therefore be fifteen decibels away from the candidate that won it, and the difference would
       read as "the match is not very good" rather than as a control that was never reset.
    */
    setParameterValue(parameterState, ParameterIds::gain, 5.0f);
    setParameterValue(parameterState, ParameterIds::bass, p.toneStack.bass * 10.0f);
    setParameterValue(parameterState, ParameterIds::mid, p.toneStack.mid * 10.0f);
    setParameterValue(parameterState, ParameterIds::treble, p.toneStack.treble * 10.0f);
    setParameterValue(parameterState, ParameterIds::presence, p.powerAmp.presence * 10.0f);
    setParameterValue(parameterState, ParameterIds::resonance, p.powerAmp.resonance * 10.0f);
    setParameterValue(parameterState, ParameterIds::master, p.powerAmp.masterDb);
    setParameterValue(parameterState, ParameterIds::lowCut, p.preEq.lowCutHz);
    setParameterValue(parameterState, ParameterIds::highCut, p.preEq.highCutHz);
    setParameterValue(parameterState, ParameterIds::gateEnabled, p.gateEnabled ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::gateThreshold, p.gateThresholdDb);
    setParameterValue(parameterState, ParameterIds::gateDepth, p.gateDepthDb);
    setParameterValue(parameterState, ParameterIds::gateAttack, p.gateAttackMs);
    setParameterValue(parameterState, ParameterIds::gateHold, p.gateHoldMs);
    setParameterValue(parameterState, ParameterIds::gateRelease, p.gateReleaseMs);
    setParameterValue(parameterState, ParameterIds::loudnessMatch, p.loudnessMatch ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::sag, p.powerAmp.sag * 100.0f);
    setParameterValue(parameterState, ParameterIds::feedback, p.powerAmp.feedback * 100.0f);
    setParameterValue(parameterState, ParameterIds::crossover, p.bass.crossoverHz);
    setParameterValue(parameterState, ParameterIds::cleanBlend, p.bass.cleanBlend * 100.0f);
    setParameterValue(parameterState, ParameterIds::dryBlend, p.dryBlend * 100.0f);
    setParameterValue(parameterState, ParameterIds::lowBandDrive, p.bass.lowDriveDb);
    setParameterValue(parameterState, ParameterIds::lowBandLevel, p.bass.lowLevelDb);
    setParameterValue(parameterState, ParameterIds::highBandLevel, p.bass.highLevelDb);
    const std::array stageIds { ParameterIds::stage1, ParameterIds::stage2,
                                ParameterIds::stage3, ParameterIds::stage4 };
    for (std::size_t stage = 0; stage < stageIds.size(); ++stage)
        setParameterValue(parameterState, stageIds[stage], p.stages[stage].driveDb);

    // The rest of the voicing the candidate was rendered through. Left alone until now,
    // which meant an applied rig kept the previous sound's tightness, bias and cabinet and
    // so could not match the render it had just been chosen from.
    setParameterValue(parameterState, ParameterIds::tightness, p.preEq.tightness * 10.0f);
    setParameterValue(parameterState, ParameterIds::pickEmphasis, p.preEq.pickEmphasisDb);
    setParameterValue(parameterState, ParameterIds::bias, p.stages[0].bias);
    setParameterValue(parameterState, ParameterIds::cabinet, p.cabinet.bypass ? 0.0f : 1.0f);
    setParameterValue(parameterState, ParameterIds::cabinetBlend, p.cabinet.blend * 100.0f);
    setParameterValue(parameterState, ParameterIds::cabinetWidth, p.cabinet.width * 100.0f);
    setParameterValue(parameterState, ParameterIds::cabinetAlignment,
                      static_cast<float>(p.cabinet.slots[1].delaySamples));
    /* The cabinet fields the search fits and the apply path used to drop on the floor.

       `highCutHz` is fitted from the reference's own darkness
       (SourceReconstruction.cpp, `setCandidateParameters`) and had no parameter to be written
       into, so the one cabinet measurement a match makes was discarded every time a candidate
       was applied -- and Auto Match then guarded four cabinet controls of which three had never
       been written. Both halves of that are fixed here. */
    setParameterValue(parameterState, ParameterIds::cabLowCut, p.cabinet.lowCutHz);
    setParameterValue(parameterState, ParameterIds::cabHighCut, p.cabinet.highCutHz);
    setParameterValue(parameterState, ParameterIds::cabDiBlend, p.cabinet.bassDiBlend * 100.0f);

    // Everything the offline render did not contain. Opt-in rather than automatic: a board
    // and a reverb are the player's own work, and silently discarding them to win a tone
    // comparison is not a trade this should make on its own.
    if (isolateChain)
    {
        for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
            setParameterValue(parameterState, pedalParameterId(slot, PedalControl::bypass), 1.0f);
        // Bypassing a slot keeps its settings and its loaded model; the sends have no such
        // control, so closing the mix is the only way to take them out of the path.
        setParameterValue(parameterState, ParameterIds::delayMix, 0.0f);
        setParameterValue(parameterState, ParameterIds::reverbMix, 0.0f);
    }

    appliedRecoveredRig = p;
    // What was actually written, recorded while it is still true. `appliedRecoveredRig` holds
    // engine parameters; "keep the matched value" needs the host parameter the user just moved.
    captureAutoMatchSnapshot(isolateChain);
}

juce::StringArray TubeForgeAudioProcessor::reconstructionApplyWarnings() const
{
    if (! appliedRecoveredRig) return {};
    const auto& p = *appliedRecoveredRig;

    juce::StringArray warnings;

    for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
    {
        // A slot at zero mix is exactly transparent whatever else it is set to, so it is
        // not worth warning about.
        const auto pedal = currentPedalParameters(slot);
        if (pedal.model == 0 || pedal.bypassed || pedal.mix <= 0.5f) continue;
        // Named from the model table, so a pedal added to it never warns as "unknown".
        warnings.add("Pedal " + juce::String(slot + 1) + " ("
            + juce::String(std::string(nts::pedals::pedalModel(pedal.model).name)).toLowerCase()
            + ") is still in front of the amplifier. The match was made without it, and a "
              "pedal ahead of the preamp changes the tone rather than adding to it.");
    }

    if (const auto mix = parameterOf(Param::delayMix); mix > 0.5f)
        warnings.add("Delay is still on the output at " + juce::String(juce::roundToInt(mix))
                     + "% mix. The match was made dry.");
    if (const auto mix = parameterOf(Param::reverbMix); mix > 0.5f)
        warnings.add("Reverb is still on the output at " + juce::String(juce::roundToInt(mix))
                     + "% mix. The match was made dry.");

    // Only worth saying while the cabinet section is actually in the path: a loaded response
    // that nothing is convolving against changes nothing.
    if (parameterOf(Param::cabinet) >= 0.5f)
        for (int slot = 0; slot < 2; ++slot)
            if (const auto ir = cabinetIrFile(slot); ir != juce::File {})
                warnings.add("Cabinet " + juce::String(slot == 0 ? "A" : "B") + " is loaded with "
                    + ir.getFileName() + ", not the response the candidate was rendered through. "
                      "Clear it to hear the rig that was actually matched.");

    // The search fits a broad EQ to the reference and renders every candidate through it,
    // and there is no control here that holds it, so it cannot travel with the rest of the
    // rig. Disclosed rather than dropped in silence, which is what used to happen.
    const auto shelf = p.preEq.lowShelfEnabled ? p.preEq.lowShelfDb : 0.0f;
    const auto mid = p.preEq.midEmphasisEnabled ? p.preEq.midEmphasisDb : 0.0f;
    if (std::abs(shelf) > 0.25f || std::abs(mid) > 0.25f)
        warnings.add("The match fitted a reference EQ of "
            + juce::String(shelf, 1) + " dB low shelf and " + juce::String(mid, 1)
            + " dB mid that the amplifier has no control for, so it is not applied. Expect the "
              "live sound to sit that far away from the candidate you auditioned.");

    /* The candidate was rendered at 4x or higher and the tier will not let playback reach it.

       Read from the tier rather than from the oversampling parameter because applying a rig now
       pins that parameter to Auto, so the parameter says what was asked for and the tier says
       what will happen. Eco is the case: it caps at 1x, which is precisely the fold-back the
       search excluded from its own scoring. */
    if (const auto factor = tierLimits().maximumOversamplingFactor; factor < 4)
        warnings.add("The " + juce::String(factor) + "x oversampling ceiling on this performance "
            "tier is below the " + juce::String(4) + "x the candidate was rendered at, so the live "
            "sound carries aliasing the match was scored without. Raise the tier to hear what won.");

    /* Everything the search fitted that no parameter can carry, and which is therefore **not
       applied at all**.

       `currentAmpParameters` builds the live rig from the factory voicing and overrides it with
       the host parameters. A fitted field with no parameter behind it is not merely un-editable:
       it silently reverts to whatever the voicing says. Compared against the voicing that will
       actually be used rather than against a literal, so this reports the real gap between the
       render that was auditioned and the rig that is now playing. */
    const auto& voicing = factoryAmpParameters[factoryAmpIndex(
        p.instrument == nts::amp::Instrument::bass ? 1 : 0, static_cast<int>(p.topology))];
    juce::StringArray dropped;
    if (p.stageCount != voicing.stageCount)
        dropped.add("a " + juce::String(static_cast<int>(p.stageCount)) + "-stage preamp (playing "
                    + juce::String(static_cast<int>(voicing.stageCount)) + ")");
    if (const auto fitted = p.stages[0].attackReduction, live = voicing.stages[0].attackReduction;
        std::abs(fitted - live) > 0.02f)
        dropped.add("attack softening at " + juce::String(juce::roundToInt(fitted * 100.0f))
                    + "% (playing " + juce::String(juce::roundToInt(live * 100.0f)) + "%)");
    if (const auto fitted = p.stages[0].asymmetry, live = voicing.stages[0].asymmetry;
        std::abs(fitted - live) > 0.02f)
        dropped.add("stage asymmetry at " + juce::String(juce::roundToInt(fitted * 100.0f))
                    + "% (playing " + juce::String(juce::roundToInt(live * 100.0f)) + "%)");
    if (const auto fitted = p.powerAmp.saturation, live = voicing.powerAmp.saturation;
        std::abs(fitted - live) > 0.02f)
        dropped.add("power saturation at " + juce::String(juce::roundToInt(fitted * 100.0f))
                    + "% (playing " + juce::String(juce::roundToInt(live * 100.0f)) + "%)");
    if (std::abs(p.postLowDb - voicing.postLowDb) > 0.25f
        || std::abs(p.postMidDb - voicing.postMidDb) > 0.25f
        || std::abs(p.postHighDb - voicing.postHighDb) > 0.25f)
        dropped.add("a post EQ of " + juce::String(p.postLowDb, 1) + " / "
            + juce::String(p.postMidDb, 1) + " / " + juce::String(p.postHighDb, 1) + " dB");
    if (! dropped.isEmpty())
        warnings.add("The match fitted " + dropped.joinIntoString(", ")
            + ", and there is no control to hold any of it -- so it is not applied and the voicing's "
              "own values are what you are hearing.");

    return warnings;
}

juce::AudioProcessorValueTreeState::ParameterLayout TubeForgeAudioProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Bool = juce::AudioParameterBool;
    using Choice = juce::AudioParameterChoice;
    using Range = juce::NormalisableRange<float>;
    using FloatAttributes = juce::AudioParameterFloatAttributes;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::input, 1 }, "Input",
                                       Range { -60.0f, 24.0f, 0.1f }, 0.0f,
                                       FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::output, 1 }, "Output",
                                       Range { -60.0f, 24.0f, 0.1f }, 0.0f,
                                       FloatAttributes {}.withLabel("dB")));
    // Meta rather than an ordinary automatable parameter: it is a control-surface switch
    // that the host also drives through getBypassParameter.
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::bypass, 1 }, "Bypass", false,
                                      juce::AudioParameterBoolAttributes {}.withMeta(true)));
    const auto addFloat = [&layout](const char* id, const char* name, Range range, float initial,
                                    const char* label = "")
    {
        layout.add(std::make_unique<Float>(juce::ParameterID { id, 1 }, name, range, initial,
                                           FloatAttributes {}.withLabel(label)));
    };
    addFloat(ParameterIds::gain, "Gain", Range { 0.0f, 10.0f, 0.01f }, 5.0f);
    addFloat(ParameterIds::bass, "Bass", Range { 0.0f, 10.0f, 0.01f }, 5.0f);
    addFloat(ParameterIds::mid, "Mid", Range { 0.0f, 10.0f, 0.01f }, 5.0f);
    addFloat(ParameterIds::treble, "Treble", Range { 0.0f, 10.0f, 0.01f }, 5.0f);
    addFloat(ParameterIds::presence, "Presence", Range { 0.0f, 10.0f, 0.01f }, 5.0f);
    addFloat(ParameterIds::resonance, "Resonance", Range { 0.0f, 10.0f, 0.01f }, 5.0f);
    addFloat(ParameterIds::master, "Master", Range { -60.0f, 12.0f, 0.1f }, -3.0f, "dB");
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::cabinet, 1 }, "Cabinet", true));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::instrument, 1 }, "Instrument",
                                        juce::StringArray { "Guitar", "Bass" }, 0));
    // Built from the engine's own names rather than repeated here: the choice index *is* the
    // Topology value, so a voicing appended to the enum must appear in this list at the same
    // position or every host automation lane silently points at the wrong amplifier.
    juce::StringArray topologyNames;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
        topologyNames.add(juce::String(std::string(
            nts::amp::topologyName(static_cast<nts::amp::Topology>(index)))));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::topology, 1 }, "Topology",
                                        topologyNames, 0));
    /* One list for every voicing, filtered only in the interface.

       A switch that exists on one amplifier and not another is exactly the shape of thing that
       tempts a per-voicing parameter list, and that would make this lane's meaning depend on the
       topology parameter's value -- the same trap the topology list itself avoids. The engine
       ignores a switch the current voicing does not have, so an out-of-range pairing is inert
       rather than wrong. */
    juce::StringArray panelSwitchNames;
    for (std::size_t index = 0; index < nts::amp::panelSwitchCount; ++index)
        panelSwitchNames.add(juce::String(std::string(
            nts::amp::panelSwitchName(static_cast<nts::amp::PanelSwitch>(index)))));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::panelSwitch, 1 },
                                        "Panel switch", panelSwitchNames, 0));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::engineMode, 1 }, "Engine mode",
                                        juce::StringArray { "Traditional", "Neural capture", "Physical circuit" }, 0));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::neuralMonitor, 1 }, "Neural monitor",
                                        juce::StringArray { "Model", "Bypass DI", "Loudness matched" }, 0));
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::neuralCompensation, 1 },
                                      "Neural input compensation", false));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::circuitPreampTube, 1 }, "Circuit preamp tube",
                                        juce::StringArray { "12AU7", "12AT7", "12AX7" }, 2));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::circuitPowerTube, 1 }, "Circuit power tube",
                                        juce::StringArray { "6V6", "EL34" }, 1));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::circuitPowerTopology, 1 }, "Circuit power topology",
                                        juce::StringArray { "Single-ended", "Push-pull Class A", "Push-pull Class AB" }, 2));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::circuitToneStack, 1 }, "Circuit tone stack",
                                        juce::StringArray { "Vintage FMV", "Modern", "Bass" }, 0));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::circuitBackend, 1 }, "Circuit solver",
                                        juce::StringArray { "Gray-box", "Numerical" }, 0));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::circuitCabinetStyle, 1 }, "Circuit cabinet",
                                        juce::StringArray { "Reactive", "Open back", "Bass sealed" }, 0));
    addFloat(ParameterIds::stage1, "Stage 1 gain", Range { -12.0f, 42.0f, 0.1f }, 11.0f, "dB");
    addFloat(ParameterIds::stage2, "Stage 2 gain", Range { -12.0f, 42.0f, 0.1f }, 13.5f, "dB");
    addFloat(ParameterIds::stage3, "Stage 3 gain", Range { -12.0f, 42.0f, 0.1f }, 16.0f, "dB");
    addFloat(ParameterIds::stage4, "Stage 4 gain", Range { -12.0f, 42.0f, 0.1f }, 18.5f, "dB");
    addFloat(ParameterIds::bias, "Stage bias", Range { -0.8f, 0.8f, 0.001f }, 0.0f);
    addFloat(ParameterIds::lowCut, "Pre low cut", Range { 20.0f, 500.0f, 1.0f, 0.4f }, 95.0f, "Hz");
    addFloat(ParameterIds::highCut, "Pre high cut", Range { 3000.0f, 22000.0f, 10.0f, 0.5f }, 16500.0f, "Hz");
    // "Auto" is appended rather than inserted so the existing indices keep their
    // meaning and saved projects and host automation stay valid.
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::oversampling, 1 }, "Oversampling",
                                        juce::StringArray { "1x (minimum latency)", "2x", "4x", "8x",
                                                            "Auto (follows gain)" }, 4));
    // Appended rather than grouped with the amplifier controls: it is a rig-wide setting, not a
    // tone control, and appending keeps every existing index stable for saved projects and host
    // automation. Defaults to Standard, which is what the plug-in did before the tier existed
    // apart from the oversampling ceiling.
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::performanceTier, 1 },
                                        "Performance",
                                        juce::StringArray { "Eco (lowest CPU)", "Standard",
                                                            "Studio (no limits)" }, 1));
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::gateEnabled, 1 },
                                      "Noise gate", true));
    addFloat(ParameterIds::gateThreshold, "Gate threshold", Range { -90.0f, -20.0f, 0.1f }, -58.0f, "dB");
    // Depth rather than a fixed full mute: a shallow gate ducks hum between
    // phrases without swallowing the tail of a note. Defaulted to -15 dB, which is
    // noise-floor removal; -90 dB is still there for anyone who wants a hard gate.
    addFloat(ParameterIds::gateDepth, "Gate depth", Range { -90.0f, 0.0f, 0.1f }, -15.0f, "dB");
    addFloat(ParameterIds::gateAttack, "Gate attack", Range { 0.1f, 50.0f, 0.1f }, 2.0f, "ms");
    addFloat(ParameterIds::gateHold, "Gate hold", Range { 0.0f, 500.0f, 1.0f }, 60.0f, "ms");
    addFloat(ParameterIds::gateRelease, "Gate release", Range { 5.0f, 2000.0f, 1.0f }, 250.0f, "ms");
    // Auditioning two rigs at matched loudness is genuinely useful, which is why this defaults
    // on and why hand-built presets keep it. It is also a slow compressor across the output that
    // normalises the amplifier's RMS back to the dry input's, so raising drive or master stops
    // getting louder -- the reason it now has a control instead of only a field in preset JSON.
    // Song-match presets ship with it off; see setCandidateParameters in SourceReconstruction.
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::loudnessMatch, 1 },
                                      "Loudness match", true));
    addFloat(ParameterIds::sag, "Sag", Range { 0.0f, 100.0f, 0.1f }, 35.0f, "%");
    addFloat(ParameterIds::feedback, "Feedback", Range { 0.0f, 100.0f, 0.1f }, 35.0f, "%");
    /* The ceiling went 500 Hz to 1 kHz, and the cost is worth stating rather than discovering.

       A real bi-amp head sweeps its crossover from about 100 Hz to 1 kHz and is recommended to be
       started at 500 -- which was *exactly* the old top rail, so the whole upper half of the
       control was missing and the one setting most worth having sat at the stop. Below about
       300 Hz this control is a mud guard on the fundamental; above it, it is the thing that makes
       a bi-amp a bi-amp, because the note body passes clean and only the attack is driven.

       **What it costs:** a host automation lane stores a normalised position, so an existing lane
       written against the 60-500 range now points at a higher frequency than it did. Saved
       *projects* are unaffected -- `ProjectState` stores the value in Hz -- and so are presets.
       Accepted deliberately: the alternative is a voicing whose defining number is unreachable.
    */
    addFloat(ParameterIds::crossover, "Bass crossover", Range { 60.0f, 1000.0f, 1.0f, 0.5f }, 180.0f, "Hz");
    addFloat(ParameterIds::cleanBlend, "Bass clean blend", Range { 0.0f, 100.0f, 0.1f }, 55.0f, "%");
    // Zero is a true bypass and is what every preset written before it existed carries, so an
    // old project cannot change sound because a new control appeared. See AmpParameters::dryBlend
    // for why this is not the same thing as either of the two blends that already existed.
    addFloat(ParameterIds::dryBlend, "Dry blend", Range { 0.0f, 100.0f, 0.1f }, 0.0f, "%");
    addFloat(ParameterIds::lowBandDrive, "Bass low drive", Range { -12.0f, 24.0f, 0.1f }, 2.6f, "dB");
    addFloat(ParameterIds::lowBandLevel, "Bass low level", Range { -24.0f, 12.0f, 0.1f }, 0.0f, "dB");
    addFloat(ParameterIds::highBandLevel, "Bass high level", Range { -24.0f, 12.0f, 0.1f }, 0.0f, "dB");
    addFloat(ParameterIds::cabinetAlignment, "Cabinet alignment", Range { 0.0f, 256.0f, 1.0f }, 0.0f, "samples");
    addFloat(ParameterIds::tightness, "Tightness", Range { 0.0f, 10.0f, 0.01f }, 7.2f);
    addFloat(ParameterIds::pickEmphasis, "Pick emphasis", Range { -12.0f, 12.0f, 0.1f }, 2.5f, "dB");
    // Effects default to silent: they are a send, so zero mix is a true bypass and an existing
    // project that predates them sounds exactly as it did.
    addFloat(ParameterIds::delayMix, "Delay mix", Range { 0.0f, 100.0f, 0.1f }, 0.0f, "%");
    addFloat(ParameterIds::delayTime, "Delay time", Range { 20.0f, 2000.0f, 1.0f, 0.4f }, 375.0f, "ms");
    addFloat(ParameterIds::delayFeedback, "Delay feedback", Range { 0.0f, 95.0f, 0.1f }, 35.0f, "%");
    addFloat(ParameterIds::delayTone, "Delay tone", Range { 800.0f, 20000.0f, 10.0f, 0.4f }, 6000.0f, "Hz");
    addFloat(ParameterIds::reverbMix, "Reverb mix", Range { 0.0f, 100.0f, 0.1f }, 0.0f, "%");
    addFloat(ParameterIds::reverbSize, "Reverb size", Range { 0.0f, 100.0f, 0.1f }, 50.0f, "%");
    addFloat(ParameterIds::reverbDamping, "Reverb damping", Range { 0.0f, 100.0f, 0.1f }, 50.0f, "%");
    // Defaults to 50%, which is what CabinetParameters already used, so the sound is unchanged
    // and the control simply exposes a blend that was previously fixed by the preset.
    addFloat(ParameterIds::cabinetBlend, "Cabinet blend", Range { 0.0f, 100.0f, 0.1f }, 50.0f, "%");
    // Splits the two cabinet slots left and right instead of summing them: one DI through two
    // different rigs, which is how a wide rhythm guitar is actually made. Defaults to 0, where the
    // stage is bit-identical to what it did before this existed, so no stored preset moves. Watch
    // the mono-fold reading when raising it with cabinet alignment in use.
    addFloat(ParameterIds::cabinetWidth, "Cabinet width", Range { 0.0f, 100.0f, 0.1f }, 0.0f, "%");

    /* The cabinet stage's own controls.

       **Every default here is the value the field effectively had before it was reachable**, so
       a project, a preset or a fresh instance sounds exactly as it did: unity level, unmuted, in
       phase, no delay on A, and the two pan defaults that are the hard left/right split `width`
       has always performed. The two cuts default to `CabinetParameters`' own defaults and the DI
       blend to the bypass value. A null test in `nts_amp_tests` is what holds that claim up.

       The pans are stored as a percentage rather than -1..1 because a host lane and a text field
       both read better that way; `currentCabinetParameters` scales them back. */
    addFloat(ParameterIds::cabLevelA, "Cabinet A level", Range { -60.0f, 12.0f, 0.1f }, 0.0f, "dB");
    addFloat(ParameterIds::cabLevelB, "Cabinet B level", Range { -60.0f, 12.0f, 0.1f }, 0.0f, "dB");
    addFloat(ParameterIds::cabPanA, "Cabinet A pan", Range { -100.0f, 100.0f, 0.1f }, -100.0f, "%");
    addFloat(ParameterIds::cabPanB, "Cabinet B pan", Range { -100.0f, 100.0f, 0.1f }, 100.0f, "%");
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::cabPhaseA, 1 },
                                      "Cabinet A polarity", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::cabPhaseB, 1 },
                                      "Cabinet B polarity", false));
    // Slot B's delay is `cabinetAlignment`, which predates this block and keeps its id; this is
    // the matching control for slot A. Aligning two microphones means moving whichever of them
    // is early, and with only a B delay half of those pairs were unreachable.
    addFloat(ParameterIds::cabDelayA, "Cabinet A alignment", Range { 0.0f, 256.0f, 1.0f }, 0.0f, "samples");
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::cabMuteA, 1 },
                                      "Cabinet A mute", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::cabMuteB, 1 },
                                      "Cabinet B mute", false));
    addFloat(ParameterIds::cabLowCut, "Cabinet low cut", Range { 20.0f, 500.0f, 1.0f, 0.4f }, 70.0f, "Hz");
    addFloat(ParameterIds::cabHighCut, "Cabinet high cut", Range { 2000.0f, 22000.0f, 10.0f, 0.5f }, 10500.0f, "Hz");
    // Blends the cabinet's *input* against its output -- it bypasses the speaker, not the drive.
    // See AmpParameters::dryBlend, which spells out how the three blends in this plug-in differ.
    addFloat(ParameterIds::cabDiBlend, "Cabinet DI blend", Range { 0.0f, 100.0f, 0.1f }, 0.0f, "%");
    addFloat(ParameterIds::cabOutputTrim, "Cabinet output", Range { -24.0f, 12.0f, 0.1f }, 0.0f, "dB");

    /* The built-in cabinet model. Built from the engine's own names rather than repeated here, so
       a cabinet appended to `nts::ir::CabinetKind` appears in this list at the same position --
       the choice index *is* the enum value, the same contract the topology list keeps.

       **The defaults are a real cabinet, and old projects override them back to Legacy.** A fresh
       instance should sound like a cabinet rather than like the two synthetic decays that stood in
       for one; a project saved before schema 6 was voiced against those decays and must keep them.
       `applyProjectState` writes Legacy into both slots when it restores a project with no cabinet
       block, which is exactly the set of projects that predate the model. */
    juce::StringArray cabinetNames;
    for (std::size_t index = 0; index < static_cast<std::size_t>(nts::ir::CabinetKind::count); ++index)
        cabinetNames.add(juce::String(std::string(
            nts::ir::cabinetName(static_cast<nts::ir::CabinetKind>(index)))));
    juce::StringArray microphoneNames;
    for (std::size_t index = 0; index < static_cast<std::size_t>(nts::ir::MicrophoneKind::count); ++index)
        microphoneNames.add(juce::String(std::string(
            nts::ir::microphoneName(static_cast<nts::ir::MicrophoneKind>(index)))));

    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::cabModelA, 1 },
                                        "Cabinet A model", cabinetNames,
                                        static_cast<int>(nts::ir::CabinetKind::closed4x12)));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::cabModelB, 1 },
                                        "Cabinet B model", cabinetNames,
                                        static_cast<int>(nts::ir::CabinetKind::open2x12)));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::cabMicA, 1 },
                                        "Cabinet A microphone", microphoneNames,
                                        static_cast<int>(nts::ir::MicrophoneKind::dynamicPresence)));
    // A different microphone on B by default, because two *different* responses are what makes the
    // blend and the width control mean anything -- the same one twice is one cabinet at 6 dB.
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::cabMicB, 1 },
                                        "Cabinet B microphone", microphoneNames,
                                        static_cast<int>(nts::ir::MicrophoneKind::ribbon)));
    addFloat(ParameterIds::cabPositionA, "Cabinet A position", Range { 0.0f, 100.0f, 0.1f }, 25.0f, "%");
    addFloat(ParameterIds::cabPositionB, "Cabinet B position", Range { 0.0f, 100.0f, 0.1f }, 65.0f, "%");
    addFloat(ParameterIds::cabDistanceA, "Cabinet A distance", Range { 1.0f, 24.0f, 0.1f, 0.6f }, 2.0f, "in");
    addFloat(ParameterIds::cabDistanceB, "Cabinet B distance", Range { 1.0f, 24.0f, 0.1f, 0.6f }, 5.0f, "in");

    /* How a loaded impulse response is prepared, per slot.

       Every default is index 0 and every index 0 is what the loader already did, so a project
       that loaded a response before these existed prepares it exactly as it did.

       Length was previously the performance tier's business alone, and it is a legitimate *tonal*
       choice as well: past the first few milliseconds a cabinet impulse is mostly room, so a short
       response is drier and cheaper at the same time. "Tier maximum" keeps the old behaviour of
       following the tier, and an explicit length is still capped by it -- a user asking for 4096
       taps on Eco is asking for something that tier exists to refuse. */
    const juce::StringArray lengthNames { "Tier maximum", "128", "256", "512", "1024", "2048", "4096" };
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::cabIrLengthA, 1 },
                                        "Cabinet A response length", lengthNames, 0));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::cabIrLengthB, 1 },
                                        "Cabinet B response length", lengthNames, 0));
    /* Peak-normalising a room-heavy response against a close-mic one makes the blend control lie
       about what it is doing: the room capture has a much lower average level for the same peak,
       so a blend of 50% is nothing of the sort. RMS matches what is heard; None leaves the file
       exactly as its author wrote it, which is what somebody comparing two vendors' packs wants. */
    const juce::StringArray normalisationNames { "Peak", "RMS", "None" };
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::cabIrNormA, 1 },
                                        "Cabinet A normalisation", normalisationNames, 0));
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::cabIrNormB, 1 },
                                        "Cabinet B normalisation", normalisationNames, 0));
    // Off by default: it discards whatever real phase structure a measurement had, which for a
    // single response is part of what was measured. See `nts::ir::minimumPhaseFromImpulse`.
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::cabIrMinPhaseA, 1 },
                                      "Cabinet A minimum phase", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::cabIrMinPhaseB, 1 },
                                      "Cabinet B minimum phase", false));
    /* Auto Match. Meta rather than an ordinary automatable parameter, and the distinction is
       load-bearing: this control decides *who writes the other parameters*, so a host automating
       it would be automating the ownership of forty-seven other lanes. Marking it meta says that
       to the host, and keeps it out of the lanes a user reaches for by accident. */
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::autoMatch, 1 },
                                      "Auto Match", false,
                                      juce::AudioParameterBoolAttributes {}.withMeta(true)));
    /* Live tracking, off by default and meta for the same reason as the switch above.

       Default off because a control that moves on its own is alarming until you know why it is
       moving, and this one moves the input trim -- the first thing a player reaches for when
       something sounds wrong. Opt-in, bounded to 6 dB either side of what the match set, and it
       touches nothing else. */
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::autoMatchTracking, 1 },
                                      "Auto Match tracking", false,
                                      juce::AudioParameterBoolAttributes {}.withMeta(true)));
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::tunerMute, 1 },
                                      "Mute while tuning", false,
                                      juce::AudioParameterBoolAttributes {}.withMeta(true)));
    // Concert A. 440 is the default and the standard; the range covers what people actually ask
    // for -- 432 and 435 downwards, 442 to 444 upwards for playing along with an orchestra or with
    // a record cut slightly fast. 0.5 Hz steps because the difference between 440 and 441 is about
    // four cents, which is inside what this tuner resolves, so whole-Hz steps would be coarser than
    // the instrument. Declared directly rather than through addFloat so it can be marked meta
    // alongside tunerMute: it changes a readout, not the audio.
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::tunerReference, 1 },
                                       "Tuner reference A", Range { 415.0f, 466.0f, 0.5f }, 440.0f,
                                       FloatAttributes {}.withLabel("Hz").withMeta(true)));

    // The pedalboard. Every slot defaults to None, so a project that predates this stage --
    // and a fresh instance -- is amplifier and cabinet alone, exactly as before.
    // Built from the model table rather than repeated here: the choice index *is* the model
    // index, so a pedal appended to the table must appear in this list at the same position.
    juce::StringArray pedalKinds;
    for (std::size_t model = 0; model < nts::pedals::modelCount(); ++model)
        pedalKinds.add(juce::String(std::string(nts::pedals::pedalModel(static_cast<int>(model)).name)));

    for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
    {
        const auto prefix = "Pedal " + juce::String(static_cast<int>(slot) + 1) + " ";
        const auto& ids = pedalParameterIds[slot];
        layout.add(std::make_unique<Choice>(juce::ParameterID { ids[0], 1 }, prefix + "kind",
                                            pedalKinds, 0));
        layout.add(std::make_unique<Bool>(juce::ParameterID { ids[1], 1 }, prefix + "bypass", false));
        addFloat(ids[2], (prefix + "drive").toRawUTF8(), Range { 0.0f, 10.0f, 0.01f }, 5.0f);
        addFloat(ids[3], (prefix + "tone").toRawUTF8(), Range { 0.0f, 10.0f, 0.01f }, 5.0f);
        addFloat(ids[4], (prefix + "level").toRawUTF8(), Range { -24.0f, 24.0f, 0.1f }, 0.0f, "dB");
        addFloat(ids[5], (prefix + "mix").toRawUTF8(), Range { 0.0f, 100.0f, 0.1f }, 100.0f, "%");
        // Generic names on purpose -- see PedalControl. A model that does not use these leaves
        // them at 5, which every engine reads as its neutral position.
        addFloat(ids[6], (prefix + "aux A").toRawUTF8(), Range { 0.0f, 10.0f, 0.01f }, 5.0f);
        addFloat(ids[7], (prefix + "aux B").toRawUTF8(), Range { 0.0f, 10.0f, 0.01f }, 5.0f);
    }
    return layout;
}

const char* TubeForgeAudioProcessor::pedalParameterId(std::size_t slot, PedalControl control) noexcept
{
    if (slot >= nts::pedals::slotCount) return ParameterIds::pedal1Kind;
    return pedalParameterIds[slot][static_cast<std::size_t>(control)];
}

const char* TubeForgeAudioProcessor::parameterId(Param id) noexcept
{
    return parameterIdList[static_cast<std::size_t>(id)];
}

float TubeForgeAudioProcessor::valueOf(const juce::AudioProcessorValueTreeState& state,
                                       const char* parameterId) noexcept
{
    if (const auto* value = state.getRawParameterValue(parameterId))
        return value->load(std::memory_order_relaxed);
    return 0.0f;
}

void TubeForgeAudioProcessor::setParameterValue(juce::AudioProcessorValueTreeState& state,
                                                const char* parameterId,
                                                float plainValue)
{
    if (auto* parameter = state.getParameter(parameterId))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
        parameter->endChangeGesture();
    }
}

nts::state::ProjectState TubeForgeAudioProcessor::makeProjectState() const
{
    nts::state::ProjectState state;
    state.engine.inputGainDb = valueOf(parameterState, ParameterIds::input);
    state.engine.outputGainDb = valueOf(parameterState, ParameterIds::output);
    state.engine.bypass = valueOf(parameterState, ParameterIds::bypass) >= 0.5f;
    state.engine.ampControls.reserve(ampControlIds.size());
    for (const auto* id : ampControlIds) state.engine.ampControls.push_back(valueOf(parameterState, id));
    state.device.sampleRate = currentSampleRate;
    state.device.bufferSize = currentBlockSize;
    state.graph.latencySamples = latencyBudget.processingSamples();
    state.graph.physicalCircuitJson = nts::circuit::serializeCircuit(circuitGraphSnapshot());
    {
        const std::scoped_lock lock(cabinetIrMutex);
        state.assets.cabinetIrPathA = cabinetIrSlots[0].file.getFullPathName().toStdString();
        state.assets.cabinetIrPathB = cabinetIrSlots[1].file.getFullPathName().toStdString();
        // Recorded at load rather than recomputed here: hashing a file on every save -- and
        // `getStateInformation` is called whenever a host feels like it -- would read from disk on
        // the message thread for a value that cannot have changed since the load.
        state.assets.cabinetIrHashA = cabinetIrSlots[0].digest;
        state.assets.cabinetIrHashB = cabinetIrSlots[1].digest;
    }
    static_assert(nts::state::maximumPedalSlots == nts::pedals::slotCount,
                  "the saved pedalboard and the engine's board must have the same number of slots");
    state.cabinet.controls.reserve(cabinetControlIds.size());
    for (const auto* id : cabinetControlIds)
        state.cabinet.controls.push_back(valueOf(parameterState, id));
    state.autoMatch = autoMatchProjectState();
    state.pedalboard.slots.reserve(nts::pedals::slotCount);
    {
        const std::scoped_lock lock(pedalModelMutex);
        for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
        {
            const auto parameters = currentPedalParameters(slot);
            state.pedalboard.slots.push_back({ parameters.model,
                                               parameters.bypassed, parameters.drive,
                                               parameters.tone, parameters.levelDb, parameters.mix,
                                               parameters.auxA, parameters.auxB,
                                               pedalModelSlots[slot].file.getFullPathName().toStdString() });
        }
    }
    return state;
}

bool TubeForgeAudioProcessor::applyProjectState(const nts::state::ProjectState& state)
{
    std::string validationError;
    if (! nts::state::validate(state, validationError))
        return false;

    /* A project recall is not a person turning knobs, and it replaces the whole rig.

       Both halves matter. Without the scope the Auto Match guard would see forty-seven change
       gestures and either raise a dialog or release every control; and `clearAutoMatchHold`
       afterwards is what stops the *previous* session's matched values from being guarded
       against a rig that is no longer on the amplifier. The `autoMatch` switch itself is
       restored from `ampControlIds` like every other saved control, so a project saved with
       Auto Match on comes back on -- and armed, waiting for a candidate. */
    const AutoWriteScope autoWrite(*this);
    setParameterValue(parameterState, ParameterIds::input, state.engine.inputGainDb);
    setParameterValue(parameterState, ParameterIds::output, state.engine.outputGainDb);
    setParameterValue(parameterState, ParameterIds::bypass, state.engine.bypass ? 1.0f : 0.0f);
    if (state.engine.ampControls.size() >= 25 && state.engine.ampControls.size() <= ampControlIds.size())
        for (std::size_t index = 0; index < state.engine.ampControls.size(); ++index)
            setParameterValue(parameterState, ampControlIds[index], state.engine.ampControls[index]);
    /* The cabinet block, restored positionally against its own list.

       No lower bound, unlike the amp controls above: an *empty* list is the normal and correct
       state for every project written before schema 6, and it must leave these controls at their
       defaults rather than being treated as a short read. Those defaults are the values the
       fields held while they were unreachable, so an old project restores the cabinet it
       actually described. A list longer than the current table is refused outright, because it
       came from a build that knows controls this one does not. */
    if (state.cabinet.controls.size() <= cabinetControlIds.size())
        for (std::size_t index = 0; index < state.cabinet.controls.size(); ++index)
            setParameterValue(parameterState, cabinetControlIds[index], state.cabinet.controls[index]);
    /* A project written before the cabinet model existed keeps the cabinet it was voiced against.

       An empty block is exactly the set of projects that predate schema 6, and those were mixed
       against the two synthetic decays that used to be the built-in responses. The model's
       defaults are a real cabinet -- which is right for a fresh instance and wrong for somebody's
       finished track -- so those projects are put back on Legacy here.

       Written after the positional restore above so it cannot be undone by it, and deliberately
       *not* conditional on the slot having a user response: a slot with a file loaded ignores its
       model setting entirely, and would then find Legacy waiting if the file were ever cleared,
       which is the same cabinet it would have had. */
    if (state.cabinet.controls.empty())
    {
        const auto legacy = static_cast<float>(nts::ir::CabinetKind::legacy);
        setParameterValue(parameterState, ParameterIds::cabModelA, legacy);
        setParameterValue(parameterState, ParameterIds::cabModelB, legacy);
    }
    if (! state.graph.physicalCircuitJson.empty())
    {
        nts::circuit::CircuitGraphDescription restored;
        std::string error;
        if (! nts::circuit::deserializeCircuit(state.graph.physicalCircuitJson, restored, error)) return false;
        {
            const std::scoped_lock lock(circuitGraphMutex);
            desiredCircuitGraph = restored;
        }
        physicalCircuitControlHash.store(circuitHash(restored), std::memory_order_release);
        physicalCircuitRequestedHash.store(0, std::memory_order_release);
        if (currentBlockSize > 0)
        {
            const auto report = physicalCircuit.stageGraph(restored);
            if (! report.isValid()) return false;
        }
    }
    restoreCabinetIrPaths(state.assets.cabinetIrPathA, state.assets.cabinetIrPathB,
                          state.assets.cabinetIrHashA, state.assets.cabinetIrHashB);
    /* Re-render the built-in model against what this project just asked for.

       Directly rather than through the hash the audio thread watches, because a project is most
       often recalled with the transport stopped -- and a cabinet that only appears once somebody
       presses play is a cabinet that looks broken. Slots holding a user response are skipped by
       `refreshCabinetResponses` itself, so this cannot undo the restore above. */
    refreshCabinetResponses();
    restorePedalboard(state.pedalboard);
    // Last, and outside nothing: every parameter this project describes has been written by now,
    // so restoring the hold cannot be undone by a later write in this same function. A project
    // that was not holding -- including every project written before schema 5 -- clears it.
    restoreAutoMatchProjectState(state.autoMatch);
    return true;
}

void TubeForgeAudioProcessor::restorePedalboard(const nts::state::PedalboardState& board)
{
    // A project written before schema 4 has no board at all. Leaving the parameters alone
    // then is the point: they are already at None, which is the rig that project described.
    const auto count = std::min(board.slots.size(), nts::pedals::slotCount);
    for (std::size_t slot = 0; slot < count; ++slot)
    {
        const auto& saved = board.slots[slot];
        const auto set = [this, slot](PedalControl control, float value)
        { setParameterValue(parameterState, pedalParameterId(slot, control), value); };
        // Clamped rather than trusted: validation lets a kind through that a later build may
        // have added, and selecting a choice index the parameter does not have would leave it
        // wherever it happened to be.
        set(PedalControl::kind, static_cast<float>(std::clamp(
            saved.kind, 0, static_cast<int>(nts::pedals::modelCount()) - 1)));
        set(PedalControl::bypass, saved.bypassed ? 1.0f : 0.0f);
        set(PedalControl::drive, saved.drive);
        set(PedalControl::tone, saved.tone);
        set(PedalControl::level, saved.levelDb);
        set(PedalControl::mix, saved.mix);
        set(PedalControl::auxA, saved.auxA);
        set(PedalControl::auxB, saved.auxB);

        if (saved.modelPath.empty()) continue;
        const juce::File artifact(juce::String::fromUTF8(saved.modelPath.c_str()));
        if (artifact.isDirectory()) { requestPedalModelLoad(static_cast<int>(slot), artifact); continue; }
        // The capture is gone, so say so and keep the path. A project reopened on a machine
        // that has not synced its capture folder yet should not silently destroy the
        // reference the next time it is saved -- the same rule the cabinet slots follow.
        const std::scoped_lock lock(pedalModelMutex);
        pedalModelSlots[slot].file = artifact;
        pedalModelSlots[slot].status = "Missing: " + artifact.getFileName().toStdString();
    }
}
