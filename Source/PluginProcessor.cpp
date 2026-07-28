#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <TubeForgeAssets.h>

#include <nts/diagnostics/ProcessMemory.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>

namespace ParameterIds
{
constexpr auto input = "input";
constexpr auto output = "output";
constexpr auto bypass = "bypass";
constexpr auto gain = "gain";
constexpr auto bass = "bass";
constexpr auto mid = "mid";
constexpr auto treble = "treble";
constexpr auto presence = "presence";
constexpr auto resonance = "resonance";
constexpr auto master = "master";
constexpr auto cabinet = "cabinet";
constexpr auto instrument = "instrument";
constexpr auto topology = "topology";
constexpr auto stage1 = "stage1";
constexpr auto stage2 = "stage2";
constexpr auto stage3 = "stage3";
constexpr auto stage4 = "stage4";
constexpr auto bias = "bias";
constexpr auto lowCut = "lowCut";
constexpr auto highCut = "highCut";
constexpr auto oversampling = "oversampling";
constexpr auto sag = "sag";
constexpr auto feedback = "feedback";
constexpr auto crossover = "crossover";
constexpr auto cleanBlend = "cleanBlend";
constexpr auto cabinetAlignment = "cabinetAlignment";
constexpr auto tightness = "tightness";
constexpr auto pickEmphasis = "pickEmphasis";
constexpr auto engineMode = "engineMode";
constexpr auto neuralMonitor = "neuralMonitor";
constexpr auto neuralCompensation = "neuralCompensation";
constexpr auto circuitPreampTube = "circuitPreampTube";
constexpr auto circuitPowerTube = "circuitPowerTube";
constexpr auto circuitPowerTopology = "circuitPowerTopology";
constexpr auto circuitToneStack = "circuitToneStack";
constexpr auto circuitBackend = "circuitBackend";
constexpr auto circuitCabinetStyle = "circuitCabinetStyle";
} // namespace ParameterIds

namespace
{
constexpr std::array ampControlIds {
    ParameterIds::gain, ParameterIds::bass, ParameterIds::mid, ParameterIds::treble,
    ParameterIds::presence, ParameterIds::resonance, ParameterIds::master, ParameterIds::cabinet,
    ParameterIds::instrument, ParameterIds::topology, ParameterIds::stage1, ParameterIds::stage2,
    ParameterIds::stage3, ParameterIds::stage4, ParameterIds::bias, ParameterIds::lowCut,
    ParameterIds::highCut, ParameterIds::oversampling, ParameterIds::sag, ParameterIds::feedback,
    ParameterIds::crossover, ParameterIds::cleanBlend, ParameterIds::cabinetAlignment,
    ParameterIds::tightness, ParameterIds::pickEmphasis, ParameterIds::engineMode,
    ParameterIds::neuralMonitor, ParameterIds::neuralCompensation,
    ParameterIds::circuitPreampTube, ParameterIds::circuitPowerTube,
    ParameterIds::circuitPowerTopology, ParameterIds::circuitToneStack,
    ParameterIds::circuitBackend, ParameterIds::circuitCabinetStyle
};

void setCircuitParameter(nts::circuit::NodeSpec& node, std::string_view id, float value)
{
    const auto found = std::find_if(node.parameters.begin(), node.parameters.end(), [id](const auto& parameter)
    { return parameter.id == id; });
    if (found != node.parameters.end()) found->value = value;
    else node.parameters.push_back({ std::string(id), value });
}

std::uint64_t circuitHash(const nts::circuit::CircuitGraphDescription& graph)
{
    const auto json = nts::circuit::serializeCircuit(graph);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : json) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ULL; }
    return hash;
}

std::filesystem::path logPath()
{
    return std::filesystem::path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                     .getChildFile("TubeForge")
                                     .getChildFile("logs")
                                     .getChildFile("tubeforge.jsonl")
                                     .getFullPathName()
                                     .toStdString());
}

juce::File assistantPreferencesFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("assistant-preferences.json");
}

std::filesystem::path tonePackageLibraryPath()
{
    return std::filesystem::path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("profiles").getFullPathName().toStdString());
}

struct BufferMeasurement
{
    float peak {}, rms {}, zeroCrossingRate {}, correlation { 1.0f };
    std::uint32_t clipped {};
};

BufferMeasurement measureBuffer(const juce::AudioBuffer<float>& buffer) noexcept
{
    BufferMeasurement result;
    const auto channels = std::min(2, buffer.getNumChannels());
    const auto samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0) return result;
    double energy = 0.0, cross = 0.0, leftEnergy = 0.0, rightEnergy = 0.0;
    std::uint32_t crossings {};
    for (int sample = 0; sample < samples; ++sample)
    {
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto value = buffer.getSample(channel, sample);
            result.peak = std::max(result.peak, std::abs(value)); energy += value * value;
            if (std::abs(value) >= 0.999f) ++result.clipped;
        }
        const auto left = buffer.getSample(0, sample);
        if (sample > 0 && std::signbit(left) != std::signbit(buffer.getSample(0, sample - 1))) ++crossings;
        if (channels > 1)
        {
            const auto right = buffer.getSample(1, sample);
            cross += left * right; leftEnergy += left * left; rightEnergy += right * right;
        }
    }
    result.rms = std::sqrt(static_cast<float>(energy / static_cast<double>(samples * channels)));
    result.zeroCrossingRate = static_cast<float>(crossings) / static_cast<float>(samples);
    if (channels > 1) result.correlation = static_cast<float>(cross / std::sqrt(std::max(1.0e-18, leftEnergy * rightEnergy)));
    return result;
}

bool decodeStemFile(const juce::File& file, double targetSampleRate, std::size_t targetSamples,
                    nts::reconstruction::StereoAudio& result, std::string& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0
        || reader->lengthInSamples > std::numeric_limits<int>::max()
        || targetSamples > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        error = "Could not decode neural stem: " + file.getFileName().toStdString();
        return false;
    }
    const auto sourceSamples = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> decoded(static_cast<int>(std::min<unsigned int>(2, reader->numChannels)), sourceSamples);
    if (! reader->read(&decoded, 0, sourceSamples, 0, true, reader->numChannels > 1))
    {
        error = "Could not read neural stem: " + file.getFileName().toStdString();
        return false;
    }
    result.sampleRate = targetSampleRate;
    result.left.assign(targetSamples, 0.0f);
    result.right.assign(targetSamples, 0.0f);
    const auto outputSamples = static_cast<int>(targetSamples);
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto sourceChannel = std::min(channel, decoded.getNumChannels() - 1);
        auto& destination = channel == 0 ? result.left : result.right;
        if (std::abs(reader->sampleRate - targetSampleRate) < 0.01)
        {
            const auto copyCount = std::min(sourceSamples, outputSamples);
            std::copy_n(decoded.getReadPointer(sourceChannel), copyCount, destination.begin());
        }
        else
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.process(reader->sampleRate / targetSampleRate,
                                 decoded.getReadPointer(sourceChannel), destination.data(),
                                 outputSamples, sourceSamples, 0);
        }
    }
    return true;
}
} // namespace

TubeForgeAudioProcessor::~TubeForgeAudioProcessor()
{
    if (neuralLoader.joinable()) neuralLoader.request_stop();
    if (circuitCompiler.joinable()) circuitCompiler.request_stop();
    if (toneAnalysisWorker.joinable()) toneAnalysisWorker.request_stop();
    if (reconstructionWorker.joinable()) reconstructionWorker.request_stop();
}

TubeForgeAudioProcessor::TubeForgeAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameterState(*this, nullptr, "TubeForgeParameters", createParameterLayout()),
      engine(runtimeParameters, meters),
      logger(logPath(), &diagnostics),
      tonePackageLibrary(tonePackageLibraryPath())
{
    ampFaceplateImage = juce::ImageFileFormat::loadFrom(
        TubeForgeAssets::tubeforge_amp_faceplate_png,
        TubeForgeAssets::tubeforge_amp_faceplate_pngSize);
    factoryAmpParameters[0] = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern,
                                                            nts::amp::Instrument::guitar).parameters;
    factoryAmpParameters[1] = nts::amp::makeOriginalPreset(nts::amp::Topology::vintageBloom,
                                                            nts::amp::Instrument::guitar).parameters;
    factoryAmpParameters[2] = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern,
                                                            nts::amp::Instrument::bass).parameters;
    factoryAmpParameters[3] = nts::amp::makeOriginalPreset(nts::amp::Topology::vintageBloom,
                                                            nts::amp::Instrument::bass).parameters;
    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "runtime", "processor-created", "TubeForge processor created", modeName().toStdString() });
    const auto preferencesFile = assistantPreferencesFile();
    if (preferencesFile.existsAsFile())
    {
        std::string preferenceError;
        if (const auto restored = nts::assistant::deserializePreferences(
                preferencesFile.loadFileAsString().toStdString(), preferenceError))
            assistantPreferences = *restored;
    }
    std::string packageError;
    if (! tonePackageLibrary.refresh(packageError))
        logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::warning,
                     "ecosystem", "profile-library", "Profile library unavailable", packageError });
}

void TubeForgeAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    if (circuitCompiler.joinable())
    {
        circuitCompiler.request_stop();
        circuitCompiler.join();
    }
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    absoluteSamplePosition = 0;
    inputSnapshot.setSize(std::max(1, getTotalNumInputChannels()), std::max(1, samplesPerBlock),
                          false, true, false);
    engine.prepare(sampleRate,
                   static_cast<std::size_t>(samplesPerBlock),
                   static_cast<std::size_t>(getTotalNumInputChannels()),
                   static_cast<std::size_t>(getTotalNumOutputChannels()));
    traditionalAmp.prepare({ sampleRate, static_cast<std::size_t>(samplesPerBlock),
                              static_cast<std::size_t>(getTotalNumOutputChannels()) });
    traditionalAmp.setParametersImmediately(currentAmpParameters());
    physicalCircuit.prepare({ sampleRate, static_cast<std::size_t>(samplesPerBlock),
                              static_cast<std::size_t>(getTotalNumOutputChannels()) });
    physicalCircuitControlHash.store(0, std::memory_order_relaxed);
    physicalCircuitRequestedHash.store(0, std::memory_order_relaxed);
    refreshPhysicalCircuit();
    neuralAmp.prepare(sampleRate, static_cast<std::size_t>(samplesPerBlock),
                      static_cast<std::size_t>(getTotalNumOutputChannels()));
    diagnostics.prepare(sampleRate, static_cast<std::uint32_t>(samplesPerBlock));
    latencyBudget.hostSamples = samplesPerBlock;
    latencyBudget.oversamplingSamples = static_cast<int>(traditionalAmp.latencySamples());
    pendingOversamplingLatencySamples.store(latencyBudget.oversamplingSamples,
                                            std::memory_order_relaxed);
    diagnostics.setLatencyBudget(latencyBudget);
    updateReportedLatency();

    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "audio", "prepared", "Audio engine prepared",
                 "sampleRate=" + std::to_string(sampleRate) + ",blockSize=" + std::to_string(samplesPerBlock) });
}

void TubeForgeAudioProcessor::releaseResources()
{
    if (circuitCompiler.joinable())
    {
        circuitCompiler.request_stop();
        circuitCompiler.join();
    }
    cancelPendingUpdate();
    engine.reset();
    traditionalAmp.reset();
    physicalCircuit.reset();
    neuralAmp.reset();
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
    const auto assistantInputMeasurement = measureBuffer(buffer);

    runtimeParameters.inputGainDb.store(0.0f, std::memory_order_relaxed);
    runtimeParameters.outputGainDb.store(valueOf(parameterState, ParameterIds::output), std::memory_order_relaxed);
    runtimeParameters.bypass.store(valueOf(parameterState, ParameterIds::bypass) >= 0.5f, std::memory_order_relaxed);

    constexpr auto maximumChannels = nts::audio::MeterState::maximumChannels;
    std::array<const float*, maximumChannels> inputs {};
    std::array<float*, maximumChannels> outputs {};
    const auto inputCount = std::min(getTotalNumInputChannels(), static_cast<int>(maximumChannels));
    const auto outputCount = std::min(getTotalNumOutputChannels(), static_cast<int>(maximumChannels));

    // Snapshot the input before the amplifier overwrites the buffer in place. The
    // engine still processes the buffer itself, so the audio path is unchanged;
    // the copy exists only to recover a true input level for metering below.
    const auto snapshotChannels = std::min(inputCount, inputSnapshot.getNumChannels());
    const auto snapshotSamples = std::min(buffer.getNumSamples(), inputSnapshot.getNumSamples());
    for (int channel = 0; channel < snapshotChannels; ++channel)
        inputSnapshot.copyFrom(channel, 0, buffer, channel, 0, snapshotSamples);

    for (int channel = 0; channel < inputCount; ++channel)
        inputs[static_cast<std::size_t>(channel)] = buffer.getReadPointer(channel);
    for (int channel = 0; channel < outputCount; ++channel)
        outputs[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);

    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition())
        {
            const auto playing = position->getIsPlaying();
            if (const auto time = position->getTimeInSamples())
            {
                if ((expectedNextHostSample >= 0 && *time != expectedNextHostSample)
                    || (playing && ! previousTransportPlaying))
                    neuralAmp.requestStateReset();
                expectedNextHostSample = *time + buffer.getNumSamples();
            }
            previousTransportPlaying = playing;
        }
    }

    const auto engineMode = static_cast<int>(std::lround(valueOf(parameterState, ParameterIds::engineMode)));
    if (engineMode != 2) physicalCircuit.publishPendingWithoutCrossfade();
    if (valueOf(parameterState, ParameterIds::bypass) < 0.5f)
    {
        if (engineMode == 1)
        {
            const std::array controls {
                std::clamp(valueOf(parameterState, ParameterIds::gain) * 0.2f - 1.0f, -1.0f, 1.0f),
                std::clamp((valueOf(parameterState, ParameterIds::bass)
                            + valueOf(parameterState, ParameterIds::mid)
                            + valueOf(parameterState, ParameterIds::treble)) / 15.0f - 1.0f, -1.0f, 1.0f),
                std::clamp((valueOf(parameterState, ParameterIds::master) + 24.0f) / 36.0f - 1.0f, -1.0f, 1.0f),
                std::clamp(valueOf(parameterState, ParameterIds::topology) * 2.0f - 1.0f, -1.0f, 1.0f),
                std::clamp(valueOf(parameterState, ParameterIds::instrument) * 2.0f - 1.0f, -1.0f, 1.0f)
            };
            neuralAmp.setControls(controls);
            neuralAmp.setMonitorMode(static_cast<nts::ml::NeuralMonitorMode>(std::clamp(
                static_cast<int>(valueOf(parameterState, ParameterIds::neuralMonitor)), 0, 2)));
            neuralAmp.setInputCompensationEnabled(
                valueOf(parameterState, ParameterIds::neuralCompensation) >= 0.5f);
            neuralAmp.process(outputs.data(), static_cast<std::size_t>(outputCount),
                              static_cast<std::size_t>(buffer.getNumSamples()));
        }
        else if (engineMode == 2)
        {
            physicalCircuit.process(outputs.data(), static_cast<std::size_t>(outputCount),
                                    static_cast<std::size_t>(buffer.getNumSamples()));
        }
        else
        {
            const auto ampParameters = currentAmpParameters();
            traditionalAmp.setParameters(ampParameters);
            traditionalAmp.process(outputs.data(), static_cast<std::size_t>(outputCount),
                                   static_cast<std::size_t>(buffer.getNumSamples()));
        }
    }
    refreshProcessingLatency(true);

    nts::audio::AudioProcessContext context {
        std::span<const float* const>(inputs.data(), static_cast<std::size_t>(inputCount)),
        std::span<float* const>(outputs.data(), static_cast<std::size_t>(outputCount)),
        static_cast<std::size_t>(buffer.getNumSamples()),
        currentSampleRate,
        absoluteSamplePosition
    };

    diagnostics.verifySampleRate(context.sampleRate, absoluteSamplePosition);
    engine.process(context);

    // The engine meters the buffer it was handed, which the amplifier has already
    // written to, so its input peaks are really post-amplifier levels. Republish
    // them from the untouched snapshot and keep the output peaks it just measured.
    for (int channel = 0; channel < snapshotChannels; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        meters.publish(index, inputSnapshot.getMagnitude(channel, 0, snapshotSamples),
                       meters.outputPeak(index));
    }

    diagnostics.endCallback(startedAt, static_cast<std::uint32_t>(buffer.getNumSamples()), absoluteSamplePosition);
    const auto assistantOutputMeasurement = measureBuffer(buffer);
    nts::assistant::AudioSummaryFrame assistantFrame;
    assistantFrame.inputPeak = assistantInputMeasurement.peak;
    assistantFrame.inputRms = assistantInputMeasurement.rms;
    assistantFrame.outputPeak = assistantOutputMeasurement.peak;
    assistantFrame.outputRms = assistantOutputMeasurement.rms;
    assistantFrame.zeroCrossingRate = assistantInputMeasurement.zeroCrossingRate;
    assistantFrame.channelCorrelation = assistantOutputMeasurement.correlation;
    assistantFrame.inputClipped = assistantInputMeasurement.clipped;
    assistantFrame.outputClipped = assistantOutputMeasurement.clipped;
    assistantFrame.samples = static_cast<std::uint32_t>(buffer.getNumSamples());
    assistantFrame.latencySamples = pendingOversamplingLatencySamples.load(std::memory_order_relaxed);
    assistantFrame.sampleRate = currentSampleRate;
    (void) assistantSummaryQueue.push(assistantFrame);
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

void TubeForgeAudioProcessor::requestNeuralModelLoad(const juce::File& artifactDirectory)
{
    if (neuralLoader.joinable()) neuralLoader.request_stop();
    neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::loading, std::memory_order_relaxed);
    diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::loading);
    {
        const std::scoped_lock lock(neuralStatusMutex);
        neuralStatusDetail = "Loading " + artifactDirectory.getFileName().toStdString();
    }
    neuralLoader = std::jthread([this, artifactDirectory](std::stop_token stopToken)
    {
        loadNeuralArtifact(stopToken, artifactDirectory);
    });
}

