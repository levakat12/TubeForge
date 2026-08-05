#pragma once

#include <nts/dsp/Common.h>
#include <nts/dsp/Convolution.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Filters.h>
#include <nts/dsp/ModeCrossfader.h>
#include <nts/dsp/Oversampling.h>
#include <nts/dsp/Smoothing.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nts::amp
{
enum class Instrument { guitar, bass };
enum class PickupProfile { passive, active };

/** The amplifier voicings, each a starting point for every parameter below.

    A topology is not a switch inside the DSP: it selects a block of values in
    `makeOriginalPreset` and nothing else, so a voicing is reachable by hand from any other and
    adding one costs no audio code. What distinguishes them is which corners of the parameter
    space they occupy -- number of gain stages, where the supply sags and how fast it recovers,
    how much global feedback the power section runs, and whether the tone stack is passive.

    The order is part of the saved state and of the host-visible choice parameter, so new
    voicings are **appended** rather than inserted. `tightModern` and `vintageBloom` must keep
    indices 0 and 1 or every project saved before the others existed recalls a different amp.
*/
enum class Topology
{
    tightModern,
    vintageBloom,
    americanClean,
    britishCrunch,
    classAChime,
    saggingRectifier,
    studioDirect
};

inline constexpr std::size_t topologyCount = 7;

/** The name a topology is written under in a `.ntone` preset. Stable across releases.

    Separate from `topologyName` because one is a file format and the other is a label. Renaming
    a voicing in the interface must not silently orphan every preset that referenced it.
*/
[[nodiscard]] std::string_view topologyKey(Topology topology) noexcept;

/// The name a topology is shown under. Feeds the host's choice parameter, so the plug-in and
/// the engine cannot disagree about what voicing index 3 is.
[[nodiscard]] std::string_view topologyName(Topology topology) noexcept;

/** Parses a `topologyKey`. Empty for an unknown name rather than a guess.

    A preset written by a later build naming a voicing this one has never heard of is the case
    that matters: the caller falls back to `tightModern` and loads the rest of the file, which
    keeps a forward-dated preset partially useful instead of wholly rejected.
*/
[[nodiscard]] std::optional<Topology> topologyFromKey(std::string_view key) noexcept;

enum class ToneStackType { passiveCoupled, activeThreeBand, bassSemiParametric };

struct CalibrationProfile
{
    float targetRmsLowDb { -24.0f };
    float targetRmsHighDb { -18.0f };
    float targetPeakDb { -12.0f };
    PickupProfile pickup { PickupProfile::passive };
};

struct CalibrationReading
{
    float peakDb { -120.0f };
    float rmsDb { -120.0f };
    float suggestedTrimDb {};
};

class InputCalibrator
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setProfile(const CalibrationProfile& profile) noexcept { calibration = profile; }
    void process(const float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] CalibrationReading reading() const noexcept { return lastReading; }

private:
    CalibrationProfile calibration;
    CalibrationReading lastReading;
    double smoothedEnergy {};
    double sampleRate { 48000.0 };
};

struct PreEqParameters
{
    float lowCutHz { 80.0f };
    float highCutHz { 18000.0f };
    float tightness { 0.5f };
    float pickEmphasisDb {};
    bool lowShelfEnabled {};
    float lowShelfDb {};
    bool midEmphasisEnabled {};
    float midEmphasisDb {};
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const PreEqParameters&) const = default;
};

struct PreampStageConfig
{
    float driveDb { 12.0f };
    float bias {};
    float asymmetry {};
    float lowCutHz { 80.0f };
    float highCutHz { 12000.0f };
    float outputTrimDb { -6.0f };
    int oversamplingFactor { 4 };
    float dynamicBias { 0.25f };
    float frequencySaturation { 0.2f };
    float attackReduction { 0.15f };
    float memoryAmount { 0.25f };
    /** Take the rational tanh in the saturator instead of std::tanh. Off by default.

        The saturator is the amplifier's second-largest cost after the cabinet, and it runs at
        the oversampled rate across every stage, so this is where an approximation buys the most.
        The error is bounded at 1e-4 (see dsp::fastTanh), which is far below the resolution of
        anything downstream of a distorting valve stage -- but it is not bit-exact, so it is the
        performance tier's decision rather than the default.
    */
    bool approximateSaturation {};
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const PreampStageConfig&) const = default;
};

struct ToneStackParameters
{
    ToneStackType type { ToneStackType::passiveCoupled };
    float bass { 0.5f };
    float mid { 0.5f };
    float treble { 0.5f };
    float midFrequencyHz { 700.0f };
    float midQ { 0.8f };
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const ToneStackParameters&) const = default;
};

