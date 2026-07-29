#pragma once

#include <nts/audio/CoreEngine.h>
#include <nts/assistant/ToneAssistant.h>
#include <nts/amp/TraditionalAmp.h>
#include <nts/circuit/CircuitProcessor.h>
#include <nts/diagnostics/DiagnosticsCollector.h>
#include <nts/diagnostics/LatencyBudget.h>
#include <nts/diagnostics/StructuredLogger.h>
#include <nts/ecosystem/TonePackage.h>
#include <nts/ml/NeuralAmpProcessor.h>
#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/state/ProjectState.h>
#include <nts/tone/ToneProfileDatabase.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

class TubeForgeAudioProcessor final : public juce::AudioProcessor,
                                      private juce::AsyncUpdater
{
public:
    TubeForgeAudioProcessor();
    ~TubeForgeAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destinationData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    [[nodiscard]] juce::Result saveProject(const juce::File& file) const;
    [[nodiscard]] juce::Result loadProject(const juce::File& file);
    [[nodiscard]] nts::diagnostics::DiagnosticsSnapshot diagnosticsSnapshot() const noexcept;
    [[nodiscard]] const nts::audio::MeterState& meterState() const noexcept { return meters; }
    [[nodiscard]] const juce::Image& ampFaceplateArtwork() const noexcept { return ampFaceplateImage; }
    [[nodiscard]] juce::String modeName() const;
    [[nodiscard]] juce::String deviceStatusText() const;
    void refreshNonRealtimeDiagnostics() noexcept;
    void requestNeuralModelLoad(const juce::File& artifactDirectory);
    [[nodiscard]] juce::String neuralModelStatusText() const;
    [[nodiscard]] nts::ml::CalibrationReading neuralCalibrationReading() const noexcept
    { return neuralAmp.calibrationReading(); }
    void refreshPhysicalCircuit();
    [[nodiscard]] std::vector<nts::circuit::NodeTelemetry> circuitTelemetrySnapshot() const
    { return physicalCircuit.telemetrySnapshot(); }
    [[nodiscard]] nts::circuit::CircuitGraphDescription circuitGraphSnapshot() const;
    [[nodiscard]] juce::String circuitStatusText() const;
    void requestToneAnalysis(const juce::File& audioFile);
    [[nodiscard]] juce::String toneAnalysisStatusText() const;
    [[nodiscard]] juce::String toneNearestProfileText() const;
    [[nodiscard]] std::optional<nts::tone::ToneAnalysisResult> toneAnalysisSnapshot() const;
    [[nodiscard]] std::size_t toneProfileCount() const;
    void requestSongReconstruction(const juce::File& songFile,
                                   nts::reconstruction::TargetInstrument target,
                                   nts::reconstruction::StereoMode stereoMode);
    void cancelSongReconstruction();
    [[nodiscard]] juce::String reconstructionStatusText() const;
    [[nodiscard]] float reconstructionProgress() const noexcept
    { return reconstructionProgressValue.load(std::memory_order_relaxed); }
    [[nodiscard]] std::optional<nts::reconstruction::ReconstructionResult> reconstructionSnapshot() const;
    [[nodiscard]] std::vector<nts::reconstruction::PlayableRegion> reconstructionRegionsSnapshot() const;
    [[nodiscard]] bool requestReconstructionRegion(std::size_t index);
    [[nodiscard]] bool applyReconstructionCandidate(std::size_t index);
    [[nodiscard]] juce::Result exportReconstruction(const juce::File& file) const;
    void refreshAssistant();
    void setAssistantGoal(nts::assistant::Goal goal);
    [[nodiscard]] std::vector<nts::assistant::Recommendation> assistantRecommendations() const;
    [[nodiscard]] juce::String assistantStatusText() const;
    [[nodiscard]] bool previewAssistantRecommendation(std::size_t index);
    [[nodiscard]] bool acceptAssistantPreview();
    [[nodiscard]] bool rejectAssistantPreview();
    [[nodiscard]] bool undoAssistantChange();
    [[nodiscard]] bool assistantPreviewActive() const;
    void setAssistantPersonalizationEnabled(bool enabled);
    [[nodiscard]] bool assistantPersonalizationEnabled() const;
    void clearAssistantPreferences();
    [[nodiscard]] std::vector<nts::ecosystem::ProfileRecord> searchTonePackages(
        const nts::ecosystem::ProfileQuery& query) const;
    [[nodiscard]] juce::Result refreshTonePackages();
    [[nodiscard]] juce::Result importTonePackage(const juce::File& packageDirectory);
    [[nodiscard]] juce::Result exportCurrentTonePackage(const juce::File& destination,
                                                        const juce::String& name,
                                                        const juce::String& author) const;
    [[nodiscard]] juce::Result applyTonePackage(const juce::String& packageId);
    [[nodiscard]] juce::Result setTonePackageFavorite(const juce::String& packageId, bool favorite);
    void setStandaloneApplicationMode(bool shouldBeStandalone) noexcept
    {
        standaloneApplicationMode = shouldBeStandalone;
    }

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameterState; }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    static float valueOf(const juce::AudioProcessorValueTreeState& state, const char* parameterId) noexcept;
    static void setParameterValue(juce::AudioProcessorValueTreeState& state,
                                  const char* parameterId,
                                  float plainValue);
    [[nodiscard]] nts::state::ProjectState makeProjectState() const;
    bool applyProjectState(const nts::state::ProjectState& state);
    /** Index of the "Auto" entry in the oversampling choice list. Appended last so
        the existing 1x/2x/4x/8x indices stay valid in saved projects.
    */
    static constexpr int automaticOversamplingIndex = 4;

    [[nodiscard]] int automaticOversamplingFactor(
        const nts::amp::AmpParameters& parameters) const noexcept;
    [[nodiscard]] nts::amp::AmpParameters currentAmpParameters() const noexcept;
    [[nodiscard]] nts::circuit::SimpleControls currentCircuitControls() const noexcept;
    [[nodiscard]] nts::circuit::CircuitGraphDescription currentCircuitGraph() const;
    void refreshProcessingLatency(bool notifyHostFromAudioThread) noexcept;
    void updateReportedLatency();
    void handleAsyncUpdate() override;
    void loadNeuralArtifact(std::stop_token stopToken, juce::File artifactDirectory);
    void loadPackagedNeuralModel(std::stop_token stopToken, juce::File packageDirectory);
    void analyzeToneFile(std::stop_token stopToken, juce::File audioFile);
    void reconstructSongFile(std::stop_token stopToken, juce::File songFile,
                             nts::reconstruction::TargetInstrument target,
                             nts::reconstruction::StereoMode stereoMode,
                             std::optional<std::size_t> regionOverride = std::nullopt);
    [[nodiscard]] std::optional<nts::reconstruction::StemSet> runMlStemSeparation(
        const juce::File& songFile, const nts::reconstruction::StereoAudio& mixture,
        nts::reconstruction::TargetInstrument target, std::stop_token stopToken,
        std::string& failureReason);
    [[nodiscard]] std::vector<nts::assistant::ParameterValue> assistantParameterValues() const;
    void applyAssistantParameterValues(std::span<const nts::assistant::ParameterValue> values);
    void saveAssistantPreferences() const;

    juce::AudioProcessorValueTreeState parameterState;
    nts::audio::RuntimeParameters runtimeParameters;
    nts::audio::MeterState meters;
    juce::Image ampFaceplateImage;
    nts::audio::CoreEngine engine;
    nts::amp::TraditionalAmpProcessor traditionalAmp;
    nts::circuit::CircuitProcessor physicalCircuit;
    nts::ml::NeuralAmpProcessor neuralAmp;
    std::array<nts::amp::AmpParameters, 4> factoryAmpParameters;
    nts::diagnostics::DiagnosticsCollector diagnostics;
    nts::diagnostics::LatencyBudget latencyBudget;
    nts::diagnostics::StructuredLogger logger;
    std::uint64_t absoluteSamplePosition {};
    // The amplifier processes the host buffer in place, so the untouched input has
    // to be kept aside for the engine's input metering. Sized in prepareToPlay to
    // keep the audio callback allocation-free.
    juce::AudioBuffer<float> inputSnapshot;
    double currentSampleRate { 44100.0 };
    int currentBlockSize {};
    std::atomic<int> pendingOversamplingLatencySamples {};
    // Last factor chosen by Auto, kept so the choice has hysteresis across blocks.
    mutable std::atomic<int> autoOversamplingFactor { 4 };
    std::atomic<nts::diagnostics::AssetLoadStatus> neuralLoadStatus { nts::diagnostics::AssetLoadStatus::idle };
    mutable std::mutex neuralStatusMutex;
    std::string neuralStatusDetail { "No neural model loaded" };
    std::int64_t expectedNextHostSample { -1 };
    bool previousTransportPlaying {};
    bool standaloneApplicationMode {};
    std::jthread neuralLoader;
    std::atomic<std::uint64_t> physicalCircuitControlHash {};
    std::atomic<std::uint64_t> physicalCircuitRequestedHash {};
    mutable std::mutex circuitStatusMutex;
    std::string circuitStatusDetail { "Default physical circuit ready to compile" };
    mutable std::mutex circuitGraphMutex;
    nts::circuit::CircuitGraphDescription desiredCircuitGraph;
    std::jthread circuitCompiler;
    nts::tone::ToneAnalyzer toneAnalyzer;
    nts::tone::ToneProfileDatabase toneProfiles;
    mutable std::mutex toneAnalysisMutex;
    std::optional<nts::tone::ToneAnalysisResult> latestToneAnalysis;
    std::string toneAnalysisStatus { "Choose a guitar or bass recording to analyze" };
    std::string toneNearestProfile { "Profile library is empty" };
    std::jthread toneAnalysisWorker;
    nts::reconstruction::StemSeparator stemSeparator;
    nts::reconstruction::RigReconstructor rigReconstructor;
    mutable std::mutex reconstructionMutex;
    std::optional<nts::reconstruction::ReconstructionResult> latestReconstruction;
    std::vector<nts::reconstruction::PlayableRegion> reconstructionRegions;
    juce::File lastReconstructionSong;
    nts::reconstruction::TargetInstrument lastReconstructionTarget { nts::reconstruction::TargetInstrument::guitar };
    nts::reconstruction::StereoMode lastReconstructionStereoMode { nts::reconstruction::StereoMode::fullStereo };
    std::size_t activeReconstructionRegion {};
    std::string reconstructionStatus { "Import a song to begin source reconstruction" };
    std::atomic<float> reconstructionProgressValue {};
    std::jthread reconstructionWorker;
    nts::assistant::SummaryQueue assistantSummaryQueue;
    nts::assistant::SummaryAccumulator assistantSummaryAccumulator;
    nts::assistant::RecommendationEngine assistantEngine;
    nts::assistant::SafeActionSession assistantActionSession;
    mutable std::mutex assistantMutex;
    nts::assistant::PreferenceProfile assistantPreferences;
    std::vector<nts::assistant::Recommendation> currentAssistantRecommendations;
    nts::assistant::Goal assistantGoal { nts::assistant::Goal::diagnose };
    std::string assistantStatus { "Play through TubeForge to begin bounded signal diagnostics" };
    std::chrono::steady_clock::time_point lastAssistantEvaluation {};
    nts::ecosystem::ProfileLibrary tonePackageLibrary;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TubeForgeAudioProcessor)
};