void TubeForgeAudioProcessor::loadNeuralArtifact(std::stop_token stopToken,
                                                 juce::File artifactDirectory)
{
    const auto fail = [this](std::string message)
    {
        {
            const std::scoped_lock lock(neuralStatusMutex);
            neuralStatusDetail = std::move(message);
        }
        neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::failed, std::memory_order_relaxed);
        diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::failed);
    };
    const auto modelFile = artifactDirectory.getChildFile("model.bin");
    const auto inputFile = artifactDirectory.getChildFile("test-vectors").getChildFile("input.f32");
    const auto outputFile = artifactDirectory.getChildFile("test-vectors").getChildFile("output.f32");
    const auto metadataFile = artifactDirectory.getChildFile("test-vectors").getChildFile("metadata.json");
    const auto normalizationFile = artifactDirectory.getChildFile("normalization.json");
    const auto manifestFile = artifactDirectory.getChildFile("manifest.json");
    if (! modelFile.existsAsFile() || ! inputFile.existsAsFile() || ! outputFile.existsAsFile()
        || ! metadataFile.existsAsFile() || ! normalizationFile.existsAsFile() || ! manifestFile.existsAsFile())
    { fail("Model artifact is incomplete"); return; }
    juce::var manifest;
    if (juce::JSON::parse(manifestFile.loadFileAsString(), manifest).failed() || ! manifest.isObject())
    { fail("Model manifest is invalid"); return; }
    const auto formatVersion = static_cast<int>(manifest.getProperty("modelFormatVersion", 0));
    if (formatVersion < 1 || formatVersion > 2)
    { fail("Model manifest schema is unsupported"); return; }
    juce::MemoryBlock modelBlock, inputBlock, outputBlock;
    if (! modelFile.loadFileAsData(modelBlock) || ! inputFile.loadFileAsData(inputBlock)
        || ! outputFile.loadFileAsData(outputBlock) || inputBlock.getSize() == 0
        || inputBlock.getSize() != outputBlock.getSize() || inputBlock.getSize() % sizeof(float) != 0)
    { fail("Model or test-vector data is invalid"); return; }
    const auto expectedHash = manifest.getProperty("sha256", "").toString();
    const auto actualHash = juce::SHA256(modelBlock.getData(), modelBlock.getSize()).toHexString();
    if (expectedHash.isEmpty() || ! actualHash.equalsIgnoreCase(expectedHash))
    { fail("model.bin SHA-256 does not match the manifest"); return; }
    const auto sampleCount = inputBlock.getSize() / sizeof(float);
    std::vector<float> testInput(sampleCount), expectedOutput(sampleCount);
    std::memcpy(testInput.data(), inputBlock.getData(), inputBlock.getSize());
    std::memcpy(expectedOutput.data(), outputBlock.getData(), outputBlock.getSize());
    juce::var metadata, normalization;
    if (juce::JSON::parse(metadataFile.loadFileAsString(), metadata).failed()
        || juce::JSON::parse(normalizationFile.loadFileAsString(), normalization).failed())
    { fail("Model validation metadata is invalid"); return; }
    const auto maximumError = static_cast<float>(static_cast<double>(
        metadata.getProperty("maximumAbsoluteErrorTolerance", 1.0e-5)));
    const auto expectedRms = static_cast<float>(static_cast<double>(
        normalization.getProperty("inputRmsDb", -21.0)));
    if (stopToken.stop_requested()) return;
    std::string error;
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(modelBlock.getData()),
                                 modelBlock.getSize());
    if (! neuralAmp.stageModel(bytes, testInput, expectedOutput, maximumError, expectedRms, error))
    { fail(error); return; }
    {
        const std::scoped_lock lock(neuralStatusMutex);
        neuralStatusDetail = "Ready: " + artifactDirectory.getFileName().toStdString();
    }
    neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::ready, std::memory_order_relaxed);
    diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::ready);
}

void TubeForgeAudioProcessor::loadPackagedNeuralModel(std::stop_token stopToken,
                                                      juce::File packageDirectory)
{
    const auto fail = [this](std::string message)
    {
        { const std::scoped_lock lock(neuralStatusMutex); neuralStatusDetail = std::move(message); }
        neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::failed, std::memory_order_relaxed);
        diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::failed);
    };
    juce::MemoryBlock modelBlock;
    if (! packageDirectory.getChildFile("model.bin").loadFileAsData(modelBlock))
    { fail("Packaged neural model is missing"); return; }
    juce::var test;
    if (juce::JSON::parse(packageDirectory.getChildFile("model-test.json").loadFileAsString(), test).failed())
    { fail("Packaged model test vector is invalid"); return; }
    const auto inputValue = test.getProperty("input", {});
    const auto expectedValue = test.getProperty("expected", {});
    const auto* input = inputValue.getArray();
    const auto* expected = expectedValue.getArray();
    const auto tolerance = static_cast<float>(static_cast<double>(test.getProperty("tolerance", 1.0e-5)));
    if (input == nullptr || expected == nullptr || input->isEmpty() || input->size() != expected->size())
    { fail("Packaged model test vector is incomplete"); return; }
    std::vector<float> testInput(static_cast<std::size_t>(input->size()));
    std::vector<float> expectedOutput(testInput.size());
    for (int index = 0; index < input->size(); ++index)
    {
        testInput[static_cast<std::size_t>(index)] = static_cast<float>(input->getReference(index));
        expectedOutput[static_cast<std::size_t>(index)] = static_cast<float>(expected->getReference(index));
    }
    if (stopToken.stop_requested()) return;
    std::string error;
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(modelBlock.getData()), modelBlock.getSize());
    if (! neuralAmp.stageModel(bytes, testInput, expectedOutput, tolerance, -21.0f, error))
    { fail(error); return; }
    { const std::scoped_lock lock(neuralStatusMutex); neuralStatusDetail = "Ready: " + packageDirectory.getFileName().toStdString(); }
    neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::ready, std::memory_order_relaxed);
    diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::ready);
}

juce::String TubeForgeAudioProcessor::neuralModelStatusText() const
{
    const std::scoped_lock lock(neuralStatusMutex);
    return juce::String::fromUTF8(neuralStatusDetail.c_str());
}

void TubeForgeAudioProcessor::refreshPhysicalCircuit()
{
    if (currentBlockSize <= 0) return;
    const auto graph = currentCircuitGraph();
    const auto hash = circuitHash(graph);
    if (hash == physicalCircuitControlHash.load(std::memory_order_acquire)
        || hash == physicalCircuitRequestedHash.load(std::memory_order_acquire)) return;
    physicalCircuitRequestedHash.store(hash, std::memory_order_release);
    if (circuitCompiler.joinable()) circuitCompiler.request_stop();
    circuitCompiler = std::jthread([this, graph, hash](std::stop_token stopToken)
    {
        if (stopToken.stop_requested()) return;
        const auto report = physicalCircuit.stageGraph(graph);
        if (stopToken.stop_requested()) return;
        const auto publicationBusy = std::any_of(report.messages.begin(), report.messages.end(), [](const auto& message)
        {
            return message.message == "A graph publication is already pending";
        });
        if (!report.isValid() || publicationBusy)
        {
            const std::scoped_lock lock(circuitStatusMutex);
            circuitStatusDetail = report.messages.empty() ? "Physical circuit validation failed" : report.messages.front().message;
            physicalCircuitRequestedHash.store(0, std::memory_order_release);
            return;
        }
        physicalCircuitControlHash.store(hash, std::memory_order_release);
        physicalCircuitRequestedHash.store(0, std::memory_order_release);
        {
            const std::scoped_lock graphLock(circuitGraphMutex);
            desiredCircuitGraph = graph;
        }
        const std::scoped_lock lock(circuitStatusMutex);
        circuitStatusDetail = report.messages.empty()
            ? "Compiled on worker: " + std::to_string(graph.nodes.size()) + " immutable nodes, zero added latency"
            : "Compiled with bounded virtual-model warning: " + report.messages.front().message;
    });
}

nts::circuit::CircuitGraphDescription TubeForgeAudioProcessor::circuitGraphSnapshot() const
{
    // Parameter reads are atomic; returning the requested graph makes the editor
    // respond immediately while the immutable runtime copy compiles off-thread.
    return currentCircuitGraph();
}

juce::String TubeForgeAudioProcessor::circuitStatusText() const
{
    const std::scoped_lock lock(circuitStatusMutex);
    return juce::String::fromUTF8(circuitStatusDetail.c_str());
}

void TubeForgeAudioProcessor::requestToneAnalysis(const juce::File& audioFile)
{
    if (toneAnalysisWorker.joinable()) toneAnalysisWorker.request_stop();
    {
        const std::scoped_lock lock(toneAnalysisMutex);
        toneAnalysisStatus = "Analyzing " + audioFile.getFileName().toStdString() + " off the audio thread...";
    }
    toneAnalysisWorker = std::jthread([this, audioFile](std::stop_token stopToken)
    {
        analyzeToneFile(stopToken, audioFile);
    });
}

void TubeForgeAudioProcessor::analyzeToneFile(std::stop_token stopToken, juce::File audioFile)
{
    const auto fail = [this](std::string message)
    {
        const std::scoped_lock lock(toneAnalysisMutex);
        toneAnalysisStatus = std::move(message);
    };
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(audioFile));
    if (reader == nullptr) { fail("Could not decode the selected audio file"); return; }
    const auto maximumSamples = static_cast<juce::int64>(reader->sampleRate * 60.0);
    const auto samples = static_cast<int>(std::min(reader->lengthInSamples, maximumSamples));
    if (samples <= 0 || reader->numChannels == 0) { fail("The selected audio file is empty"); return; }
    juce::AudioBuffer<float> audio(static_cast<int>(std::min<unsigned int>(2, reader->numChannels)), samples);
    if (!reader->read(&audio, 0, samples, 0, true, reader->numChannels > 1))
    { fail("Could not read samples from the selected audio file"); return; }
    if (stopToken.stop_requested()) return;
    const auto left = std::span<const float>(audio.getReadPointer(0), static_cast<std::size_t>(samples));
    const auto right = audio.getNumChannels() > 1
        ? std::span<const float>(audio.getReadPointer(1), static_cast<std::size_t>(samples))
        : std::span<const float> {};
    auto result = toneAnalyzer.analyze({ left, right, reader->sampleRate,
                                         nts::tone::SourceType::userRecording, 1.0f });
    if (stopToken.stop_requested()) return;
    if (!result.success) { fail(result.error); return; }

    const auto fingerprint = static_cast<juce::int64>(audioFile.getFullPathName().hashCode64()
        ^ audioFile.getSize() ^ audioFile.getLastModificationTime().toMilliseconds());
    nts::tone::ToneProfile profile;
    profile.profileId = "user-" + juce::String::toHexString(fingerprint).toStdString();
    profile.name = audioFile.getFileNameWithoutExtension().toStdString();
    profile.embedding = result.embedding; profile.features = result.features; profile.report = result.report;
    profile.instrument = result.report.context.instrument; profile.sourceType = nts::tone::SourceType::userRecording;
    profile.captureQuality = result.report.confidence.aggregate; profile.modelVersion = result.analysisVersion;
    profile.userTags = { "user-analysis" };
    profile.licensingMetadata = "user-supplied audio; rights and redistribution permission not verified";
    const std::scoped_lock lock(toneAnalysisMutex);
    const auto nearest = toneProfiles.search(profile, 1);
    toneNearestProfile = nearest.empty() ? "First profile in this session"
        : "Nearest: " + nearest.front().name + "  (" + std::to_string(
            static_cast<int>(std::lround(nearest.front().similarity.score * 100.0f))) + "% match)";
    std::string databaseError;
    if (!toneProfiles.addOrReplace(std::move(profile), databaseError))
    { toneAnalysisStatus = databaseError; return; }
    latestToneAnalysis = std::move(result);
    toneAnalysisStatus = "Analysis complete: " + audioFile.getFileName().toStdString();
}