struct PhaseInverterParameters
{
    float drive { 1.5f };
    float headroom { 0.8f };
    float asymmetry { 0.12f };
    float differentialImbalance { 0.05f };
    float feedback { 0.2f };
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const PhaseInverterParameters&) const = default;
};

struct PowerAmpParameters
{
    float masterDb { -3.0f };
    float saturation { 0.45f };
    float damping { 0.6f };
    float sag { 0.35f };
    float biasCharacter {};
    float presence { 0.5f };
    float resonance { 0.5f };
    float feedback { 0.35f };
    float sagAttackMs { 35.0f };
    float sagRecoveryMs { 420.0f };
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const PowerAmpParameters&) const = default;
};

struct CabinetMetadata
{
    std::string name { "TubeForge 4x12" };
    std::string microphone { "Dynamic 57" };
    float position {};
};

struct CabinetParameters
{
    float blend { 0.5f };
    /** Splits the two cabinet slots across the stereo field: 0 sums them, 1 sends A left and B right.

        One take through two different rigs, panned apart, is how a wide rhythm guitar is actually
        made in a studio, and it is the only honest way to get that width from a single DI. Two
        takes cannot be synthesised from one performance -- what makes a real double-track wide is
        independent pick timing and vibrato, and a delayed or detuned copy of the same take is
        correlated with itself, so it combs or choruses rather than sounding like a second player.
        Two *different* responses decorrelate spectrally instead, which sums to mono without
        cancelling.

        A separate control rather than a reinterpretation of `blend`, so that every stored preset
        keeps the sound it was saved with: at the default of 0 this stage is bit-identical to what
        it did before the parameter existed.

        Mono compatibility is the thing to watch, not the width itself -- see
        `monoCompatibility()`. `delaySamplesB` is an inter-channel delay once this is up, which is
        wide on speakers and comb-filtered the instant a mix bus sums it.
    */
    float width {};
    bool phaseInvertB {};
    std::size_t delaySamplesB {};
    float lowCutHz { 70.0f };
    float highCutHz { 10500.0f };
    bool bypass {};
    float bassDiBlend {};
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const CabinetParameters&) const = default;
};

struct BassPathParameters
{
    float crossoverHz { 180.0f };
    float cleanBlend { 0.55f };
    float lowCompression { 0.45f };
    float highDriveDb { 6.0f };
    float lowMono { 1.0f };
    bool lowSaturation {};
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const BassPathParameters&) const = default;
};

struct AmpParameters
{
    Instrument instrument { Instrument::guitar };
    PickupProfile pickup { PickupProfile::passive };
    Topology topology { Topology::tightModern };
    float manualInputTrimDb {};
    /** The gate runs before the preamp, so anything it closes on is removed ahead
        of 60-plus dB of gain. Every control is exposed rather than fixed: a gate
        deep enough and fast enough to kill hum also truncates note decay, and
        which side of that trade is right depends on the rig.
    */
    bool gateEnabled { true };
    float gateThresholdDb { -58.0f };
    /// Ducks the noise floor rather than muting; see NoiseGateParameters::rangeDb.
    float gateDepthDb { -15.0f };
    float gateAttackMs { 2.0f };
    float gateHoldMs { 60.0f };
    float gateReleaseMs { 250.0f };
    PreEqParameters preEq;
    std::array<PreampStageConfig, 4> stages {};
    std::size_t stageCount { 3 };
    ToneStackParameters toneStack;
    PhaseInverterParameters phaseInverter;
    PowerAmpParameters powerAmp;
    CabinetParameters cabinet;
    BassPathParameters bass;
    float postLowDb {};
    float postMidDb {};
    float postHighDb {};
    float outputGainDb { -6.0f };
    bool loudnessMatch { true };
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const AmpParameters&) const = default;
};

struct AmpPreset
{
    int schemaVersion { 1 };
    std::string name { "Original Tight Guitar" };
    AmpParameters parameters;
    CalibrationProfile calibration;
};

[[nodiscard]] AmpPreset makeOriginalPreset(Topology topology, Instrument instrument);

/** The built-in synthesized cabinet responses, one per slot (0 = A, 1 = B).

    Exposed so that a user-loaded response can be reverted without tearing down and
    re-preparing the whole amplifier.
*/
[[nodiscard]] std::vector<float> makeDefaultCabinetImpulse(int slot);
[[nodiscard]] CabinetMetadata defaultCabinetMetadata(int slot);
[[nodiscard]] std::string serializePreset(const AmpPreset& preset, bool pretty = true);
[[nodiscard]] std::optional<AmpPreset> deserializePreset(std::string_view json);

