#pragma once

#include <nts/audio/CoreEngine.h>
#include <nts/assistant/ToneAssistant.h>
#include <nts/amp/TraditionalAmp.h>
#include <nts/circuit/CircuitProcessor.h>
#include <nts/diagnostics/DiagnosticsCollector.h>
#include <nts/diagnostics/SpscRingBuffer.h>
#include <nts/dsp/DelayLine.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Effects.h>
#include <nts/dsp/PitchDetector.h>
#include <nts/dsp/Smoothing.h>
#include <nts/ir/CabinetIrLoader.h>
#include <nts/diagnostics/LatencyBudget.h>
#include <nts/diagnostics/StructuredLogger.h>
#include <nts/ecosystem/TonePackage.h>
#include <nts/ml/NeuralAmpProcessor.h>
#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/state/ProjectState.h>
#include <nts/tone/ToneProfileDatabase.h>

#include "StudioServices.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

/** The one place a runtime-read parameter is named.

    Both the Param enumerators and the string-id table in the implementation file expand
    from this list, so an enumerator cannot end up pointing at a different parameter's
    value. Two parallel lists guarded by a test was the obvious alternative and it does
    not work: any test able to reach the mapping has to go through the same table it is
    checking, so a transposed pair agrees with itself and passes.

    Each name here must match a constant in the ParameterIds namespace exactly.
*/
#define TUBEFORGE_RUNTIME_PARAMETERS(X)                                                  \
    X(input) X(output) X(bypass) X(gain) X(bass) X(mid) X(treble) X(presence)             \
    X(resonance) X(master) X(cabinet) X(instrument) X(topology) X(stage1) X(stage2)       \
    X(stage3) X(stage4) X(bias) X(lowCut) X(highCut) X(oversampling) X(sag) X(feedback)   \
    X(crossover) X(cleanBlend) X(cabinetAlignment) X(tightness) X(pickEmphasis)           \
    X(engineMode) X(neuralMonitor) X(neuralCompensation) X(circuitPreampTube)             \
    X(circuitPowerTube) X(circuitPowerTopology) X(circuitToneStack) X(circuitBackend)     \
    X(circuitCabinetStyle) X(gateEnabled) X(gateThreshold) X(gateDepth) X(gateAttack)     \
    X(gateHold) X(gateRelease) X(delayMix) X(delayTime) X(delayFeedback) X(delayTone)      \
    X(reverbMix) X(reverbSize) X(reverbDamping) X(cabinetBlend) X(tunerMute)

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

    /** Hands the host the bypass control so it can drive it and keep its delay
        compensation applied.

        Returning non-null moves responsibility here: the host stops calling
        processBlockBypassed and instead sets this parameter, so processBlock owns the
        transition. It crossfades to a latency-aligned dry path rather than switching, and
        processBlockBypassed is deliberately not implemented -- it would be dead code that
        could drift away from the path that actually runs.
    */
    juce::AudioProcessorParameter* getBypassParameter() const override;

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
    /** Loads a cabinet impulse response from an audio file into slot 0 (A) or 1 (B).

        Decoding, resampling and preparation happen off the message thread; the prepared
        response is then staged into the amplifier and faded in. The decoded response is kept
        at its own sample rate so it can be re-prepared if the host changes rate.
    */
    void requestCabinetIrLoad(int slot, const juce::File& irFile);
    /** Puts the built-in response back in one slot. */
    void clearCabinetIr(int slot);
    [[nodiscard]] juce::String cabinetIrStatusText(int slot) const;
    [[nodiscard]] juce::File cabinetIrFile(int slot) const;

    /** What the tuner display should show right now.

        Detection runs off the audio thread; the callback only forwards decimated samples.
    */
    struct TunerReading
    {
        int midiNote { -1 };
        float cents {};
        float frequencyHz {};
        float confidence {};
        bool voiced {};
    };
    [[nodiscard]] TunerReading tunerReading() const noexcept;
    /** Drains the tuner's audio-thread queue and re-runs detection. Called from the shell tick. */
    void updateTuner();

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
    { return studio.reconstructionProgress(); }
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

    /** Index into the cached raw-parameter pointer table.

        Resolving a parameter by string costs a string-compare tree descent through the
        value tree state's adapter table, and the audio callback does about fifty of them
        per block. The pointers are stable for the lifetime of the value tree state, so
        they are resolved once in the constructor and indexed by this enum instead.
    */
    enum class Param : std::size_t
    {
#define TUBEFORGE_DECLARE_PARAM_ENUMERATOR(name) name,
        TUBEFORGE_RUNTIME_PARAMETERS(TUBEFORGE_DECLARE_PARAM_ENUMERATOR)
#undef TUBEFORGE_DECLARE_PARAM_ENUMERATOR
        count
    };

    /** Current value of a parameter. Allocation-free and lock-free; safe on the audio thread. */
    [[nodiscard]] float parameterOf(Param id) const noexcept
    {
        return parameterPointers[static_cast<std::size_t>(id)]->load(std::memory_order_relaxed);
    }

    /** The string id backing an enumerator, so the mapping can be verified from a test. */
    [[nodiscard]] static const char* parameterId(Param id) noexcept;

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
    [[nodiscard]] std::vector<nts::assistant::ParameterValue> assistantParameterValues() const;
    void applyAssistantParameterValues(std::span<const nts::assistant::ParameterValue> values);
    void saveAssistantPreferences() const;

    juce::AudioProcessorValueTreeState parameterState;
    std::array<std::atomic<float>*, static_cast<std::size_t>(Param::count)> parameterPointers {};
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
    // Applies the Input parameter for the engines that have no trim of their own. The
    // traditional amplifier applies it inside AmpVoice, where its calibrator measures the
    // untrimmed signal first; see the note at the call site in processBlock.
    nts::dsp::SmoothedParameter inputTrimGain;
    /// Gate for the neural and circuit engines; the traditional path keeps its own. See
    /// the note at the call site for why the two are not merged.
    nts::dsp::NoiseGate sharedGate;
    /// Delay then reverb, after the amplifier and shared by every engine: effects belong to
    /// the rig, not to one of the three ways of making the distortion.
    nts::dsp::Delay delayEffect;
    nts::dsp::Reverb reverbEffect;

    /** Live pitch tracking.

        The audio callback pushes decimated mono samples into a lock-free queue and does
        nothing else; the correlation search runs on the message thread when the editor ticks.
        Decimating to roughly 8 kHz first is what makes the search affordable -- it cuts the
        lag range by six -- and costs nothing, because a guitar's fundamental is far below the
        reduced Nyquist.
    */
    static constexpr std::size_t tunerQueueCapacity = 16384;
    static constexpr double tunerAnalysisRate = 8000.0;
    nts::diagnostics::SpscRingBuffer<float, tunerQueueCapacity> tunerQueue;
    int tunerDecimationFactor { 6 };
    int tunerDecimationCounter {};
    float tunerDecimationAccumulator {};
    nts::dsp::PitchDetector tunerDetector;
    std::vector<float> tunerFrame;
    std::size_t tunerFrameFill {};
    mutable std::mutex tunerMutex;
    TunerReading latestTunerReading;

    /** Where a mode change is in its fade-out / switch / fade-in cycle. */
    enum class EngineSwitch { idle, fadingOut, fadingIn };

    // Bypass crossfades against a dry path delayed to match the latency the host has been
    // told about, so engaging it neither clicks nor shifts the signal in time.
    nts::dsp::DelayLine dryDelay;
    nts::dsp::SmoothedParameter bypassMix;
    juce::AudioBuffer<float> dryBuffer;
    // Engine changes mute instead of crossfading: the three engines report different
    // latencies, so overlapping two of them would comb-filter rather than blend.
    nts::dsp::SmoothedParameter engineSwitchGain;
    EngineSwitch engineSwitchPhase { EngineSwitch::idle };
    int activeEngineMode {};
    // Beyond any oversampling latency the amplifier can report, so the dry path never has
    // to shorten and prepareToPlay is the only place this memory is claimed.
    static constexpr std::size_t maximumDryDelaySamples = 8192;

    /** The four factory voices, which are exactly the instrument x topology combinations.

        Exposed as host programs so a MIDI foot controller can select them, which is how these
        get switched on stage.
    */
    static constexpr int factoryProgramCount = 4;
    /// Set by the audio thread from a MIDI program change, applied on the message thread.
    std::atomic<int> pendingProgramChange { -1 };

    std::atomic<int> pendingOversamplingLatencySamples {};
    // Last factor chosen by Auto, kept so the choice has hysteresis across blocks.
    mutable std::atomic<int> autoOversamplingFactor { 4 };
    /** One cabinet slot's user-loaded response.

        The decoded audio is held at the rate it was decoded to, so a host sample-rate change
        can re-prepare it rather than leaving a response that is silently the wrong length.
    */
    struct CabinetIrSlot
    {
        juce::File file;
        nts::dsp::ImpulseResponse decoded;
        std::string status { "Built-in cabinet response" };
    };

    void applyRecoveredRig(const nts::amp::AmpParameters& rig);
    void refreshEffectParameters() noexcept;
    void applyCabinetIr(int slot);
    void restoreCabinetIrPaths(const std::string& pathA, const std::string& pathB);
    nts::ir::CabinetIrLoader cabinetIrLoader;
    mutable std::mutex cabinetIrMutex;
    std::array<CabinetIrSlot, 2> cabinetIrSlots;

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
    StudioServices studio;
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