juce::String TubeForgeAudioProcessor::toneAnalysisStatusText() const
{
    const std::scoped_lock lock(toneAnalysisMutex);
    return juce::String::fromUTF8(toneAnalysisStatus.c_str());
}

juce::String TubeForgeAudioProcessor::toneNearestProfileText() const
{
    const std::scoped_lock lock(toneAnalysisMutex);
    return juce::String::fromUTF8(toneNearestProfile.c_str());
}

std::optional<nts::tone::ToneAnalysisResult> TubeForgeAudioProcessor::toneAnalysisSnapshot() const
{
    const std::scoped_lock lock(toneAnalysisMutex);
    return latestToneAnalysis;
}

std::size_t TubeForgeAudioProcessor::toneProfileCount() const
{
    const std::scoped_lock lock(toneAnalysisMutex);
    return toneProfiles.size();
}

void TubeForgeAudioProcessor::requestSongReconstruction(
    const juce::File& songFile, nts::reconstruction::TargetInstrument target,
    nts::reconstruction::StereoMode stereoMode)
{
    if (reconstructionWorker.joinable()) reconstructionWorker.request_stop();
    reconstructionProgressValue.store(0.0f, std::memory_order_relaxed);
    {
        const std::scoped_lock lock(reconstructionMutex);
        latestReconstruction.reset();
        reconstructionRegions.clear();
        lastReconstructionSong = songFile;
        lastReconstructionTarget = target;
        lastReconstructionStereoMode = stereoMode;
        activeReconstructionRegion = 0;
        reconstructionStatus = "Decoding " + songFile.getFileName().toStdString() + " off the audio thread...";
    }
    reconstructionWorker = std::jthread([this, songFile, target, stereoMode](std::stop_token stopToken)
    {
        reconstructSongFile(stopToken, songFile, target, stereoMode, std::nullopt);
    });
}

void TubeForgeAudioProcessor::cancelSongReconstruction()
{
    if (reconstructionWorker.joinable()) reconstructionWorker.request_stop();
    const std::scoped_lock lock(reconstructionMutex);
    reconstructionStatus = "Reconstruction cancellation requested";
}

void TubeForgeAudioProcessor::reconstructSongFile(
    std::stop_token stopToken, juce::File songFile,
    nts::reconstruction::TargetInstrument target,
    nts::reconstruction::StereoMode stereoMode,
    std::optional<std::size_t> regionOverride)
{
    const auto fail = [this](std::string message)
    {
        reconstructionProgressValue.store(0.0f, std::memory_order_relaxed);
        const std::scoped_lock lock(reconstructionMutex);
        reconstructionStatus = std::move(message);
    };
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(songFile));
    if (reader == nullptr) { fail("Could not decode the selected song; decoder support may be unavailable"); return; }
    const auto maximumSamples = static_cast<juce::int64>(reader->sampleRate * 60.0 * 20.0);
    const auto sourceSamples64 = std::min(reader->lengthInSamples, maximumSamples);
    if (sourceSamples64 <= 0 || sourceSamples64 > std::numeric_limits<int>::max() || reader->numChannels == 0)
    { fail("The selected song is empty or too long for the offline cache"); return; }
    const auto sourceSamples = static_cast<int>(sourceSamples64);
    juce::AudioBuffer<float> decoded(static_cast<int>(std::min<unsigned int>(2, reader->numChannels)), sourceSamples);
    if (! reader->read(&decoded, 0, sourceSamples, 0, true, reader->numChannels > 1))
    { fail("Could not read samples from the selected song"); return; }
    if (stopToken.stop_requested()) { fail("Reconstruction cancelled"); return; }
    reconstructionProgressValue.store(0.08f, std::memory_order_relaxed);

    constexpr double workingSampleRate = 48000.0;
    const auto outputSamples64 = static_cast<juce::int64>(std::ceil(
        static_cast<double>(sourceSamples) * workingSampleRate / reader->sampleRate));
    if (outputSamples64 <= 0 || outputSamples64 > std::numeric_limits<int>::max())
    { fail("Resampled song would exceed the offline cache limit"); return; }
    const auto outputSamples = static_cast<int>(outputSamples64);
    nts::reconstruction::StereoAudio song;
    song.sampleRate = workingSampleRate;
    song.left.resize(static_cast<std::size_t>(outputSamples));
    song.right.resize(static_cast<std::size_t>(outputSamples));
    const auto speedRatio = reader->sampleRate / workingSampleRate;
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto sourceChannel = std::min(channel, decoded.getNumChannels() - 1);
        juce::LagrangeInterpolator interpolator;
        auto& destination = channel == 0 ? song.left : song.right;
        interpolator.process(speedRatio, decoded.getReadPointer(sourceChannel), destination.data(),
                             outputSamples, sourceSamples, 0);
    }
    reconstructionProgressValue.store(0.12f, std::memory_order_relaxed);
    std::string mlFailure;
    auto mlStems = runMlStemSeparation(songFile, song, target, stopToken, mlFailure);
    if (stopToken.stop_requested()) { fail("Reconstruction cancelled"); return; }
    nts::reconstruction::StemSet stems;
    if (mlStems)
    {
        stems = std::move(*mlStems);
    }
    else
    {
        {
            const std::scoped_lock lock(reconstructionMutex);
            reconstructionStatus = "ML separator unavailable; running artifact-prone CPU fallback";
        }
        nts::reconstruction::SeparationOptions options;
        stems = stemSeparator.separate(song, options,
            [this](const nts::reconstruction::SeparationProgress& update)
            {
                reconstructionProgressValue.store(0.12f + update.fraction * 0.48f, std::memory_order_relaxed);
                const std::scoped_lock lock(reconstructionMutex);
                reconstructionStatus = update.stage;
                return true;
            }, stopToken);
        if (! mlFailure.empty()) stems.warnings.emplace_back("ML fallback: " + mlFailure);
    }
    if (! stems.success) { fail(stems.error); return; }

    const auto cacheDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("cache").getChildFile("source-reconstruction");
    if (cacheDirectory.createDirectory().wasOk())
    {
        std::unique_ptr<juce::OutputStream> stream = cacheDirectory
            .getChildFile(juce::String(stems.cacheKey) + ".wav").createOutputStream();
        juce::WavAudioFormat format;
        auto writer = format.createWriterFor(stream,
            juce::AudioFormatWriter::Options{}.withSampleRate(workingSampleRate)
                                               .withNumChannels(2).withBitsPerSample(24));
        if (writer != nullptr)
        {
            juce::AudioBuffer<float> cacheAudio(2, outputSamples);
            cacheAudio.copyFrom(0, 0, song.left.data(), outputSamples);
            cacheAudio.copyFrom(1, 0, song.right.data(), outputSamples);
            writer->writeFromAudioSampleBuffer(cacheAudio, 0, outputSamples);
        }
    }
    if (stopToken.stop_requested()) { fail("Reconstruction cancelled"); return; }
    reconstructionProgressValue.store(0.64f, std::memory_order_relaxed);
    auto selected = nts::reconstruction::selectStem(stems, target);
    selected = nts::reconstruction::applyStereoMode(selected, stereoMode);
    const auto qualityRegions = nts::reconstruction::scoreRegions(stems, target, 7.5, 3.75);
    auto playableRegions = nts::reconstruction::analyzePlayableRegions(selected, qualityRegions, target);
    if (playableRegions.empty()) { fail("No playable regions were detected in the selected stem"); return; }
    auto selectedRegion = std::size_t {};
    if (regionOverride && *regionOverride < playableRegions.size()) selectedRegion = *regionOverride;
    else
        selectedRegion = static_cast<std::size_t>(std::distance(playableRegions.begin(),
            std::max_element(playableRegions.begin(), playableRegions.end(), [](const auto& first, const auto& second)
            { return first.quality.confidence < second.quality.confidence; })));
    const auto regionAnalysis = playableRegions[selectedRegion];
    const auto regionQuality = regionAnalysis.quality;
    {
        const std::scoped_lock lock(reconstructionMutex);
        reconstructionRegions = playableRegions;
        activeReconstructionRegion = selectedRegion;
        reconstructionStatus = "Analyzing selected " + std::string(nts::reconstruction::toString(regionAnalysis.gainCharacter))
            + " part: " + regionAnalysis.dominantPitch + ", " + regionAnalysis.estimatedTuning;
    }
    auto region = nts::reconstruction::extractRegion(selected, regionQuality.startSeconds, regionQuality.endSeconds);
    auto normalized = nts::reconstruction::normalizeReference(region, regionQuality.reverbAmount > 0.45f);
    nts::tone::ToneAnalyzer analyzer;
    auto tone = analyzer.analyze({ normalized.audio.left, normalized.audio.right, workingSampleRate,
                                   nts::tone::SourceType::isolatedStem, regionQuality.confidence });
    if (! tone.success) { fail("Tone extraction failed: " + tone.error); return; }
    reconstructionProgressValue.store(0.72f, std::memory_order_relaxed);
    nts::reconstruction::ReconstructionReference reference;
    reference.sourceHash = stems.cacheKey;
    reference.regionStartSeconds = regionQuality.startSeconds;
    reference.regionEndSeconds = regionQuality.endSeconds;
    reference.separationModelVersion = stems.modelVersion;
    reference.analysisConfidence = tone.report.confidence.aggregate;
    reference.target = target;
    reference.dominantPitch = regionAnalysis.dominantPitch;
    reference.estimatedTuning = regionAnalysis.estimatedTuning;
    reference.gainCharacter = std::string(nts::reconstruction::toString(regionAnalysis.gainCharacter));
    reference.pitchConfidence = regionAnalysis.pitchConfidence;
    reference.tuningOffsetCents = regionAnalysis.tuningOffsetCents;
    reference.tone = std::move(tone);
    reference.quality = regionQuality;
    auto result = rigReconstructor.reconstruct(reference, normalized.audio.left, workingSampleRate, 4,
        [this](const nts::reconstruction::SeparationProgress& update)
        {
            reconstructionProgressValue.store(0.72f + update.fraction * 0.27f, std::memory_order_relaxed);
            const std::scoped_lock lock(reconstructionMutex);
            reconstructionStatus = update.stage;
            return true;
        }, stopToken);
    if (! result.success) { fail(result.error); return; }
    for (const auto& warning : stems.warnings) result.warnings.push_back(warning);
    result.warnings.emplace_back("user DI adaptation uses the selected reference until a dedicated DI is supplied");
    {
        const std::scoped_lock lock(reconstructionMutex);
        latestReconstruction = std::move(result);
        reconstructionStatus = "Ready: " + reference.gainCharacter + " part, " + reference.dominantPitch
            + ", " + reference.estimatedTuning + "; " + std::to_string(
            latestReconstruction->candidates.size()) + " playable candidates";
    }
    reconstructionProgressValue.store(1.0f, std::memory_order_relaxed);
}

