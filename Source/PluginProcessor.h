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
#include <nts/nam/CaptureLibrary.h>
#include <nts/pedals/PedalBoard.h>
#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/state/ProjectState.h>
#include <nts/tone/ToneProfileDatabase.h>

#include "StudioServices.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
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
    X(gateHold) X(gateRelease) X(loudnessMatch) X(cabinetWidth) X(tunerReference)          \
    X(delayMix) X(delayTime) X(delayFeedback) X(delayTone)                                 \
    X(reverbMix) X(reverbSize) X(reverbDamping) X(cabinetBlend) X(tunerMute)                \
    X(performanceTier)                                                                       \
    X(pedal1Kind) X(pedal1Bypass) X(pedal1Drive) X(pedal1Tone) X(pedal1Level) X(pedal1Mix)   \
    X(pedal2Kind) X(pedal2Bypass) X(pedal2Drive) X(pedal2Tone) X(pedal2Level) X(pedal2Mix)   \
    X(pedal3Kind) X(pedal3Bypass) X(pedal3Drive) X(pedal3Tone) X(pedal3Level) X(pedal3Mix)   \
    X(pedal4Kind) X(pedal4Bypass) X(pedal4Drive) X(pedal4Tone) X(pedal4Level) X(pedal4Mix)

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
    [[nodiscard]] juce::String modeName() const;
    [[nodiscard]] juce::String deviceStatusText() const;
    void refreshNonRealtimeDiagnostics() noexcept;
    void requestNeuralModelLoad(const juce::File& artifactDirectory);
    [[nodiscard]] juce::String neuralModelStatusText() const;
    [[nodiscard]] nts::ml::CalibrationReading neuralCalibrationReading() const noexcept
    { return neuralAmp.calibrationReading(); }
    /** Loads a converted capture into one pedal slot.

        The same artifact directory the amplifier's neural engine takes -- manifest, digest,
        normalization and test vectors -- validated on a worker the same way, and staged into
        that slot's own model host so a pedal capture and an amp capture can be loaded at once.

        There is deliberately no unload: a slot's Kind control is what takes a pedal out of
        the chain, and dropping the model as well would mean re-reading it from disk to get
        the sound back.
    */
    void requestPedalModelLoad(int slot, const juce::File& artifactDirectory);
    [[nodiscard]] juce::String pedalModelStatusText(int slot) const;
    [[nodiscard]] juce::File pedalModelFile(int slot) const;

    /** The user's collection of converted `.nam` captures.

        Reading it is message-thread work -- the editor lists it and the load buttons pick from
        it -- so it is handed out directly rather than wrapped. Importing is not: that goes
        through `requestCaptureImport` below, which does the decompressing and converting on a
        worker.
    */
    [[nodiscard]] nts::nam::CaptureLibrary& captureLibrary() noexcept { return captures; }

    /** Imports a `.zip`, a `.nam`, or a folder of either into the capture library.

        Converts everything it finds rather than asking which one, because a single pedal archive
        holds up to eighty-nine captures of the same box at different settings -- there is no
        answer to "which one" at import time, only at load time.

        `onProgress` and `onFinished` are called on the message thread. Only one import runs at a
        time; starting a second cancels the first.
    */
    void requestCaptureImport(const juce::File& source, nts::nam::Tier tier);
    void cancelCaptureImport();
    [[nodiscard]] bool captureImportRunning() const noexcept
    { return importRunning.load(std::memory_order_relaxed); }
    /// A line describing what the import is doing, or what it did.
    [[nodiscard]] juce::String captureImportStatusText() const;
    /// 0 to 1 while an import runs; 1 when none is.
    [[nodiscard]] float captureImportProgress() const noexcept
    { return importProgress.load(std::memory_order_relaxed); }

    /// The six controls a pedal slot has, in the order PedalParameters declares them.
    enum class PedalControl : std::size_t { kind, bypass, drive, tone, level, mix };
    static constexpr std::size_t pedalParameterStride = 6;
    /** The parameter id backing one slot's control.

        Shared with the editor page rather than letting it rebuild ids from the naming
        convention, because a page that invents an id the layout never registered fails at
        run time in a way nothing here would catch.
    */
    [[nodiscard]] static const char* pedalParameterId(std::size_t slot, PedalControl control) noexcept;

    /** Loads a cabinet impulse response from an audio file into slot 0 (A) or 1 (B).

        Decoding, resampling and preparation happen off the message thread; the prepared
        response is then staged into the amplifier and faded in. The decoded response is kept
        at its own sample rate so it can be re-prepared if the host changes rate.
    */
    void requestCabinetIrLoad(int slot, const juce::File& irFile);
    /** Puts the built-in response back in one slot. */
    void clearCabinetIr(int slot);
    [[nodiscard]] juce::String cabinetIrStatusText(int slot) const;
    /** Level the cabinet output loses if a mix bus folds it to mono, in dB. 0 is safe.

        Exists for the cabinet width control: two different responses split across the field are
        broadly mono-safe, but cabinet alignment becomes an inter-channel delay once width is up,
        and an inter-channel delay is a comb filter under summing. Measured from the audio, so it
        reports what the loaded responses actually do rather than what the settings imply.
    */
    [[nodiscard]] float cabinetMonoFoldDb() const noexcept
    { return traditionalAmp.cabinetMonoCompatibility(); }
    [[nodiscard]] juce::File cabinetIrFile(int slot) const;

    /** What the tuner display should show right now.

        Detection runs off the audio thread; the callback only forwards decimated samples.
    */
    struct TunerReading
    {
        int midiNote { -1 };
        float cents {};
        float frequencyHz {};
        /** Where the nearest note sits at the current reference, in Hz.

            Carried alongside the measured frequency rather than recomputed in the editor, so the
            two numbers the user compares always come from the same reference and the same reading.
        */
        float targetHz {};
        /// The A4 the reading was resolved against, so the display can state what it assumed.
        float referenceHz { 440.0f };
        float confidence {};
        bool voiced {};
    };
    [[nodiscard]] TunerReading tunerReading() const noexcept;
    /** Drains the tuner's audio-thread queue and re-runs detection. Called from the shell tick. */
    void updateTuner();
    /** Whether an editor exists to consume the display-only feeds.

        The tuner decimation and the assistant's per-block measurement both exist to fill queues
        that only the editor drains. With no editor open -- a mix session with the plug-in window
        closed, which is most of a session's running time and exactly the case a weak machine
        cares about -- nothing reads them, the queues fill, and every push is dropped. The work
        was still being done on the audio thread for every block.

        Deliberately editor *existence* rather than page visibility: the editor's timer drains
        both queues whether or not their page is showing, precisely so a reading is fresh the
        moment the page is opened. Gating on visibility would break that; gating on existence
        cannot, because with no editor there is no page to open.
    */
    void setEditorActive(bool active) noexcept
    { editorActive.store(active, std::memory_order_relaxed); }

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
    /// See StudioServices::songMatchInProgress. Drives the veil on the tone-editing pages.
    [[nodiscard]] bool songMatchInProgress() const noexcept
    { return studio.songMatchInProgress(); }
    [[nodiscard]] std::optional<nts::reconstruction::ReconstructionResult> reconstructionSnapshot() const;
    [[nodiscard]] std::vector<nts::reconstruction::PlayableRegion> reconstructionRegionsSnapshot() const;
    [[nodiscard]] bool requestReconstructionRegion(std::size_t index);
    /** Puts a recovered rig on the amplifier.

        `isolateChain` bypasses the pedals and closes the delay and reverb sends, which is
        what makes the live sound comparable to the render the candidate was ranked from:
        neither was in the offline chain, and both are in front of or behind the amplifier
        in a way that changes the tone rather than merely adding to it.
    */
    [[nodiscard]] bool applyReconstructionCandidate(std::size_t index, bool isolateChain);
    /** What the live chain still does to the rig that was last applied, if anything.

        Answered from the parameters as they stand now rather than as they stood at the
        moment of applying, so switching a pedal back on after the fact says so instead of
        leaving a panel that was true a minute ago. Empty until a candidate is applied.
    */
    [[nodiscard]] juce::StringArray reconstructionApplyWarnings() const;
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

    /** How much work the engine is allowed to do, as one explicit user-facing choice.

        Micro-optimisation alone does not make a 4096-tap dual-impulse chain at 8x oversampling
        run on a dual-core 2 GHz machine. Past a point the only remaining lever is to do less,
        and this makes that lever visible and deliberate rather than something the plug-in does
        behind the user's back.

        `standard` is the default and is what the plug-in did before this existed, except for the
        oversampling ceiling. `studio` lifts every cap. `eco` is the one that buys real headroom,
        and the row that matters most is the impulse length: a guitar cabinet's response past a
        few milliseconds is mostly room, so truncating to 256 taps is close to inaudible and is a
        sixteen-fold cut in the single most expensive operation in the plug-in.
    */
    enum class PerformanceTier { eco, standard, studio };

    struct TierLimits
    {
        int maximumOversamplingFactor {};
        std::size_t maximumImpulseTaps {};
        /// Forces cabinet blend to slot A, so only one convolution ever runs.
        bool singleCabinet {};
        /// Rational approximations in the preamp saturator and the recurrent gate activations.
        bool approximateNonlinearities {};
        /// Collapse stereo neural inference even when the two channels genuinely differ.
        bool forceNeuralMonoCollapse {};
        /// Feed the assistant's per-block measurement at all.
        bool assistantMetering {};
    };

    [[nodiscard]] static constexpr TierLimits limitsFor(PerformanceTier tier) noexcept
    {
        switch (tier)
        {
            case PerformanceTier::eco:      return { 1, 256,  true,  true,  true,  false };
            case PerformanceTier::studio:   return { 8, 4096, false, false, false, true };
            case PerformanceTier::standard:
            default:                        return { 2, 1024, false, true,  false, true };
        }
    }

    [[nodiscard]] PerformanceTier performanceTier() const noexcept;
    [[nodiscard]] TierLimits tierLimits() const noexcept { return limitsFor(performanceTier()); }


