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
#include <nts/ir/CabinetLibrary.h>
#include <nts/ir/CabinetModel.h>
#include <nts/diagnostics/LatencyBudget.h>
#include <nts/diagnostics/StructuredLogger.h>
#include <nts/ecosystem/TonePackage.h>
#include <nts/ml/NeuralAmpProcessor.h>
#include <nts/nam/CaptureLibrary.h>
#include <nts/pedals/PedalBoard.h>
#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/state/ProjectState.h>
#include <nts/tone/ToneProfileDatabase.h>

#include "AutoMatch.h"
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
    X(crossover) X(cleanBlend) X(dryBlend) X(panelSwitch)                                 \
    X(lowBandDrive) X(lowBandLevel) X(highBandLevel)                                      \
    X(cabinetAlignment) X(tightness) X(pickEmphasis)                                      \
    X(engineMode) X(neuralMonitor) X(neuralCompensation) X(circuitPreampTube)             \
    X(circuitPowerTube) X(circuitPowerTopology) X(circuitToneStack) X(circuitBackend)     \
    X(circuitCabinetStyle) X(gateEnabled) X(gateThreshold) X(gateDepth) X(gateAttack)     \
    X(gateHold) X(gateRelease) X(loudnessMatch) X(cabinetWidth) X(tunerReference)          \
    X(delayMix) X(delayTime) X(delayFeedback) X(delayTone)                                 \
    X(reverbMix) X(reverbSize) X(reverbDamping) X(cabinetBlend) X(tunerMute)                \
    X(performanceTier)                                                                       \
    X(pedal1Kind) X(pedal1Bypass) X(pedal1Drive) X(pedal1Tone) X(pedal1Level) X(pedal1Mix)   \
    X(pedal1AuxA) X(pedal1AuxB)   \
    X(pedal2Kind) X(pedal2Bypass) X(pedal2Drive) X(pedal2Tone) X(pedal2Level) X(pedal2Mix)   \
    X(pedal2AuxA) X(pedal2AuxB)   \
    X(pedal3Kind) X(pedal3Bypass) X(pedal3Drive) X(pedal3Tone) X(pedal3Level) X(pedal3Mix)   \
    X(pedal3AuxA) X(pedal3AuxB)   \
    X(pedal4Kind) X(pedal4Bypass) X(pedal4Drive) X(pedal4Tone) X(pedal4Level) X(pedal4Mix)   \
    X(pedal4AuxA) X(pedal4AuxB)                                                               \
    /* Not read by the audio path -- Auto Match decides who writes the other parameters, and  \
       nothing in processBlock cares. It is here because every registered parameter has to be \
       reachable through this table, which `nts_wrapper_tests` checks, and because reading it \
       from the guard is then one atomic load instead of a string lookup. */                  \
    X(autoMatch) X(autoMatchTracking)                                                          \
    /* The cabinet stage. Read every block by `currentCabinetParameters`, which is why they are \
       here rather than only in the layout. See ParameterIds for why they are named `cab*` and  \
       not `cabinet*`. */                                                                       \
    X(cabLevelA) X(cabLevelB) X(cabPanA) X(cabPanB) X(cabPhaseA) X(cabPhaseB)                   \
    X(cabDelayA) X(cabMuteA) X(cabMuteB) X(cabLowCut) X(cabHighCut) X(cabDiBlend)               \
    X(cabOutputTrim)                                                                            \
    /* The built-in cabinet model. Read from the audio thread only to hash them -- rendering one \
       is message-thread work; see TubeForgeAudioProcessor::cabinetModelHash. */                \
    X(cabModelA) X(cabModelB) X(cabMicA) X(cabMicB)                                             \
    X(cabPositionA) X(cabPositionB) X(cabDistanceA) X(cabDistanceB)                             \
    /* How a loaded response is prepared. Read on the message thread when one is applied, and   \
       hashed on the audio thread so a change is noticed -- see cabinetModelHash. */            \
    X(cabIrLengthA) X(cabIrLengthB) X(cabIrNormA) X(cabIrNormB)                                 \
    X(cabIrMinPhaseA) X(cabIrMinPhaseB)