std::optional<nts::reconstruction::StemSet> TubeForgeAudioProcessor::runMlStemSeparation(
    const juce::File& songFile, const nts::reconstruction::StereoAudio& mixture,
    nts::reconstruction::TargetInstrument target, std::stop_token stopToken,
    std::string& failureReason)
{
    const auto runtimeRoot = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("ml-separation");
    const auto runtimeManifest = runtimeRoot.getChildFile("runtime.json");
    if (! runtimeManifest.existsAsFile())
    {
        failureReason = "runtime is not installed; run scripts/setup-ml-separator.ps1";
        return std::nullopt;
    }
    juce::var runtime;
    if (juce::JSON::parse(runtimeManifest.loadFileAsString(), runtime).failed() || ! runtime.isObject())
    {
        failureReason = "runtime.json is invalid";
        return std::nullopt;
    }
    const juce::File python(runtime.getProperty("python", "").toString());
    const juce::File worker(runtime.getProperty("worker", "").toString());
    const auto model = runtime.getProperty("model", "htdemucs_6s").toString();
    const auto device = runtime.getProperty("device", "auto").toString();
    if (! python.existsAsFile() || ! worker.existsAsFile())
    {
        failureReason = "configured Python or Demucs worker is missing";
        return std::nullopt;
    }

    const auto sourceHash = juce::MD5(songFile).toHexString();
    const auto cacheKey = sourceHash + "-" + model.replaceCharacter(':', '_');
    const auto outputDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("cache").getChildFile("source-reconstruction")
        .getChildFile("ml").getChildFile(cacheKey);
    if (outputDirectory.createDirectory().failed())
    {
        failureReason = "could not create the local neural-stem cache";
        return std::nullopt;
    }
    const auto outputManifest = outputDirectory.getChildFile("manifest.json");
    const auto requiredTarget = target == nts::reconstruction::TargetInstrument::guitar ? "guitar.wav" : "bass.wav";
    const auto cacheReady = outputManifest.existsAsFile()
        && outputDirectory.getChildFile("vocals.wav").existsAsFile()
        && outputDirectory.getChildFile("drums.wav").existsAsFile()
        && outputDirectory.getChildFile(requiredTarget).existsAsFile();
    if (! cacheReady)
    {
        {
            const std::scoped_lock lock(reconstructionMutex);
            reconstructionStatus = "Starting local Demucs neural separator (first run downloads the model)";
        }
        juce::ChildProcess process;
        juce::StringArray arguments { python.getFullPathName(), worker.getFullPathName(),
                                      "--input", songFile.getFullPathName(),
                                      "--output", outputDirectory.getFullPathName(),
                                      "--model", model, "--device", device };
        if (! process.start(arguments))
        {
            failureReason = "could not start the Demucs worker process";
            return std::nullopt;
        }
        auto lastProgressText = juce::String();
        while (process.isRunning())
        {
            if (stopToken.stop_requested())
            {
                process.kill();
                failureReason = "neural separation was cancelled";
                return std::nullopt;
            }
            const auto progressFile = outputDirectory.getChildFile("progress.json");
            if (progressFile.existsAsFile())
            {
                juce::var progress;
                if (juce::JSON::parse(progressFile.loadFileAsString(), progress).wasOk() && progress.isObject())
                {
                    const auto fraction = std::clamp(static_cast<float>(progress.getProperty("fraction", 0.0)), 0.0f, 1.0f);
                    reconstructionProgressValue.store(0.12f + fraction * 0.48f, std::memory_order_relaxed);
                    const auto stage = progress.getProperty("stage", "Running neural separator").toString();
                    if (stage != lastProgressText)
                    {
                        const std::scoped_lock lock(reconstructionMutex);
                        reconstructionStatus = stage.toStdString();
                        lastProgressText = stage;
                    }
                }
            }
            process.waitForProcessToFinish(200);
        }
        const auto output = process.readAllProcessOutput();
        if (process.getExitCode() != 0 || ! outputManifest.existsAsFile())
        {
            failureReason = "Demucs failed: " + output.substring(std::max(0, output.length() - 500)).toStdString();
            return std::nullopt;
        }
    }

    juce::var manifest;
    if (juce::JSON::parse(outputManifest.loadFileAsString(), manifest).failed() || ! manifest.isObject())
    {
        failureReason = "neural stem manifest is invalid";
        return std::nullopt;
    }
    nts::reconstruction::StemSet result;
    result.success = true;
    result.cacheKey = cacheKey.toStdString();
    result.modelVersion = manifest.getProperty("modelVersion", "demucs:unknown").toString().toStdString();
    result.usedGpu = manifest.getProperty("device", "cpu").toString().containsIgnoreCase("cuda");
    result.reconstructionError = static_cast<float>(manifest.getProperty("reconstructionError", 0.0));
    std::string decodeError;
    if (! decodeStemFile(outputDirectory.getChildFile("vocals.wav"), mixture.sampleRate, mixture.samples(),
                         result.vocals, decodeError)
        || ! decodeStemFile(outputDirectory.getChildFile("drums.wav"), mixture.sampleRate, mixture.samples(),
                            result.drums, decodeError))
    {
        failureReason = decodeError;
        return std::nullopt;
    }
    if (target == nts::reconstruction::TargetInstrument::bass)
    {
        if (! decodeStemFile(outputDirectory.getChildFile("bass.wav"), mixture.sampleRate, mixture.samples(),
                             result.bass, decodeError))
        { failureReason = decodeError; return std::nullopt; }
    }
    else
    {
        if (! decodeStemFile(outputDirectory.getChildFile("guitar.wav"), mixture.sampleRate, mixture.samples(),
                             result.guitar, decodeError))
        { failureReason = decodeError; return std::nullopt; }
    }
    if (result.reconstructionError > 0.1f)
        result.warnings.emplace_back("neural stem mixture consistency is lower than expected");
    result.warnings.emplace_back("local " + result.modelVersion + " separation; source audio remains in the private cache");
    failureReason.clear();
    return result;
}

juce::String TubeForgeAudioProcessor::reconstructionStatusText() const
{
    const std::scoped_lock lock(reconstructionMutex);
    return juce::String::fromUTF8(reconstructionStatus.c_str());
}

std::optional<nts::reconstruction::ReconstructionResult> TubeForgeAudioProcessor::reconstructionSnapshot() const
{
    const std::scoped_lock lock(reconstructionMutex);
    return latestReconstruction;
}

std::vector<nts::reconstruction::PlayableRegion> TubeForgeAudioProcessor::reconstructionRegionsSnapshot() const
{
    const std::scoped_lock lock(reconstructionMutex);
    return reconstructionRegions;
}

bool TubeForgeAudioProcessor::requestReconstructionRegion(std::size_t index)
{
    juce::File song;
    nts::reconstruction::TargetInstrument target;
    nts::reconstruction::StereoMode stereoMode;
    {
        const std::scoped_lock lock(reconstructionMutex);
        if (index >= reconstructionRegions.size() || ! lastReconstructionSong.existsAsFile()) return false;
        song = lastReconstructionSong;
        target = lastReconstructionTarget;
        stereoMode = lastReconstructionStereoMode;
        latestReconstruction.reset();
        activeReconstructionRegion = index;
        reconstructionStatus = "Rebuilding from selected part (cached neural stems)...";
    }
    if (reconstructionWorker.joinable()) reconstructionWorker.request_stop();
    reconstructionProgressValue.store(0.60f, std::memory_order_relaxed);
    reconstructionWorker = std::jthread([this, song, target, stereoMode, index](std::stop_token stopToken)
    { reconstructSongFile(stopToken, song, target, stereoMode, index); });
    return true;
}

