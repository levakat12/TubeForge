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
    const auto instrument = std::clamp(static_cast<int>(std::lround(parameterOf(Param::instrument))), 0, 1);
    const auto topology = std::clamp(static_cast<int>(std::lround(parameterOf(Param::topology))), 0, 1);
    return instrument * 2 + topology;
}

void TubeForgeAudioProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= factoryProgramCount) return;
    // Through the parameters rather than into the amplifier, so the change is automatable,
    // saved with the project, and picked up by the traditional amp's existing preset
    // crossfade rather than switching under the signal.
    setParameterValue(parameterState, ParameterIds::instrument, static_cast<float>(index / 2));
    setParameterValue(parameterState, ParameterIds::topology, static_cast<float>(index % 2));
}

const juce::String TubeForgeAudioProcessor::getProgramName(int index)
{
    if (index < 0 || index >= factoryProgramCount) return {};
    static constexpr std::array names {
        "Tight Modern Guitar", "Vintage Bloom Guitar",
        "Tight Modern Bass", "Vintage Bloom Bass"
    };
    return names[static_cast<std::size_t>(index)];
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

void TubeForgeAudioProcessor::applyRecoveredRig(const nts::amp::AmpParameters& p)
{
    // Reached through a callback from StudioServices rather than called by it directly, so the
    // dependency runs one way: the processor knows about the studio, not the reverse.
    setParameterValue(parameterState, ParameterIds::engineMode, 0.0f);
    setParameterValue(parameterState, ParameterIds::instrument,
                      p.instrument == nts::amp::Instrument::bass ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::topology,
                      p.topology == nts::amp::Topology::vintageBloom ? 1.0f : 0.0f);
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
    setParameterValue(parameterState, ParameterIds::sag, p.powerAmp.sag * 100.0f);
    setParameterValue(parameterState, ParameterIds::feedback, p.powerAmp.feedback * 100.0f);
    setParameterValue(parameterState, ParameterIds::crossover, p.bass.crossoverHz);
    setParameterValue(parameterState, ParameterIds::cleanBlend, p.bass.cleanBlend * 100.0f);
    const std::array stageIds { ParameterIds::stage1, ParameterIds::stage2,
                                ParameterIds::stage3, ParameterIds::stage4 };
    for (std::size_t stage = 0; stage < stageIds.size(); ++stage)
        setParameterValue(parameterState, stageIds[stage], p.stages[stage].driveDb);
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
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::topology, 1 }, "Topology",
                                        juce::StringArray { "Tight Modern", "Vintage Bloom" }, 0));
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
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::gateEnabled, 1 },
                                      "Noise gate", true));
    addFloat(ParameterIds::gateThreshold, "Gate threshold", Range { -90.0f, -20.0f, 0.1f }, -58.0f, "dB");
    // Depth rather than a fixed full mute: a shallow gate ducks hum between
    // phrases without swallowing the tail of a note.
    addFloat(ParameterIds::gateDepth, "Gate depth", Range { -90.0f, 0.0f, 0.1f }, -80.0f, "dB");
    addFloat(ParameterIds::gateAttack, "Gate attack", Range { 0.1f, 50.0f, 0.1f }, 2.0f, "ms");
    addFloat(ParameterIds::gateHold, "Gate hold", Range { 0.0f, 500.0f, 1.0f }, 60.0f, "ms");
    addFloat(ParameterIds::gateRelease, "Gate release", Range { 5.0f, 2000.0f, 1.0f }, 250.0f, "ms");
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
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::tunerMute, 1 },
                                      "Mute while tuning", false,
                                      juce::AudioParameterBoolAttributes {}.withMeta(true)));
    return layout;
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
    return true;
}