class PreEq
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const PreEqParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

private:
    dsp::ProcessSpec spec;
    dsp::Biquad lowCut, highCut, pick, lowShelf, mid;
    bool useLowShelf {};
    bool useMid {};
};

class ResponsivePreampStage
{
public:
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void setConfig(const PreampStageConfig& config, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] float biasState(std::size_t channel) const noexcept;
    [[nodiscard]] const PreampStageConfig& configuration() const noexcept { return config; }
    [[nodiscard]] std::size_t latencySamples() const noexcept;

private:
    [[nodiscard]] dsp::Oversampler& selectedOversampler() noexcept;
    dsp::ProcessSpec spec;
    PreampStageConfig config;
    dsp::Biquad lowCut, highCut, dcBlock;
    std::array<dsp::Oversampler, 4> oversamplers;
    /** One high-rate scratch buffer for all four, sized for the worst factor.

        The four instances exist so that changing oversampling factor under audio does not
        allocate. Only one of them is ever selected, so giving each its own scratch cost fifteen
        times the block size per stage -- four stages and two voices of it -- almost all of which
        was never touched but all of which competed for cache. Safe to share precisely because
        selectedOversampler returns exactly one.
    */
    std::vector<float> oversamplerWork;
    dsp::SmoothedParameter drive;
    dsp::SmoothedParameter trim;
    std::array<float, dsp::maximumChannels> envelope {};
    std::array<float, dsp::maximumChannels> previousEnvelope {};
    std::array<float, dsp::maximumChannels> biasMemory {};
    std::array<float, dsp::maximumChannels> recoveryGain {};
    std::array<float, dsp::maximumChannels> frequencyState {};
    // The saturator subtracts tanh(bias * polarity) to keep the stage centred. Both terms depend
    // only on config, so they take exactly two values for a whole block rather than the one per
    // oversampled sample the lambda used to compute. Cached in setConfig; see process().
    float biasOffsetPositive {};
    float biasOffsetNegative {};
};

class ToneStack
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const ToneStackParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::array<dsp::BiquadCoefficients, 3> coefficients() const noexcept { return generated; }

private:
    dsp::ProcessSpec spec;
    ToneStackParameters parameters;
    std::array<dsp::BiquadCoefficients, 3> generated {};
    dsp::Biquad low, middle, high;
};

class PhaseInverter
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const PhaseInverterParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

private:
    PhaseInverterParameters parameters;
    double sampleRate { 48000.0 };
    dsp::Biquad lowCut, shaping;
    std::array<float, dsp::maximumChannels> feedbackState {};
};

class PowerAmp
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const PowerAmpParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] float supplyState(std::size_t channel) const noexcept;

private:
    dsp::ProcessSpec spec;
    PowerAmpParameters parameters;
    dsp::Biquad presence, resonance;
    dsp::SmoothedParameter master;
    std::array<float, dsp::maximumChannels> supply { 1.0f, 1.0f };
    std::array<float, dsp::maximumChannels> energy {};
    std::array<float, dsp::maximumChannels> feedbackState {};
    float sagAttackCoefficient {};
    float sagRecoveryCoefficient {};
};

class CabinetSection
{
public:
    static constexpr std::size_t maximumIrLength = 4096;
    static constexpr std::size_t maximumAlignmentSamples = 256;
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void setParameters(const CabinetParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    /** Stages a response into the inactive side and asks the audio thread to fade across.

        Safe to call while audio is running and from a worker thread, which is what makes
        user IR loading possible. Pass zero crossfade samples at prepare time, when nothing
        is sounding yet and an immediate swap is what is wanted. Returns false if a swap is
        already in flight -- retry rather than overwrite it.
    */
    bool loadImpulseA(std::span<const float> left, std::span<const float> right = {},
                      CabinetMetadata metadata = {}, std::size_t crossfadeSamples = 2048);
    bool loadImpulseB(std::span<const float> left, std::span<const float> right = {},
                      CabinetMetadata metadata = {}, std::size_t crossfadeSamples = 2048);
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] const CabinetMetadata& metadataA() const noexcept { return firstMetadata; }
    [[nodiscard]] const CabinetMetadata& metadataB() const noexcept { return secondMetadata; }
    /** Samples of audible decay left behind after the input goes silent. Hosts use this,
        via getTailLengthSeconds, to decide how long to keep pulling blocks once transport
        stops -- reporting zero truncates the cabinet decay of every offline render.
    */
    [[nodiscard]] std::size_t tailSamples() const noexcept;
    /** Buffering latency, zero unless a response long enough to want the FFT path is loaded.

        Flows up through AmpVoice into the plug-in's reported latency, so the host compensates
        for it and the dry path stays aligned.
    */
    [[nodiscard]] std::size_t latencySamples() const noexcept;
    /** Puts the built-in responses back in both slots. */
    void restoreDefaultImpulses(std::size_t crossfadeSamples = 2048);