bool TubeForgeAudioProcessor::applyReconstructionCandidate(std::size_t index)
{
    nts::reconstruction::RigCandidate candidate;
    {
        const std::scoped_lock lock(reconstructionMutex);
        if (! latestReconstruction || index >= latestReconstruction->candidates.size()) return false;
        candidate = latestReconstruction->candidates[index];
        reconstructionStatus = "Applied " + candidate.rigPreset.name + " for live DI playing";
    }
    const auto& p = candidate.rigPreset.parameters;
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
    setParameterValue(parameterState, ParameterIds::sag, p.powerAmp.sag * 100.0f);
    setParameterValue(parameterState, ParameterIds::feedback, p.powerAmp.feedback * 100.0f);
    setParameterValue(parameterState, ParameterIds::crossover, p.bass.crossoverHz);
    setParameterValue(parameterState, ParameterIds::cleanBlend, p.bass.cleanBlend * 100.0f);
    const std::array stageIds { ParameterIds::stage1, ParameterIds::stage2, ParameterIds::stage3, ParameterIds::stage4 };
    for (std::size_t stage = 0; stage < p.stages.size(); ++stage)
        setParameterValue(parameterState, stageIds[stage], p.stages[stage].driveDb);
    return true;
}

juce::Result TubeForgeAudioProcessor::exportReconstruction(const juce::File& file) const
{
    const std::scoped_lock lock(reconstructionMutex);
    if (! latestReconstruction) return juce::Result::fail("No reconstruction result is available");
    if (! file.replaceWithText(juce::String::fromUTF8(
            nts::reconstruction::serializeResult(*latestReconstruction).c_str())))
        return juce::Result::fail("Could not write the reconstruction package");
    return juce::Result::ok();
}

std::vector<nts::assistant::ParameterValue> TubeForgeAudioProcessor::assistantParameterValues() const
{
    std::vector<nts::assistant::ParameterValue> values;
    values.reserve(nts::assistant::parameterSchema().size());
    for (const auto& descriptor : nts::assistant::parameterSchema())
    {
        const auto id = std::string(descriptor.id);
        values.push_back({ id, valueOf(parameterState, id.c_str()) });
    }
    return values;
}

void TubeForgeAudioProcessor::applyAssistantParameterValues(
    std::span<const nts::assistant::ParameterValue> values)
{
    for (const auto& value : values)
        setParameterValue(parameterState, value.id.c_str(), value.value);
}

void TubeForgeAudioProcessor::refreshAssistant()
{
    nts::assistant::AudioSummaryFrame frame;
    bool receivedFrame {};
    while (assistantSummaryQueue.pop(frame))
    {
        assistantSummaryAccumulator.add(frame);
        receivedFrame = true;
    }
    const auto now = std::chrono::steady_clock::now();
    nts::assistant::Goal goal;
    nts::assistant::PreferenceProfile preferences;
    {
        const std::scoped_lock lock(assistantMutex);
        if (assistantActionSession.hasPreview()) return;
        goal = assistantGoal; preferences = assistantPreferences;
        if (! receivedFrame && goal == nts::assistant::Goal::diagnose) return;
        if (now - lastAssistantEvaluation < std::chrono::milliseconds(750)) return;
        lastAssistantEvaluation = now;
    }
    nts::assistant::AssistantInput input;
    input.signal = assistantSummaryAccumulator.observation();
    input.rig = currentAmpParameters(); input.parameters = assistantParameterValues(); input.goal = goal;
    {
        const std::scoped_lock lock(toneAnalysisMutex);
        input.tone = latestToneAnalysis;
    }
    {
        const std::scoped_lock lock(reconstructionMutex);
        if (latestReconstruction) input.reference = latestReconstruction->reference.tone;
    }
    auto recommendations = assistantEngine.evaluate(input, preferences);
    const std::scoped_lock lock(assistantMutex);
    currentAssistantRecommendations = std::move(recommendations);
    if (currentAssistantRecommendations.empty())
        assistantStatus = input.signal.frames == 0
            ? "Waiting for live DI/output summary frames"
            : "No high-confidence technical problem detected";
    else
        assistantStatus = std::to_string(currentAssistantRecommendations.size())
            + " bounded suggestion(s); preview is required before acceptance";
}

void TubeForgeAudioProcessor::setAssistantGoal(nts::assistant::Goal goal)
{
    const std::scoped_lock lock(assistantMutex);
    assistantGoal = goal;
    lastAssistantEvaluation = {};
    assistantStatus = "Goal selected: " + std::string(nts::assistant::toString(goal));
}

std::vector<nts::assistant::Recommendation> TubeForgeAudioProcessor::assistantRecommendations() const
{
    const std::scoped_lock lock(assistantMutex);
    return currentAssistantRecommendations;
}

juce::String TubeForgeAudioProcessor::assistantStatusText() const
{
    const std::scoped_lock lock(assistantMutex);
    return juce::String::fromUTF8(assistantStatus.c_str());
}

bool TubeForgeAudioProcessor::previewAssistantRecommendation(std::size_t index)
{
    auto current = assistantParameterValues();
    std::vector<nts::assistant::ParameterValue> preview;
    {
        const std::scoped_lock lock(assistantMutex);
        if (index >= currentAssistantRecommendations.size())
        { assistantStatus = "No assistant suggestion selected"; return false; }
        std::string error;
        if (! assistantActionSession.preview(currentAssistantRecommendations[index], current, error))
        { assistantStatus = error; return false; }
        preview = assistantActionSession.previewState()->after;
        assistantStatus = "Previewing " + currentAssistantRecommendations[index].diagnosis
            + "; accept or reject to continue";
    }
    applyAssistantParameterValues(preview);
    return true;
}

bool TubeForgeAudioProcessor::acceptAssistantPreview()
{
    auto current = assistantParameterValues();
    {
        const std::scoped_lock lock(assistantMutex);
        std::string error;
        if (! assistantActionSession.accept(current, assistantPreferences, error))
        { assistantStatus = error; return false; }
        if (assistantPreferences.personalizationEnabled)
        {
            const auto lookup = [&current](std::string_view id, float fallback)
            {
                const auto found = std::find_if(current.begin(), current.end(), [id](const auto& item) { return item.id == id; });
                return found == current.end() ? fallback : found->value;
            };
            assistantPreferences.preferredInstrument = valueOf(parameterState, ParameterIds::instrument) >= 0.5f
                ? nts::tone::Instrument::bass : nts::tone::Instrument::guitar;
            assistantPreferences.preferredGain = std::clamp(lookup("gain", 5.0f) * 0.1f, 0.0f, 1.0f);
            assistantPreferences.preferredBrightness = std::clamp(
                (lookup("treble", 5.0f) + lookup("presence", 5.0f)) * 0.05f, 0.0f, 1.0f);
            assistantPreferences.preferredCleanBlend = std::clamp(lookup("cleanBlend", 50.0f) * 0.01f, 0.0f, 1.0f);
            if (valueOf(parameterState, ParameterIds::cabinet) >= 0.5f)
                assistantPreferences.frequentlyUsedCabinets.push_back("TubeForge internal cabinet");
        }
        assistantStatus = "Preview accepted; exact previous state is available through Undo";
        lastAssistantEvaluation = std::chrono::steady_clock::now();
    }
    applyAssistantParameterValues(current); saveAssistantPreferences(); return true;
}

bool TubeForgeAudioProcessor::rejectAssistantPreview()
{
    auto current = assistantParameterValues();
    {
        const std::scoped_lock lock(assistantMutex);
        std::string error;
        if (! assistantActionSession.reject(current, assistantPreferences, error))
        { assistantStatus = error; return false; }
        assistantStatus = "Preview rejected; exact previous parameter state restored";
        lastAssistantEvaluation = std::chrono::steady_clock::now();
    }
    applyAssistantParameterValues(current); saveAssistantPreferences(); return true;
}

bool TubeForgeAudioProcessor::undoAssistantChange()
{
    auto current = assistantParameterValues();
    {
        const std::scoped_lock lock(assistantMutex);
        std::string error;
        if (! assistantActionSession.undo(current, error))
        { assistantStatus = error; return false; }
        assistantStatus = "Assistant change undone exactly";
        lastAssistantEvaluation = std::chrono::steady_clock::now();
    }
    applyAssistantParameterValues(current); return true;
}

bool TubeForgeAudioProcessor::assistantPreviewActive() const
{
    const std::scoped_lock lock(assistantMutex);
    return assistantActionSession.hasPreview();
}

void TubeForgeAudioProcessor::saveAssistantPreferences() const
{
    nts::assistant::PreferenceProfile preferences;
    {
        const std::scoped_lock lock(assistantMutex);
        preferences = assistantPreferences;
    }
    const auto file = assistantPreferencesFile();
    if (file.getParentDirectory().createDirectory().wasOk())
        (void) file.replaceWithText(juce::String::fromUTF8(
            nts::assistant::serializePreferences(preferences).c_str()));
}

void TubeForgeAudioProcessor::setAssistantPersonalizationEnabled(bool enabled)
{
    {
        const std::scoped_lock lock(assistantMutex);
        assistantPreferences.personalizationEnabled = enabled;
        assistantStatus = enabled ? "Local personalization enabled" : "Personalization disabled";
        lastAssistantEvaluation = {};
    }
    saveAssistantPreferences();
}

bool TubeForgeAudioProcessor::assistantPersonalizationEnabled() const
{
    const std::scoped_lock lock(assistantMutex);
    return assistantPreferences.personalizationEnabled;
}

void TubeForgeAudioProcessor::clearAssistantPreferences()
{
    {
        const std::scoped_lock lock(assistantMutex);
        const auto enabled = assistantPreferences.personalizationEnabled;
        assistantPreferences = {}; assistantPreferences.personalizationEnabled = enabled;
        assistantActionSession.clearHistory();
        assistantStatus = "Local assistant preferences and undo history cleared";
        lastAssistantEvaluation = {};
    }
    const auto file = assistantPreferencesFile();
    if (file.existsAsFile()) (void) file.deleteFile();
}

std::vector<nts::ecosystem::ProfileRecord> TubeForgeAudioProcessor::searchTonePackages(
    const nts::ecosystem::ProfileQuery& query) const
{
    return tonePackageLibrary.search(query);
}

juce::Result TubeForgeAudioProcessor::refreshTonePackages()
{
    std::string error;
    if (! tonePackageLibrary.refresh(error)) return juce::Result::fail(error);
    return juce::Result::ok();
}