private:
    static float valueOf(const juce::AudioProcessorValueTreeState& state, const char* parameterId) noexcept;
    static void setParameterValue(juce::AudioProcessorValueTreeState& state,
                                  const char* parameterId,
                                  float plainValue);
    [[nodiscard]] nts::state::ProjectState makeProjectState() const;
    bool applyProjectState(const nts::state::ProjectState& state);
    void restorePedalboard(const nts::state::PedalboardState& board);
    /** Index of the "Auto" entry in the oversampling choice list. Appended last so
        the existing 1x/2x/4x/8x indices stay valid in saved projects.
    */
    static constexpr int automaticOversamplingIndex = 4;

    [[nodiscard]] int automaticOversamplingFactor(
        const nts::amp::AmpParameters& parameters) const noexcept;
    [[nodiscard]] nts::amp::AmpParameters currentAmpParameters() const noexcept;
    /** One pedal slot's settings, read from the six parameters that back it.

        The six are laid out contiguously per slot so the slot index can walk them, which is
        checked by a static assertion at the definition rather than trusted.
    */
    [[nodiscard]] nts::pedals::PedalParameters currentPedalParameters(std::size_t slot) const noexcept;
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
    nts::audio::CoreEngine engine;
    nts::amp::TraditionalAmpProcessor traditionalAmp;
    nts::circuit::CircuitProcessor physicalCircuit;
    nts::ml::NeuralAmpProcessor neuralAmp;
    /// One entry per (instrument, topology) pair, indexed `instrument * topologyCount + topology`.
    /// See `factoryAmpIndex`, which is the only thing allowed to do that arithmetic.
    std::array<nts::amp::AmpParameters, 2 * nts::amp::topologyCount> factoryAmpParameters;
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
    /** The engine's input, kept so the gate can key off it while attenuating the output.

        Sized in prepareToPlay, so the audio thread only copies into it. See runSharedGate for why
        the detector and the attenuation have to be on opposite sides of the amplifier.
    */
    std::vector<float> gateSidechain;
    /// Delay then reverb, after the amplifier and shared by every engine: effects belong to
    /// the rig, not to one of the three ways of making the distortion.
    nts::dsp::Delay delayEffect;
    nts::dsp::Reverb reverbEffect;
    /// Pedals, ahead of the amplifier and shared by every engine for the same reason. This is
    /// where a board sits on a real rig, and putting it anywhere else would stop a boost from
    /// doing the one thing a boost is for -- driving the amplifier's own front end harder.
    nts::pedals::PedalBoard pedalBoard;

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

    /** The factory voices, which are exactly the instrument x topology combinations.

        Exposed as host programs so a MIDI foot controller can select them, which is how these
        get switched on stage. Derived from `topologyCount` rather than written down: a voicing
        appended to the enum should appear on the foot controller without anyone remembering to
        come here, and a stale literal would leave the last topologies unreachable by MIDI.
    */
    static constexpr int factoryProgramCount = 2 * static_cast<int>(nts::amp::topologyCount);

    /// The one place `instrument * topologyCount + topology` is written. Both indices are
    /// clamped, so a parameter value out of range picks a real voice instead of reading past
    /// the end of the table.
    [[nodiscard]] static std::size_t factoryAmpIndex(int instrumentIndex, int topologyIndex) noexcept
    {
        const auto instrument = std::clamp(instrumentIndex, 0, 1);
        const auto topology = std::clamp(topologyIndex, 0, static_cast<int>(nts::amp::topologyCount) - 1);
        return static_cast<std::size_t>(instrument) * nts::amp::topologyCount
             + static_cast<std::size_t>(topology);
    }
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

    void applyRecoveredRig(const nts::amp::AmpParameters& rig, bool isolateChain);
    /** The rig a Song Match candidate last put on the amplifier.

        Kept so the chain warnings can be answered against live parameter values whenever
        they are asked for. Message thread only, which is where both the applying and the
        asking happen, so it needs no lock.
    */
    std::optional<nts::amp::AmpParameters> appliedRecoveredRig;
    void refreshEffectParameters() noexcept;
    void applyCabinetIr(int slot);
    void restoreCabinetIrPaths(const std::string& pathA, const std::string& pathB);
    nts::ir::CabinetIrLoader cabinetIrLoader;
    mutable std::mutex cabinetIrMutex;
    std::array<CabinetIrSlot, 2> cabinetIrSlots;

    /** One pedal slot's loaded capture.

        Kept beside the board rather than inside it because a path and a status line are
        message-thread bookkeeping: the board itself only ever needs the staged model.
    */
    struct PedalModelSlot
    {
        juce::File file;
        std::string status { "No pedal capture loaded" };
    };
    mutable std::mutex pedalModelMutex;
    std::array<PedalModelSlot, nts::pedals::slotCount> pedalModelSlots;
    /// One worker per slot, so loading a capture into slot 3 cannot cancel slot 1's.
    std::array<std::jthread, nts::pedals::slotCount> pedalLoaders;

    /// Converted `.nam` captures, and the worker that fills it. The library itself is only ever
    /// touched from the message thread; the worker hands its report back through the status
    /// strings below.
    nts::nam::CaptureLibrary captures;
    std::jthread captureImporter;
    std::atomic<bool> importRunning {};
    /// See setEditorActive. Read once per block from the audio thread.
    std::atomic<bool> editorActive {};

    /** Automatic tier reduction when the machine cannot keep up.

        Steps **down** only. A user who chose Studio chose it, and silently putting their tone
        back when a transient load passes would be worse than the dropout it avoided -- so
        recovery is theirs to ask for.

        Detected on the audio thread, applied on the message thread: changing a tier means
        touching parameter objects, which is not audio-thread work. The request crosses through
        an atomic and triggerAsyncUpdate, which is the same route the MIDI program change takes,
        and it works whether or not an editor exists -- which matters, because a machine in
        trouble is most likely one running with the window closed.
    */
    static constexpr int overloadChecksBeforeStepDown = 8;
    static constexpr int automaticTierCheckBlocks = 32;
    static constexpr double overloadLoadPercent = 75.0;
    std::atomic<int> automaticTierRequest { -1 };
    std::uint64_t lastSeenDropoutCount {};
    int automaticTierCheckCounter {};
    int overloadStreak {};
    void considerAutomaticTierReduction() noexcept;
    /// Scratch for the tuner decimation's read pointers, so the loop does not re-derive them.
    std::array<const float*, nts::audio::MeterState::maximumChannels> tunerReadPointers {};
    /// Set by the worker, consumed on the message thread: the shared library's list is rebuilt
    /// there so a page drawing it never sees it change underneath.
    std::atomic<bool> pendingCaptureRefresh {};
    std::atomic<float> importProgress { 1.0f };
    mutable std::mutex importStatusMutex;
    std::string importStatus { "No captures imported yet" };

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
