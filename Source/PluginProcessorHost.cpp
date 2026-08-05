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
    setParameterValue(parameterState, ParameterIds::instrument, static_cast<float>(index / topologies));
    setParameterValue(parameterState, ParameterIds::topology, static_cast<float>(index % topologies));
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

void TubeForgeAudioProcessor::applyRecoveredRig(const nts::amp::AmpParameters& p,
                                                bool isolateChain)
{
    // Reached through a callback from StudioServices rather than called by it directly, so the
    // dependency runs one way: the processor knows about the studio, not the reverse.
    setParameterValue(parameterState, ParameterIds::engineMode, 0.0f);
    setParameterValue(parameterState, ParameterIds::instrument,
                      p.instrument == nts::amp::Instrument::bass ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::topology,
                      static_cast<float>(static_cast<int>(p.topology)));
    setParameterValue(parameterState, ParameterIds::input, p.manualInputTrimDb);
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
                      static_cast<float>(p.cabinet.delaySamplesB));

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
}

juce::StringArray TubeForgeAudioProcessor::reconstructionApplyWarnings() const
{
    if (! appliedRecoveredRig) return {};
    const auto& p = *appliedRecoveredRig;

    juce::StringArray warnings;

    static constexpr std::array pedalNames { "empty", "boost", "overdrive", "distortion",
                                             "fuzz", "compressor", "neural capture" };
    static_assert(pedalNames.size() == nts::pedals::kindCount,
                  "every pedal kind needs a name for the chain warnings to stay truthful");
    for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
    {
        // A slot at zero mix is exactly transparent whatever else it is set to, so it is
        // not worth warning about.
        const auto pedal = currentPedalParameters(slot);
        if (pedal.kind == nts::pedals::PedalKind::none || pedal.bypassed || pedal.mix <= 0.5f)
            continue;
        warnings.add("Pedal " + juce::String(slot + 1) + " ("
            + pedalNames[static_cast<std::size_t>(pedal.kind)]
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
    addFloat(ParameterIds::crossover, "Bass crossover", Range { 60.0f, 500.0f, 1.0f, 0.5f }, 180.0f, "Hz");
    addFloat(ParameterIds::cleanBlend, "Bass clean blend", Range { 0.0f, 100.0f, 0.1f }, 55.0f, "%");
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
    const juce::StringArray pedalKinds { "None", "Boost", "Overdrive", "Distortion", "Fuzz",
                                         "Compressor", "Neural capture" };
    static_assert(nts::pedals::kindCount == 7,
                  "the pedal kind list and the PedalKind enumeration must stay the same length");
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
    }
    static_assert(nts::state::maximumPedalSlots == nts::pedals::slotCount,
                  "the saved pedalboard and the engine's board must have the same number of slots");
    state.pedalboard.slots.reserve(nts::pedals::slotCount);
    {
        const std::scoped_lock lock(pedalModelMutex);
        for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
        {
            const auto parameters = currentPedalParameters(slot);
            state.pedalboard.slots.push_back({ static_cast<int>(parameters.kind),
                                               parameters.bypassed, parameters.drive,
                                               parameters.tone, parameters.levelDb, parameters.mix,
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

    setParameterValue(parameterState, ParameterIds::input, state.engine.inputGainDb);
    setParameterValue(parameterState, ParameterIds::output, state.engine.outputGainDb);
    setParameterValue(parameterState, ParameterIds::bypass, state.engine.bypass ? 1.0f : 0.0f);
    if (state.engine.ampControls.size() >= 25 && state.engine.ampControls.size() <= ampControlIds.size())
        for (std::size_t index = 0; index < state.engine.ampControls.size(); ++index)
            setParameterValue(parameterState, ampControlIds[index], state.engine.ampControls[index]);
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
    restoreCabinetIrPaths(state.assets.cabinetIrPathA, state.assets.cabinetIrPathB);
    restorePedalboard(state.pedalboard);
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
            saved.kind, 0, static_cast<int>(nts::pedals::kindCount) - 1)));
        set(PedalControl::bypass, saved.bypassed ? 1.0f : 0.0f);
        set(PedalControl::drive, saved.drive);
        set(PedalControl::tone, saved.tone);
        set(PedalControl::level, saved.levelDb);
        set(PedalControl::mix, saved.mix);

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