class TubeForgeAudioProcessor final : public juce::AudioProcessor,
                                      private juce::AsyncUpdater,
                                      private juce::AudioProcessorParameter::Listener
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

    /** Loads the selected voicing's own values into every control that owns one.

        The amplifier's "start from here". Without it a voicing only supplies the parameters no
        knob owns, so switching to a bass amplifier keeps the previous rig's gain, cuts and
        tightness -- and `tightness` alone can put a 345 Hz high-pass in front of the distortion.

        Instrument and voicing are kept; everything else goes back to what that amplifier
        specifies. The pedalboard and the sends are left alone, because they are the player's own
        work and are not part of what an amplifier is.
    */
    void loadVoicingDefaults();

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
    /** The eight parameters a slot exposes.

        `auxA` and `auxB` are the model-specific voicing controls -- a two-band EQ, a bias, a
        blend -- and they are named generically on purpose. A `juce::AudioParameterFloat`'s name
        is fixed at construction, so a per-model name would mean registering every model's
        controls for every slot. The host therefore sees "Pedal 1 Aux B" while the interface,
        where the user actually turns the knob, shows what the model calls it.
    */
    enum class PedalControl : std::size_t { kind, bypass, drive, tone, level, mix, auxA, auxB };
    static constexpr std::size_t pedalParameterStride = 8;
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
    /** As above, checking the loaded file against a digest a project recorded.

        A mismatch does not refuse the file -- it is still the response the user pointed at -- but
        it is reported in the slot's status line, because a pack updated in place is exactly the
        kind of thing that silently re-voices a finished mix.
    */
    void requestCabinetIrLoad(int slot, const juce::File& irFile, const std::string& expectedDigest);
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
    { return cabinetStage.monoCompatibility(); }
    [[nodiscard]] juce::File cabinetIrFile(int slot) const;
    /// The response currently sounding in a slot, for the page's own display.
    [[nodiscard]] const nts::amp::CabinetMetadata& cabinetMetadata(int slot) const noexcept
    { return slot == 0 ? cabinetStage.metadataA() : cabinetStage.metadataB(); }

    /** The delay that best lines slot B up behind slot A, in samples.

        Cross-correlates the two loaded responses and returns the lag of the strongest positive
        peak, which is the measurement the alignment control has never had behind it. Two
        microphones at different distances from a speaker are at different times, and a few
        samples of that difference is the whole tonal difference between a blend that sounds like
        one cabinet and one that sounds hollow -- but nothing in the plug-in could tell the user
        what the number was, so the control was a slider to be waggled.

        Negative lags are not reachable: only slot B carries the section's alignment, so a pair
        where A is the *late* one needs slot A's own delay instead. Returns 0 for that case, and
        `alignmentPutsSlotAfirst` says which of the two the caller should be writing.

        Message thread only. Reads the decoded responses under `cabinetIrMutex`, and synthesises
        the built-in ones, so it never touches what the audio thread is convolving.
    */
    struct CabinetAlignment
    {
        /// Samples to delay the later slot by. Zero when the two already line up.
        int delaySamples {};
        /// True when it is slot **A** that arrives early and so wants the delay.
        bool delaySlotA {};
        /// Peak correlation, 0 to 1. Low means the two responses have little in common and the
        /// suggested lag is not worth much -- which the interface says rather than hiding.
        float confidence {};
    };
    [[nodiscard]] CabinetAlignment suggestedCabinetAlignment() const;

    /** The cabinet's magnitude response, for the page to draw.

        Log-spaced from 40 Hz to 16 kHz, in dB, and **cached rather than computed on demand**: a
        slot holding a user impulse response needs a transform of it, which is not something to do
        twenty times a second behind a repaint. Recomputed when a response is loaded or cleared and
        when the built-in model moves, which is exactly when it can change.

        Why this matters more than it sounds: every other control on the Cabinet page changes a
        filter, and until now the only way to find out what any of them did was to play through
        them. Two microphone positions that look like "0.25" and "0.65" are a 6 dB difference at
        3 kHz, and that is the sort of thing a picture says in one glance.
    */
    static constexpr std::size_t cabinetResponsePoints = 192;
    [[nodiscard]] static float cabinetResponseFrequency(std::size_t index) noexcept;
    // ---- Cabinet Match ----------------------------------------------------------------
    //
    // Fitting the cabinet to the song the rig was matched from. See docs/cabinet-plan.md, Track F,
    // and nts/ir/CabinetMatch.h for why this is a filter fit rather than a search.

    struct CabinetMatchOutcome
    {
        bool applied {};
        /// What the fit is worth, 0 to 1. See `nts::ir::CabinetMatchResult::confidence`.
        float confidence {};
        /// The closest built-in cabinet to the correction, for the interface to name.
        juce::String nearestCabinet;
        /// Empty on success; otherwise why nothing was applied.
        juce::String error;
    };

    /** Fits a cabinet to the last reconstruction's reference and loads it into slot A.

        **What this actually produces, stated plainly because the interface has to repeat it.**
        The reconstruction has no separate DI -- it renders the reference through each candidate
        and scores the result, and says so in its own warnings. So the residual measured here is
        the difference between *this rig playing that reference* and *the reference itself*, and a
        cabinet built from it therefore absorbs the amplifier's difference, the microphone, the
        room and the mastering EQ along with the speaker. That is a corrective filter for this rig
        against that record, which is a genuinely useful thing and is not the same claim as "this
        is the cabinet on the record".

        `depth` scales the correction from 0 to 1. Message thread: it renders offline and runs
        transforms.
    */
    [[nodiscard]] CabinetMatchOutcome matchCabinetToReference(float depth);
    /// True when a reconstruction has left a reference for `matchCabinetToReference` to fit to.
    [[nodiscard]] bool cabinetMatchAvailable() const;

    /** Where a matched cabinet stands with respect to Auto Match.

        The cabinet is the one thing a match produces that is **not a parameter**, and ownership of
        a parameter is the only kind the guard understands: it watches for change gestures on
        registered controls, and an impulse response has none. So this is the asset half of the
        same idea, and it has exactly one rule -- anything that replaces what is in slot A releases
        it. That is enough, because there is no partial state: the matched response is either still
        the one sounding or it is not.

        Deliberately **not saved with the project**. A matched cabinet is generated rather than
        loaded, so a reopened project has nothing to restore it from, and coming back claiming to
        hold a response that is no longer there would be a worse lie than coming back holding
        nothing. A recall clears the hold anyway; this clears with it.
    */
    enum class CabinetHold
    {
        /// Auto Match has not put a cabinet in slot A, or it has stopped holding the rig.
        none,
        /// The matched cabinet is what slot A is sounding.
        held,
        /// It was replaced -- a file loaded, the slot cleared, or the model changed under it.
        released
    };
    [[nodiscard]] CabinetHold cabinetHoldState() const noexcept
    { return cabinetHold.load(std::memory_order_acquire); }

    /** The user's folder of impulse responses, as a browsable list.

        Handed out directly rather than wrapped: reading and filtering it is message-thread work
        that only the picker does, and every path that reaches audio goes through
        `requestCabinetIrLoad` exactly as a file dialog's result does.
    */
    [[nodiscard]] nts::ir::CabinetLibrary& cabinetLibrary() noexcept { return cabinets; }

    /** Writes one slot's response to a 24-bit WAV at the session rate.

        The reason this exists is Cabinet Match: a fitted cabinet is *generated*, so without an
        export it lives only inside one project and cannot be taken to another track, another
        plug-in or another machine. It also covers the ordinary case of wanting to know what a
        slot is actually convolving — a model rendered at a chosen length is a perfectly good
        impulse response, and there is no reason to keep it locked in here.
    */
    [[nodiscard]] juce::Result exportCabinetResponse(int slot, const juce::File& destination) const;

    /// One slot's own response, before the blend, the levels and the section's two cuts.
    [[nodiscard]] std::vector<float> cabinetResponseCurve(int slot) const;
    /// What the section as a whole does: both slots at their levels, blended, through the cuts.
    /// This is the curve that corresponds to what is heard.
    [[nodiscard]] std::vector<float> cabinetSumResponseCurve() const;

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

    // ---- Auto Match -------------------------------------------------------------------
    //
    // The analyzer holds the rig. See docs/auto-match-plan.md for the design, and
    // Source/AutoMatch.h for the set of controls a matched rig owns.

    void setAutoMatchEnabled(bool enabled);
    [[nodiscard]] bool autoMatchEnabled() const noexcept;
    [[nodiscard]] tf::automatch::State autoMatchState() const noexcept;
    /// True while this control is being written and held by a matched rig.
    [[nodiscard]] bool autoMatchOwns(std::string_view parameterId) const noexcept;
    /// True for an owned control the user -- or the host -- has taken back.
    [[nodiscard]] bool autoMatchReleased(std::string_view parameterId) const noexcept;
    /// What the match put on a control, for the "keep the matched value" answer and for the
    /// dialog's own text. Empty when nothing is held.
    [[nodiscard]] std::optional<float> autoMatchValueOf(std::string_view parameterId) const noexcept;
    /// Hands one control back to the user. Its current value is left exactly as it is.
    void releaseAutoMatchParameter(std::string_view parameterId);
    /// Puts the matched value back on one control and resumes holding it.
    void restoreAutoMatchParameter(std::string_view parameterId);
    /// Puts the whole matched rig back and clears every release.
    void reclaimAllAutoMatchParameters();
    /// How many owned controls have been taken back, for the interface's status line.
    [[nodiscard]] int autoMatchReleasedCount() const noexcept;
    [[nodiscard]] juce::String autoMatchStatusText() const;
    /// The song the held rig was matched from, or empty. For the dialog's own wording.
    [[nodiscard]] juce::String autoMatchSourceName() const;

    /** The next control a user gesture has touched while Auto Match was holding it, or -1.

        Polled from the editor's timer, and clears itself: this is a *request for a dialog*, and
        the request has to be consumed exactly once whether or not a dialog ends up being shown.
        Deliberately a poll rather than a callback, because it is raised from
        `parameterGestureChanged` -- which a host can call on the audio thread, where opening a
        window is not merely slow but a deadlock.
    */
    [[nodiscard]] int takeAutoMatchWarning() noexcept;
    /// Index into `tf::automatch::owned` -> the parameter's display name, for that dialog.
    [[nodiscard]] juce::String autoMatchParameterName(int ownedIndex) const;
    /// The matched value, formatted the way the panel shows it.
    [[nodiscard]] juce::String autoMatchValueText(int ownedIndex) const;
    /// Controls released by something other than the user -- host automation, a program change.
    /// Cleared when the interface has reported them.
    [[nodiscard]] juce::StringArray takeAutoMatchExternalReleases();

    /** Applies a finished match, if Auto Match is on and one has arrived since the last look.

        This is what makes the mode automatic rather than merely sticky: with the switch on, a
        completed search puts its winning candidate on the amplifier without the user pressing
        Apply. Called from the editor's tick beside `refreshAssistant`, because writing host
        parameters is message-thread work and a search can only be started from an open editor
        anyway.

        Controls the user has taken back are carried across a re-derivation rather than being
        reclaimed by it -- a match re-running because a region changed must not quietly undo the
        override the user set two minutes ago.
    */
    void refreshAutoMatch();
    /// Tells Auto Match whether a re-derivation should also isolate the chain. Pushed from the
    /// Song Match page's own switch so the automatic apply matches what pressing Apply would do.
    void setAutoMatchIsolatesChain(bool isolate) noexcept { autoMatchIsolatePreference = isolate; }

    /** Puts a rig on the amplifier the way a re-derivation does, keeping the user's overrides.

        The difference from `applyRecoveredRig` is exactly one thing, and it is the thing that
        makes a re-run tolerable: a control the user has taken back keeps the value they gave it
        and stays taken back. Pressing Apply goes through `applyRecoveredRig` instead, because
        choosing a different candidate by hand *is* a new rig and the old overrides were about a
        different one.
    */
    void applyAutoMatchRig(const nts::amp::AmpParameters& rig, bool isolateChain);

    [[nodiscard]] bool autoMatchTracking() const noexcept;
    void setAutoMatchTracking(bool enabled);

    /** Whether the user has asked not to be warned again, across sessions.

        Stored beside the other TubeForge preferences rather than in the project: it is an answer
        about how much explaining they want, not about a rig. Suppressing the question never
        suppresses the *change* -- a control the user moves is still handed back to them.
    */
    [[nodiscard]] bool autoMatchWarningsSuppressed() const noexcept
    { return autoMatchSuppressWarnings; }
    void setAutoMatchWarningsSuppressed(bool suppressed);

    /** Puts a recovered amplifier on the host parameters and starts holding it.

        Public because it is the operation "make this rig the one that is playing", which the
        studio reaches through a callback and the test suite has to be able to reach directly:
        every guarantee this makes -- that the Gain macro is neutralised, that a leftover panel
        switch cannot re-voice the result, that what was written is recorded -- is only checkable
        by calling it. It is not a shortcut around `applyReconstructionCandidate`, which does the
        candidate bookkeeping this deliberately knows nothing about.
    */
    void applyRecoveredRig(const nts::amp::AmpParameters& rig, bool isolateChain);
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
    /** Writes the current rig as a shareable `.ntone` package.

        `includeCabinets` copies any impulse response the two slots have loaded **into** the
        package, so the rig arrives complete on a machine that has never seen those files.

        **Off by default, and that is a rights decision rather than a default nobody thought
        about.** Most impulse responses people load are commercial, and their licences generally
        permit use rather than redistribution -- so embedding one in a profile the user then shares
        is a redistribution they may have no right to make. The plug-in cannot read a licence and
        will not guess at one, so it asks, and the answer it assumes when nobody has answered is
        the one that cannot get anybody into trouble.
    */
    [[nodiscard]] juce::Result exportCurrentTonePackage(const juce::File& destination,
                                                        const juce::String& name,
                                                        const juce::String& author,
                                                        bool includeCabinets = false) const;
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
        /** What the pedalboard is meant to carry, as a sum of model tiers.

            Advisory, not enforced. A user who deliberately puts four expensive pedals in front
            of the amplifier gets told the tier is not meant to carry it, and then gets what
            they asked for -- silently degrading a pedal somebody chose is the worse failure,
            and the one a load-watching heuristic makes on its own.
        */
        int pedalCostBudget {};
    };

    [[nodiscard]] static constexpr TierLimits limitsFor(PerformanceTier tier) noexcept
    {
        switch (tier)
        {
            case PerformanceTier::eco:      return { 1, 256,  true,  true,  true,  false, 6 };
            case PerformanceTier::studio:   return { 8, 4096, false, false, false, true, 20 };
            case PerformanceTier::standard:
            default:                        return { 2, 1024, false, true,  false, true, 12 };
        }
    }

    [[nodiscard]] PerformanceTier performanceTier() const noexcept;
    [[nodiscard]] TierLimits tierLimits() const noexcept { return limitsFor(performanceTier()); }

    /// What the board currently costs against `TierLimits::pedalCostBudget`. Both are advisory;
    /// see that field. Read from the editor to say so, never to change what is played.
    [[nodiscard]] int pedalboardCost() const noexcept;


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
    /** `currentAmpParameters` with the amplifier's own cabinet switched out.

        The cabinet is a stage of its own now (`cabinetStage`), sitting after whichever engine is
        selected, so the copy inside `AmpVoice` must not also run or the traditional path would be
        convolved twice. Kept as a separate accessor rather than folded into
        `currentAmpParameters` because that function answers "what does this rig say", which every
        other caller -- the assistant, preset export, the reconstruction warnings -- still needs to
        include a cabinet.
    */
    [[nodiscard]] nts::amp::AmpParameters liveAmpParameters() const noexcept;
    /** What the cabinet stage should be doing, read from the host parameters.

        The four controls that predate the stage (`cabinet`, `cabinetBlend`, `cabinetWidth`,
        `cabinetAlignment`) plus the thirteen in `cabinetControlIds`. Allocation-free; called once
        per block from the audio thread.
    */
    [[nodiscard]] nts::amp::CabinetParameters currentCabinetParameters() const noexcept;
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
    /** The speaker, after whichever engine is selected and shared by all three.

        **This is a stage, not a member of the amplifier, and the difference is audible.** While
        the cabinet lived inside `AmpVoice` only the traditional engine had one: a Neural Amp
        Modeler capture of a preamp or a pedal -- which is most of what people share -- played
        into the output with no speaker in front of it, and no control in the plug-in could put
        one there. The physical circuit engine had a *different* cabinet, a graph node, so a
        loaded impulse response did nothing on it at all.

        `AmpVoice` keeps its own instance for offline rendering, where the reconstruction needs a
        cabinet in the render and cannot be handed a live audio object; on the live path that
        copy is bypassed by `liveAmpParameters`.
    */
    nts::amp::CabinetSection cabinetStage;
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
        /** SHA-256 of the file as it was read, so a project can say whether the response on disk
            is still the one it was saved with. Computed once at load; see `makeProjectState`. */
        std::string digest;
        /// True when this project recorded a different digest for the same path.
        bool changedSinceSaved {};
    };

    // ---- Auto Match internals ---------------------------------------------------------

    /** Marks a stretch of parameter writes as *not the user*.

        `setParameterValue` issues real begin/end change gestures, exactly as a knob drag does,
        so without this the guard cannot tell a matched rig being applied from a player reaching
        for a control -- and Auto Match would raise a warning dialog about its own writes. Every
        programmatic bulk write is wrapped: applying a rig, recalling a project or a preset,
        accepting an assistant action, and a MIDI program change.

        A counter rather than a flag because those paths nest: applying a tone package recalls a
        project state, which writes the same parameters again.
    */
    class AutoWriteScope
    {
    public:
        explicit AutoWriteScope(TubeForgeAudioProcessor& owner) noexcept : processor(owner)
        { processor.autoWriteDepth.fetch_add(1, std::memory_order_acq_rel); }
        ~AutoWriteScope() { processor.autoWriteDepth.fetch_sub(1, std::memory_order_acq_rel); }
        AutoWriteScope(const AutoWriteScope&) = delete;
        AutoWriteScope& operator=(const AutoWriteScope&) = delete;
    private:
        TubeForgeAudioProcessor& processor;
    };

    /// Resolves the owned table against the real parameters and starts listening. Called from
    /// the constructor; the guard runs whether or not an editor is open, because a host writing
    /// an automation lane has to release the control it wrote even with the window closed.
    void attachAutoMatchGuard();
    void detachAutoMatchGuard();
    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;
    /// Position in `tf::automatch::owned` of a host parameter index, or -1. Allocation-free and
    /// lock-free: both listener callbacks can arrive on the audio thread.
    [[nodiscard]] int autoOwnedIndexOf(int parameterIndex) const noexcept;
    /// True when this owned control is currently being held and so may be warned about.
    [[nodiscard]] bool autoMatchGuarding(int ownedIndex) const noexcept;
    /// Records what a matched rig just put on every owned control. Called from
    /// `applyRecoveredRig` once the writes are done.
    void captureAutoMatchSnapshot(bool isolatedChain);
    /** Stops holding, without touching a single parameter value.

        Called wherever something replaces the whole rig -- a project, a profile, a program
        change. The switch stays on and the state falls back to `armed`, because what those
        paths invalidate is the *match*, not the user's wish to have one: the snapshot no longer
        describes what is playing, so continuing to guard against it would be guarding a rig
        that is not there.
    */
    void clearAutoMatchHold() noexcept;
    /// What is being held, for the project file. Empty `heldValues` when nothing is.
    [[nodiscard]] nts::state::AutoMatchState autoMatchProjectState() const;
    /** Puts a saved hold back.

        Values only -- the parameters themselves have already been restored from `ampControls` by
        the time this runs, so re-writing them would be redundant and would fight a project whose
        controls were edited after the match. What this restores is the *knowledge* of which
        values the analyzer put there and which controls the user had taken back.

        A saved rig from a build with a different owned set is refused rather than mapped: the
        values are positional, so a shorter or longer list means the table moved and applying it
        would guard the wrong controls with the wrong numbers.
    */
    void restoreAutoMatchProjectState(const nts::state::AutoMatchState& saved);

    /// Host parameter index of each entry in `tf::automatch::owned`, resolved once at
    /// construction. -1 would mean an id in that table names no real parameter, which the
    /// constructor asserts on rather than leaving as a control that is silently never guarded.
    std::array<int, tf::automatch::ownedCount> autoOwnedParameterIndices {};
    std::array<float, tf::automatch::ownedCount> autoMatchValues {};
    std::atomic<int> autoWriteDepth {};
    /// Which owned controls a matched rig is currently holding. Zero means nothing is held,
    /// which is what distinguishes `armed` from `holding`.
    std::atomic<std::uint64_t> autoHeldMask {};
    std::atomic<std::uint64_t> autoReleasedMask {};
    /// Which owned controls have an open change gesture. A value change with no gesture behind
    /// it did not come from a person, so it releases silently instead of raising a dialog.
    std::atomic<std::uint64_t> autoGestureMask {};
    std::atomic<std::uint64_t> autoExternalReleaseMask {};
    std::atomic<int> autoWarningRequest { -1 };
    /** A pending "the instrument changed, re-run the match" request: the new instrument index,
        or -1. Latched by the guard and acted on from `handleAsyncUpdate`, because the guard can
        be running on the audio thread and starting a worker there is not allowed.
    */
    std::atomic<int> autoMatchInstrumentRequest { -1 };
    /// The reconstruction generation Auto Match has already applied. See `refreshAutoMatch`.
    std::uint64_t autoMatchAppliedGeneration {};
    /// Whether the last auto-applied candidate took the effects chain with it. Follows the Song
    /// Match page's own switch, which is pushed in rather than read out of the interface.
    bool autoMatchIsolatePreference { true };
    /// Loaded once at construction and written when it changes; see autoMatchWarningsSuppressed.
    bool autoMatchSuppressWarnings {};
    /// Rate limit for live tracking. A trim that moves at the tick rate would fill a host's
    /// automation lane with gestures and would read as a knob with a fault.
    std::chrono::steady_clock::time_point lastAutoMatchTracking {};
    /// Follows the live signal within the bounds the match set. See `updateAutoMatchTracking`.
    void updateAutoMatchTracking();
    void loadAutoMatchPreferences();
    mutable std::mutex autoMatchMutex;
    std::string autoMatchSource;
    bool autoMatchIsolatedChain {};

    /** The rig a Song Match candidate last put on the amplifier.

        Kept so the chain warnings can be answered against live parameter values whenever
        they are asked for. Message thread only, which is where both the applying and the
        asking happen, so it needs no lock.
    */
    std::optional<nts::amp::AmpParameters> appliedRecoveredRig;
    void refreshEffectParameters() noexcept;
    void applyCabinetIr(int slot);
    /// Puts the built-in response back in one slot of the cabinet stage: the model rendered from
    /// that slot's own controls, or the original samples when the model is set to Legacy.
    void restoreBuiltInCabinet(int slot);
    /** What the built-in model is being asked for in one slot, read from the host parameters. */
    [[nodiscard]] nts::ir::CabinetModelSettings currentCabinetModel(int slot) const noexcept;
    /** Re-derives what both cabinet slots are sounding: the built-in model, or a re-prepared
        user response for a slot that holds one.

        Message-thread work: it runs transforms and allocates. Reached from `handleAsyncUpdate`
        when the audio thread notices any of those controls have moved, which is the same route
        the physical circuit's recompile takes and for the same reason.
    */
    void refreshCabinetResponses();
    /** Hash of every control that decides what the two slots sound like -- the built-in model and
        the preparation of a loaded response -- so the audio thread can notice a change without
        doing anything about it. Mirrors `physicalCircuitControlHash`. */
    [[nodiscard]] std::uint64_t cabinetModelHash() const noexcept;
    std::atomic<std::uint64_t> renderedCabinetModelHash {};
    /// See `cabinetHoldState`. Written from the message thread, read from the editor's tick.
    std::atomic<CabinetHold> cabinetHold { CabinetHold::none };
    /** Marks slot A's matched cabinet as replaced, if one was being held.

        Called from every path that writes slot A other than the match itself. A no-op unless
        something was held, so callers do not have to know whether it was.
    */
    void releaseCabinetHold() noexcept;
    /// Recomputes `cabinetCurves` for one slot from whatever it is now sounding.
    void refreshCabinetResponseCurve(int slot);
    /// Guarded by `cabinetIrMutex`, which already guards the responses these are derived from.
    std::array<std::vector<float>, 2> cabinetCurves;
    /** Puts saved responses back, and says so when what is on disk has changed underneath.

        `hashA`/`hashB` are the digests recorded when the project was saved, empty for a project
        written before schema 6. A mismatch is not an error and does not refuse the file -- it is
        still the response the user pointed at -- but it is said out loud, because an impulse pack
        updated in place is exactly the kind of thing that silently re-voices a finished mix.
    */
    void restoreCabinetIrPaths(const std::string& pathA, const std::string& pathB,
                               const std::string& hashA = {}, const std::string& hashB = {});
    nts::ir::CabinetIrLoader cabinetIrLoader;
    /// See cabinetLibrary(). Scanned from a folder the user chooses, never copied.
    nts::ir::CabinetLibrary cabinets;
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

    /** A loaded capture that already contains a speaker, waiting to switch the cabinet off.

        The cabinet became a stage shared by all three engines, which is what makes a preamp-only
        or pedal capture usable -- and which would put a second speaker in front of a **full-rig**
        capture, that being a capture of an amplifier *and* its cabinet. Two cabinets in series is
        not a subtle error; it is the dull, honking sound people describe when they double up.

        So a capture whose author labelled it `fullRig` switches the cabinet off as it loads, and
        says so on the page. Only that direction is automatic: switching the cabinet back *on* for
        an amp capture would overrule a user who turned it off deliberately, and being wrong in
        that direction is worse than leaving a control where they put it.

        Latched here because `loadNeuralArtifact` runs on a worker and writing a host parameter is
        message-thread work; consumed in `handleAsyncUpdate`, which is the route the MIDI program
        change already takes for the same reason.
    */
    std::atomic<bool> pendingFullRigCabinetBypass {};
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