juce::Result TubeForgeAudioProcessor::importTonePackage(const juce::File& packageDirectory)
{
    std::string error;
    if (! tonePackageLibrary.importPackage(std::filesystem::path(packageDirectory.getFullPathName().toStdString()), error))
        return juce::Result::fail(error);
    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "ecosystem", "package-import", "Tone package imported", "metadata-only" });
    return juce::Result::ok();
}

juce::Result TubeForgeAudioProcessor::exportCurrentTonePackage(const juce::File& destination,
                                                               const juce::String& name,
                                                               const juce::String& author) const
{
    if (name.trim().isEmpty() || author.trim().isEmpty())
        return juce::Result::fail("Profile name and author are required");
    nts::amp::AmpPreset preset;
    preset.name = name.trim().toStdString(); preset.parameters = currentAmpParameters();
    nts::ecosystem::PackageExportRequest request;
    request.manifest.name = preset.name; request.manifest.author = author.trim().toStdString();
    request.manifest.instrument = preset.parameters.instrument == nts::amp::Instrument::bass ? "bass" : "guitar";
    request.manifest.packageLicense = "user-owned"; request.manifest.tags = { "local", "amp-rig" };
    request.rigJson = nts::amp::serializePreset(preset, true);
    std::string error;
    if (! tonePackageLibrary.exportPackage(
            std::filesystem::path(destination.withFileExtension("ntone").getFullPathName().toStdString()),
            std::move(request), error))
        return juce::Result::fail(error);
    return juce::Result::ok();
}

juce::Result TubeForgeAudioProcessor::applyTonePackage(const juce::String& packageId)
{
    const auto record = tonePackageLibrary.find(packageId.toStdString());
    if (! record || ! record->compatible) return juce::Result::fail("Profile is missing or needs a newer runtime");
    const auto rigPath = record->packagePath / "rig.json";
    const auto presetText = juce::File(juce::String(rigPath.wstring().c_str())).loadFileAsString().toStdString();
    const auto preset = nts::amp::deserializePreset(presetText);
    if (! preset) return juce::Result::fail("Profile rig failed schema validation");
    const auto& p = preset->parameters;
    setParameterValue(parameterState, ParameterIds::instrument, p.instrument == nts::amp::Instrument::bass ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::topology, p.topology == nts::amp::Topology::vintageBloom ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::gain, 5.0f);
    setParameterValue(parameterState, ParameterIds::bass, p.toneStack.bass * 10.0f);
    setParameterValue(parameterState, ParameterIds::mid, p.toneStack.mid * 10.0f);
    setParameterValue(parameterState, ParameterIds::treble, p.toneStack.treble * 10.0f);
    setParameterValue(parameterState, ParameterIds::presence, p.powerAmp.presence * 10.0f);
    setParameterValue(parameterState, ParameterIds::resonance, p.powerAmp.resonance * 10.0f);
    setParameterValue(parameterState, ParameterIds::master, p.powerAmp.masterDb);
    setParameterValue(parameterState, ParameterIds::cabinet, p.cabinet.bypass ? 0.0f : 1.0f);
    constexpr std::array<const char*, 4> stageIds { ParameterIds::stage1, ParameterIds::stage2,
                                                    ParameterIds::stage3, ParameterIds::stage4 };
    for (std::size_t index = 0; index < p.stages.size(); ++index)
        setParameterValue(parameterState, stageIds[index], p.stages[index].driveDb);
    setParameterValue(parameterState, ParameterIds::bias, p.stages[0].bias);
    setParameterValue(parameterState, ParameterIds::lowCut, p.preEq.lowCutHz);
    setParameterValue(parameterState, ParameterIds::highCut, p.preEq.highCutHz);
    setParameterValue(parameterState, ParameterIds::tightness, p.preEq.tightness * 10.0f);
    setParameterValue(parameterState, ParameterIds::pickEmphasis, p.preEq.pickEmphasisDb);
    const auto factor = p.stages[0].oversamplingFactor;
    setParameterValue(parameterState, ParameterIds::oversampling, factor >= 8 ? 3.0f : factor >= 4 ? 2.0f : factor >= 2 ? 1.0f : 0.0f);
    setParameterValue(parameterState, ParameterIds::sag, p.powerAmp.sag * 100.0f);
    setParameterValue(parameterState, ParameterIds::feedback, p.powerAmp.feedback * 100.0f);
    setParameterValue(parameterState, ParameterIds::crossover, p.bass.crossoverHz);
    setParameterValue(parameterState, ParameterIds::cleanBlend, p.bass.cleanBlend * 100.0f);
    setParameterValue(parameterState, ParameterIds::cabinetAlignment, static_cast<float>(p.cabinet.delaySamplesB));
    std::string error;
    (void) tonePackageLibrary.markUsed(packageId.toStdString(), error);
    const auto hasModel = std::any_of(record->manifest.assets.begin(), record->manifest.assets.end(), [](const auto& asset)
    { return asset.path == "model.bin"; });
    if (hasModel)
    {
        if (neuralLoader.joinable()) neuralLoader.request_stop();
        neuralLoadStatus.store(nts::diagnostics::AssetLoadStatus::loading, std::memory_order_relaxed);
        diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::loading);
        const juce::File packageDirectory(juce::String(record->packagePath.wstring().c_str()));
        neuralLoader = std::jthread([this, packageDirectory](std::stop_token stopToken)
        { loadPackagedNeuralModel(stopToken, packageDirectory); });
    }
    logger.log({ std::chrono::system_clock::now(), nts::diagnostics::LogSeverity::info,
                 "ecosystem", "package-applied", "Tone package applied", packageId.toStdString() });
    return juce::Result::ok();
}

juce::Result TubeForgeAudioProcessor::setTonePackageFavorite(const juce::String& packageId, bool favorite)
{
    std::string error;
    if (! tonePackageLibrary.setFavorite(packageId.toStdString(), favorite, error)) return juce::Result::fail(error);
    return juce::Result::ok();
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
    layout.add(std::make_unique<Bool>(juce::ParameterID { ParameterIds::bypass, 1 }, "Bypass", false));
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
    layout.add(std::make_unique<Choice>(juce::ParameterID { ParameterIds::oversampling, 1 }, "Oversampling",
                                        juce::StringArray { "1x (minimum latency)", "2x", "4x", "8x" }, 0));
    addFloat(ParameterIds::sag, "Sag", Range { 0.0f, 100.0f, 0.1f }, 35.0f, "%");
    addFloat(ParameterIds::feedback, "Feedback", Range { 0.0f, 100.0f, 0.1f }, 35.0f, "%");
    addFloat(ParameterIds::crossover, "Bass crossover", Range { 60.0f, 500.0f, 1.0f, 0.5f }, 180.0f, "Hz");
    addFloat(ParameterIds::cleanBlend, "Bass clean blend", Range { 0.0f, 100.0f, 0.1f }, 55.0f, "%");
    addFloat(ParameterIds::cabinetAlignment, "Cabinet alignment", Range { 0.0f, 256.0f, 1.0f }, 0.0f, "samples");
    addFloat(ParameterIds::tightness, "Tightness", Range { 0.0f, 10.0f, 0.01f }, 7.2f);
    addFloat(ParameterIds::pickEmphasis, "Pick emphasis", Range { -12.0f, 12.0f, 0.1f }, 2.5f, "dB");
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
    state.engine.ampControls.reserve(ampControlIds.size());
    for (const auto* id : ampControlIds) state.engine.ampControls.push_back(valueOf(parameterState, id));
    state.device.sampleRate = currentSampleRate;
    state.device.bufferSize = currentBlockSize;
    state.graph.latencySamples = latencyBudget.processingSamples();
    state.graph.physicalCircuitJson = nts::circuit::serializeCircuit(circuitGraphSnapshot());
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
    return true;
}

nts::amp::AmpParameters TubeForgeAudioProcessor::currentAmpParameters() const noexcept
{
    const auto instrumentIndex = std::clamp(static_cast<int>(
        valueOf(parameterState, ParameterIds::instrument)), 0, 1);
    const auto topologyIndex = std::clamp(static_cast<int>(
        valueOf(parameterState, ParameterIds::topology)), 0, 1);
    auto parameters = factoryAmpParameters[static_cast<std::size_t>(instrumentIndex * 2 + topologyIndex)];
    parameters.manualInputTrimDb = valueOf(parameterState, ParameterIds::input);
    const auto simpleGainOffset = (valueOf(parameterState, ParameterIds::gain) - 5.0f) * 3.0f;
    parameters.toneStack.bass = valueOf(parameterState, ParameterIds::bass) * 0.1f;
    parameters.toneStack.mid = valueOf(parameterState, ParameterIds::mid) * 0.1f;
    parameters.toneStack.treble = valueOf(parameterState, ParameterIds::treble) * 0.1f;
    parameters.powerAmp.presence = valueOf(parameterState, ParameterIds::presence) * 0.1f;
    parameters.powerAmp.resonance = valueOf(parameterState, ParameterIds::resonance) * 0.1f;
    parameters.powerAmp.masterDb = valueOf(parameterState, ParameterIds::master);
    parameters.cabinet.bypass = valueOf(parameterState, ParameterIds::cabinet) < 0.5f;
    const std::array stageIds { ParameterIds::stage1, ParameterIds::stage2,
                                ParameterIds::stage3, ParameterIds::stage4 };
    const auto oversamplingIndex = std::clamp(static_cast<int>(
        valueOf(parameterState, ParameterIds::oversampling)), 0, 3);
    const auto oversamplingFactor = std::array { 1, 2, 4, 8 }[static_cast<std::size_t>(oversamplingIndex)];
    for (std::size_t stage = 0; stage < parameters.stages.size(); ++stage)
    {
        parameters.stages[stage].driveDb = valueOf(parameterState, stageIds[stage]) + simpleGainOffset;
        parameters.stages[stage].bias = valueOf(parameterState, ParameterIds::bias);
        parameters.stages[stage].oversamplingFactor = oversamplingFactor;
    }
    parameters.preEq.lowCutHz = valueOf(parameterState, ParameterIds::lowCut);
    parameters.preEq.highCutHz = valueOf(parameterState, ParameterIds::highCut);
    parameters.preEq.tightness = valueOf(parameterState, ParameterIds::tightness) * 0.1f;
    parameters.preEq.pickEmphasisDb = valueOf(parameterState, ParameterIds::pickEmphasis);
    parameters.powerAmp.sag = valueOf(parameterState, ParameterIds::sag) * 0.01f;
    parameters.powerAmp.feedback = valueOf(parameterState, ParameterIds::feedback) * 0.01f;
    parameters.bass.crossoverHz = valueOf(parameterState, ParameterIds::crossover);
    parameters.bass.cleanBlend = valueOf(parameterState, ParameterIds::cleanBlend) * 0.01f;
    parameters.cabinet.delaySamplesB = static_cast<std::size_t>(
        valueOf(parameterState, ParameterIds::cabinetAlignment));
    return parameters;
}