    /** Level lost if the output is folded to mono, in dB. 0 is safe, negative is cancellation.

        Meaningful only in stereo; a mono configuration always reports 0 because there is nothing
        to fold. Measured from the audio rather than predicted from the parameters, so it accounts
        for what the two loaded responses actually do to each other and not merely for the
        settings. -3 dB is the uncorrelated case and unremarkable; steadily worse than that means
        `delaySamplesB` is comb-filtering the sum.
    */
    [[nodiscard]] float monoCompatibility() const noexcept
    { return monoLossDb.load(std::memory_order_relaxed); }

private:
    void measureMonoCompatibility(float* const* channels, std::size_t count,
                                  std::size_t samples) noexcept;
    dsp::ProcessSpec spec;
    CabinetParameters parameters;
    /// Running energies behind monoCompatibility, and the value it publishes.
    double monoSumEnergy {}, channelEnergy {};
    std::atomic<float> monoLossDb {};
    CabinetMetadata firstMetadata;
    CabinetMetadata secondMetadata;
    /** Two convolution implementations, and the rule for choosing between them.

        Direct convolution is O(taps) per sample and free of latency; the partitioned FFT path is
        O(log taps) but costs a partition of buffering. At the built-in 384-tap responses the
        direct path costs about four microseconds a block and is plainly right. At the 4096 taps a
        user impulse can reach it measured 61 microseconds stereo against the buffered path's 14.

        The choice is made for the **section**, never per slot. The two responses are blended, so
        if one carried a partition of latency and the other did not, blending them would comb
        rather than mix. That is why both responses are kept here: switching implementation means
        reloading both sides into the other pair, and the section cannot ask its caller to do that
        for it.
    */
    static constexpr std::size_t partitionedThresholdTaps = 1024;
    static constexpr std::size_t partitionSamples = 128;
    dsp::CrossfadingDirectConvolver first, second;
    dsp::BufferedCrossfadingConvolver firstBuffered, secondBuffered;
    /// Prepared on first use rather than up front: a user who never loads a long impulse should
    /// not carry the FFT path's spectra, which are several hundred kilobytes per slot.
    bool bufferedPrepared {};
    /// Read by the audio thread each block; written by whichever thread loads a response.
    std::atomic<bool> usingBuffered {};
    /// Copies of what is loaded, so an implementation switch can reload both sides itself.
    std::vector<float> firstLeft, firstRight, secondLeft, secondRight;
    void selectImplementation(std::size_t crossfadeSamples);
    // Tracked here rather than read back from the convolvers: a staged response is not yet
    // the active one, and the tail has to cover it from the moment it is queued.
    std::size_t firstLength {}, secondLength {};
    dsp::Biquad lowCut, highCut;
    std::vector<float> dryBuffer, firstBuffer, secondBuffer, delay;
    std::array<std::size_t, dsp::maximumChannels> delayPosition {};

    /** Whether each side contributes enough to be worth convolving.

        The blend control weights A by (1 - blend) and B by blend, so at either rail one of the
        two convolutions is computed and then multiplied by zero -- and blend 0, cabinet A alone,
        is the default. Skipping the idle side is the single largest saving available in the
        traditional engine, because cabinet convolution is its most expensive operation.

        A skipped convolver's delay line goes stale, so re-engaging one clears its history first
        and lets its output grow from silence as real input refills it. That is continuous by
        construction rather than merely fading a discontinuity, which is why no ramp is needed.
        The dead zone carries hysteresis so a control resting on the threshold cannot oscillate.
    */
    static constexpr float engageThreshold = 0.002f;
    static constexpr float disengageThreshold = 0.001f;
    static void updateEngagement(bool& engaged, bool& needsHistoryReset, float weight) noexcept;
    bool firstEngaged { true }, secondEngaged { true };
    bool firstNeedsHistoryReset {}, secondNeedsHistoryReset {};
};

