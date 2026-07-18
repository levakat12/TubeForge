#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <nts/diagnostics/ProcessMemory.h>

#include <filesystem>
#include <string_view>

namespace ParameterIds
{
constexpr auto input = "input";
constexpr auto output = "output";
constexpr auto bypass = "bypass";
} // namespace ParameterIds

namespace
{
std::filesystem::path logPath()
{
    return std::filesystem::path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                     .getChildFile("TubeForge")
                                     .getChildFile("logs")
                                     .getChildFile("tubeforge.jsonl")
                                     .getFullPathName()
                                     .toStdString());
}
} // namespace

TubeForgeAudioProcessor::TubeForgeAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameterState(*this, nullptr, "TubeForgeParameters", createParameterLayout()),
      engine(runtimeParameters, meters),
      logger(logPath(), &diagnostics)
{
    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "runtime", "processor-created", "TubeForge processor created", modeName().toStdString() });
}

void TubeForgeAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    absoluteSamplePosition = 0;
    engine.prepare(sampleRate,
                   static_cast<std::size_t>(samplesPerBlock),
                   static_cast<std::size_t>(getTotalNumInputChannels()),
                   static_cast<std::size_t>(getTotalNumOutputChannels()));
    diagnostics.prepare(sampleRate, static_cast<std::uint32_t>(samplesPerBlock));
    latencyBudget.hostSamples = samplesPerBlock;
    diagnostics.setLatencyBudget(latencyBudget);
    updateReportedLatency();

    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "audio", "prepared", "Audio engine prepared",
                 "sampleRate=" + std::to_string(sampleRate) + ",blockSize=" + std::to_string(samplesPerBlock) });
}

void TubeForgeAudioProcessor::releaseResources()
{
    engine.reset();
}

bool TubeForgeAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    const auto supportedChannelCount = output == juce::AudioChannelSet::mono()
                                    || output == juce::AudioChannelSet::stereo();
    return supportedChannelCount && input == output;
}

void TubeForgeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    juce::ScopedNoDenormals noDenormals;
    const auto startedAt = diagnostics.beginCallback();

    runtimeParameters.inputGainDb.store(valueOf(parameterState, ParameterIds::input), std::memory_order_relaxed);
    runtimeParameters.outputGainDb.store(valueOf(parameterState, ParameterIds::output), std::memory_order_relaxed);
    runtimeParameters.bypass.store(valueOf(parameterState, ParameterIds::bypass) >= 0.5f, std::memory_order_relaxed);

    constexpr auto maximumChannels = nts::audio::MeterState::maximumChannels;
    std::array<const float*, maximumChannels> inputs {};
    std::array<float*, maximumChannels> outputs {};
    const auto inputCount = std::min(getTotalNumInputChannels(), static_cast<int>(maximumChannels));
    const auto outputCount = std::min(getTotalNumOutputChannels(), static_cast<int>(maximumChannels));

    for (int channel = 0; channel < inputCount; ++channel)
        inputs[static_cast<std::size_t>(channel)] = buffer.getReadPointer(channel);
    for (int channel = 0; channel < outputCount; ++channel)
        outputs[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);

    nts::audio::AudioProcessContext context {
        std::span<const float* const>(inputs.data(), static_cast<std::size_t>(inputCount)),
        std::span<float* const>(outputs.data(), static_cast<std::size_t>(outputCount)),
        static_cast<std::size_t>(buffer.getNumSamples()),
        currentSampleRate,
        absoluteSamplePosition
    };

    diagnostics.verifySampleRate(context.sampleRate, absoluteSamplePosition);
    engine.process(context);
    diagnostics.endCallback(startedAt, static_cast<std::uint32_t>(buffer.getNumSamples()), absoluteSamplePosition);
    absoluteSamplePosition += static_cast<std::uint64_t>(buffer.getNumSamples());
}

juce::AudioProcessorEditor* TubeForgeAudioProcessor::createEditor()
{
    return new TubeForgeAudioProcessorEditor(*this);
}

bool TubeForgeAudioProcessor::hasEditor() const { return true; }
const juce::String TubeForgeAudioProcessor::getName() const { return JucePlugin_Name; }
bool TubeForgeAudioProcessor::acceptsMidi() const { return false; }
bool TubeForgeAudioProcessor::producesMidi() const { return false; }
bool TubeForgeAudioProcessor::isMidiEffect() const { return false; }
double TubeForgeAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int TubeForgeAudioProcessor::getNumPrograms() { return 1; }
int TubeForgeAudioProcessor::getCurrentProgram() { return 0; }
void TubeForgeAudioProcessor::setCurrentProgram(int index) { juce::ignoreUnused(index); }
const juce::String TubeForgeAudioProcessor::getProgramName(int index) { juce::ignoreUnused(index); return {}; }
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

juce::AudioProcessorValueTreeState::ParameterLayout TubeForgeAudioProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Bool = juce::AudioParameterBool;
    using Range = juce::NormalisableRange<float>;
    using FloatAttributes = juce::AudioParameterFloatAttributes;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::input, 1 }, "Input",
                                       Range { -60.0f, 24.0f, 0.1f }, 0.0f,
                                       FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Float>(juce::ParameterID { ParameterIds::output, 1 }, "Output",
                                       Range { -60.0f, 24.0f, 0.1f }, 0.0f,
                                       FloatAttributes {}.withLabel("dB")));
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::bypass, 1 }, "Bypass", false));
    return layout;
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
    state.device.sampleRate = currentSampleRate;
    state.device.bufferSize = currentBlockSize;
    state.graph.latencySamples = latencyBudget.processingSamples();
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
    return true;
}

void TubeForgeAudioProcessor::updateReportedLatency()
{
    const auto processingLatency = latencyBudget.processingSamples();
    if (getLatencySamples() != processingLatency)
        setLatencySamples(processingLatency);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TubeForgeAudioProcessor();
}