nts::circuit::SimpleControls TubeForgeAudioProcessor::currentCircuitControls() const noexcept
{
    nts::circuit::SimpleControls controls;
    controls.tubeCharacter = std::clamp((valueOf(parameterState, ParameterIds::bias) + 0.8f) / 1.6f, 0.0f, 1.0f);
    controls.headroom = std::clamp((valueOf(parameterState, ParameterIds::master) + 60.0f) / 72.0f, 0.0f, 1.0f);
    controls.breakup = std::clamp(valueOf(parameterState, ParameterIds::gain) * 0.1f, 0.0f, 1.0f);
    controls.tightness = std::clamp(valueOf(parameterState, ParameterIds::tightness) * 0.1f, 0.0f, 1.0f);
    controls.sag = std::clamp(valueOf(parameterState, ParameterIds::sag) * 0.01f, 0.0f, 1.0f);
    controls.powerSize = std::clamp((valueOf(parameterState, ParameterIds::resonance)
                                     + valueOf(parameterState, ParameterIds::presence)) * 0.05f, 0.0f, 1.0f);
    controls.feedback = std::clamp(valueOf(parameterState, ParameterIds::feedback) * 0.01f, 0.0f, 1.0f);
    controls.cabinet = std::clamp(valueOf(parameterState, ParameterIds::cabinet) >= 0.5f
                                      ? valueOf(parameterState, ParameterIds::treble) * 0.1f : 0.0f, 0.0f, 1.0f);
    return controls;
}

nts::circuit::CircuitGraphDescription TubeForgeAudioProcessor::currentCircuitGraph() const
{
    auto graph = nts::circuit::makeSimpleCircuit(currentCircuitControls());
    graph.id = "tubeforge.circuit.user";
    graph.name = "TubeForge User Circuit";

    const auto preampTube = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitPreampTube)), 0, 2);
    const auto powerTube = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitPowerTube)), 0, 1);
    const auto powerTopology = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitPowerTopology)), 0, 2);
    const auto toneStyle = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitToneStack)), 0, 2);
    const auto numerical = valueOf(parameterState, ParameterIds::circuitBackend) >= 0.5f;
    const auto cabinetStyle = std::clamp(static_cast<int>(valueOf(parameterState, ParameterIds::circuitCabinetStyle)), 0, 2);
    const std::array preampIds { "tube.12au7.v1", "tube.12at7.v1", "tube.12ax7.v1" };
    const std::array powerIds { "power.6v6.v1", "power.el34.v1" };
    const std::array cabinetIds { "cabinet.reactive.v1", "cabinet.open-back.v1", "cabinet.bass-sealed.v1" };
    const std::array toneIds { "passive.fmv.vintage.v1", "passive.fmv.modern.v1", "passive.fmv.bass.v1" };

    for (auto& node : graph.nodes)
    {
        if (node.type == nts::circuit::NodeType::filter)
            setCircuitParameter(node, "cutoff-hz", valueOf(parameterState, ParameterIds::lowCut));
        else if (node.type == nts::circuit::NodeType::triodeStage)
        {
            node.modelId = preampIds[static_cast<std::size_t>(preampTube)];
            node.backend = numerical ? nts::circuit::ModelBackend::numerical : nts::circuit::ModelBackend::graybox;
            const auto stageDrive = (valueOf(parameterState, ParameterIds::stage1) + 12.0f) / 54.0f;
            setCircuitParameter(node, "drive", std::clamp(0.55f * stageDrive + 0.45f * valueOf(parameterState, ParameterIds::gain) * 0.1f, 0.0f, 1.0f));
            setCircuitParameter(node, "bias", std::clamp((valueOf(parameterState, ParameterIds::bias) + 0.8f) / 1.6f, 0.0f, 1.0f));
        }
        else if (node.type == nts::circuit::NodeType::toneStack)
        {
            node.modelId = toneIds[static_cast<std::size_t>(toneStyle)];
            setCircuitParameter(node, "bass", valueOf(parameterState, ParameterIds::bass) * 0.1f);
            setCircuitParameter(node, "middle", valueOf(parameterState, ParameterIds::mid) * 0.1f);
            setCircuitParameter(node, "treble", valueOf(parameterState, ParameterIds::treble) * 0.1f);
            constexpr std::array slope { 100000.0f, 56000.0f, 100000.0f };
            constexpr std::array bassCap { 22.0e-9f, 47.0e-9f, 100.0e-9f };
            constexpr std::array midCap { 22.0e-9f, 22.0e-9f, 47.0e-9f };
            constexpr std::array trebleCap { 250.0e-12f, 470.0e-12f, 330.0e-12f };
            setCircuitParameter(node, "slope-resistance-ohm", slope[static_cast<std::size_t>(toneStyle)]);
            setCircuitParameter(node, "bass-cap-f", bassCap[static_cast<std::size_t>(toneStyle)]);
            setCircuitParameter(node, "mid-cap-f", midCap[static_cast<std::size_t>(toneStyle)]);
            setCircuitParameter(node, "treble-cap-f", trebleCap[static_cast<std::size_t>(toneStyle)]);
        }
        else if (node.type == nts::circuit::NodeType::phaseInverter)
            setCircuitParameter(node, "drive", std::clamp((valueOf(parameterState, ParameterIds::stage3) + 12.0f) / 54.0f, 0.0f, 1.0f));
        else if (node.type == nts::circuit::NodeType::powerStage)
        {
            node.modelId = powerIds[static_cast<std::size_t>(powerTube)];
            setCircuitParameter(node, "topology", static_cast<float>(powerTopology));
            setCircuitParameter(node, "tube-count", powerTopology == 0 ? 1.0f : powerTopology == 1 ? 2.0f : 4.0f);
            setCircuitParameter(node, "drive", std::clamp((valueOf(parameterState, ParameterIds::stage4) + 12.0f) / 54.0f, 0.0f, 1.0f));
            setCircuitParameter(node, "sag", valueOf(parameterState, ParameterIds::sag) * 0.01f);
            setCircuitParameter(node, "feedback", valueOf(parameterState, ParameterIds::feedback) * 0.0075f);
        }
        else if (node.type == nts::circuit::NodeType::feedback)
        {
            setCircuitParameter(node, "amount", valueOf(parameterState, ParameterIds::feedback) * 0.0075f);
            setCircuitParameter(node, "presence", valueOf(parameterState, ParameterIds::presence) * 0.1f);
        }
        else if (node.type == nts::circuit::NodeType::cabinet)
        {
            node.modelId = cabinetIds[static_cast<std::size_t>(cabinetStyle)];
            constexpr std::array resonance { 0.50f, 0.38f, 0.78f };
            constexpr std::array brightness { 0.60f, 0.78f, 0.34f };
            setCircuitParameter(node, "resonance", resonance[static_cast<std::size_t>(cabinetStyle)]);
            setCircuitParameter(node, "brightness", brightness[static_cast<std::size_t>(cabinetStyle)]);
        }
    }

    const auto triode = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    if (triode != graph.nodes.end())
        graph.nodes.insert(std::next(triode), { "high-filter", nts::circuit::NodeType::filter, "rc.lowpass.v1",
            nts::circuit::ModelBackend::graybox, {}, { { "cutoff-hz", valueOf(parameterState, ParameterIds::highCut) }, { "high-pass", 0.0f } } });
    if (valueOf(parameterState, ParameterIds::cabinet) < 0.5f)
        std::erase_if(graph.nodes, [](const auto& node) { return node.type == nts::circuit::NodeType::cabinet; });
    graph.connections.clear();
    for (std::size_t index = 1; index < graph.nodes.size(); ++index)
        graph.connections.push_back({ graph.nodes[index - 1].id, 0, graph.nodes[index].id, 0, false });
    return graph;
}

void TubeForgeAudioProcessor::refreshProcessingLatency(bool notifyHostFromAudioThread) noexcept
{
    const auto mode = static_cast<int>(std::lround(valueOf(parameterState, ParameterIds::engineMode)));
    const auto processingLatency = mode == 0 ? static_cast<int>(traditionalAmp.latencySamples())
                                            : mode == 2 ? static_cast<int>(physicalCircuit.latencySamples()) : 0;
    const auto previous = pendingOversamplingLatencySamples.exchange(processingLatency,
                                                                      std::memory_order_relaxed);
    if (processingLatency != previous && notifyHostFromAudioThread)
        triggerAsyncUpdate();
}

void TubeForgeAudioProcessor::handleAsyncUpdate()
{
    latencyBudget.oversamplingSamples = pendingOversamplingLatencySamples.load(std::memory_order_relaxed);
    diagnostics.setLatencyBudget(latencyBudget);
    updateReportedLatency();
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