class AmpVoice
{
public:
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void configure(const AmpPreset& preset) noexcept;
    void setParameters(const AmpParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] CalibrationReading calibrationReading() const noexcept { return calibrator.reading(); }
    [[nodiscard]] std::size_t latencySamples() const noexcept;
    [[nodiscard]] std::size_t tailSamples() const noexcept { return cabinet.tailSamples(); }
    bool loadCabinetImpulse(int slot, std::span<const float> left, std::span<const float> right,
                            CabinetMetadata metadata, std::size_t crossfadeSamples);
    void restoreDefaultCabinet(std::size_t crossfadeSamples) { cabinet.restoreDefaultImpulses(crossfadeSamples); }
    [[nodiscard]] const CabinetMetadata& cabinetMetadata(int slot) const noexcept
    { return slot == 0 ? cabinet.metadataA() : cabinet.metadataB(); }
    /// See CabinetSection::monoCompatibility. Level lost folding to mono, in dB; 0 is safe.
    [[nodiscard]] float cabinetMonoCompatibility() const noexcept
    { return cabinet.monoCompatibility(); }

private:
    void processDrivenPath(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    dsp::ProcessSpec spec;
    AmpParameters parameters;
    InputCalibrator calibrator;
    dsp::NoiseGate gate;
    PreEq preEq;
    std::array<ResponsivePreampStage, 4> stages;
    ToneStack toneStack;
    PhaseInverter phaseInverter;
    PowerAmp powerAmp;
    CabinetSection cabinet;
    dsp::LinkwitzRileyCrossover bassCrossover;
    dsp::Compressor bassCompressor;
    dsp::Biquad postLow, postMid, postHigh;
    dsp::SmoothedParameter inputTrim, outputGain, cleanBlend;
    std::vector<float> lowBuffer, highBuffer, dryBuffer;
    std::array<double, dsp::maximumChannels> inputEnergy {};
    std::array<double, dsp::maximumChannels> outputEnergy {};
    std::array<float, dsp::maximumChannels> matchGain { 1.0f, 1.0f };
};

class TraditionalAmpProcessor
{
public:
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void setParameters(const AmpParameters& parameters) noexcept;
    void setParametersImmediately(const AmpParameters& parameters) noexcept;
    void loadPreset(const AmpPreset& preset, std::size_t crossfadeSamples = 2048);
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] CalibrationReading calibrationReading() const noexcept;
    [[nodiscard]] std::size_t latencySamples() const noexcept;
    /** The longer of the two voices rather than the active one: both run during a preset
        crossfade, and a tail that shrank on a preset change would cut the outgoing decay.
    */
    [[nodiscard]] std::size_t tailSamples() const noexcept;
    /** Loads a cabinet response into slot 0 (A) or 1 (B).

        Applied to both voices, not just the sounding one: they alternate across preset
        changes, so loading into one only would put the previous cabinet back the next time a
        preset was recalled. Safe to call from a worker thread while audio is running.
    */
    bool loadCabinetImpulse(int slot, std::span<const float> left, std::span<const float> right = {},
                            CabinetMetadata metadata = {}, std::size_t crossfadeSamples = 2048);
    void restoreDefaultCabinet(std::size_t crossfadeSamples = 2048);
    [[nodiscard]] const CabinetMetadata& cabinetMetadata(int slot) const noexcept
    { return voices[crossfader.mode()].cabinetMetadata(slot); }
    /// See CabinetSection::monoCompatibility. Level lost folding to mono, in dB; 0 is safe.
    [[nodiscard]] float cabinetMonoCompatibility() const noexcept
    { return voices[crossfader.mode()].cabinetMonoCompatibility(); }
    [[nodiscard]] const AmpPreset& currentPreset() const noexcept { return presets[crossfader.mode()]; }

private:
    dsp::ProcessSpec spec;
    std::array<AmpVoice, 2> voices;
    std::array<AmpPreset, 2> presets;
    dsp::ModeCrossfader crossfader;
    AmpParameters requestedParameters;
    std::size_t transitionTarget {};
    bool transitionPending {};
    /** Forces the next setParameters through even if the values compare equal.

        Set by every path that reconfigures a voice behind setParameters' back -- preset loads,
        immediate application, and the end of a mode transition, which leaves the voice that
        just stopped sounding holding whatever it was last given. Without it, a parameter set
        that happens to match requestedParameters would skip the resync those paths owe.
    */
    bool parametersDirty { true };
};

[[nodiscard]] std::vector<float> renderOffline(TraditionalAmpProcessor& processor,
                                                std::span<const float> monoInput,
                                                std::size_t blockSize);
} // namespace nts::amp
