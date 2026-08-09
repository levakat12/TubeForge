#pragma once

#include <nts/dsp/Common.h>
#include <nts/dsp/Convolution.h>
#include <nts/dsp/DelayLine.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Filters.h>
#include <nts/dsp/ModeCrossfader.h>
#include <nts/dsp/Nonlinear.h>
#include <nts/dsp/Oversampling.h>
#include <nts/dsp/Smoothing.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
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

    Indices 0-6 are guitar circuits that bass reaches by retuning; 7 onwards are **bass-native**
    and were designed for the instrument rather than adapted to it. Nothing in the DSP reads that
    distinction -- it is what `topologyAffinity` publishes so a picker can filter on it, and it is
    why the bass-native block sits at the end rather than interleaved by character.
*/
enum class Topology
{
    tightModern,
    vintageBloom,
    americanClean,
    britishCrunch,
    classAChime,
    saggingRectifier,
    studioDirect,
    valveFlagship,
    cathodeVintage,
    hybridMosfet,
    shortPathGrind,
    solidStateBiAmp,
    cmosModern
};

inline constexpr std::size_t topologyCount = 13;

/** Which instrument a voicing was designed for. A browsing hint; no audio code reads it.

    `either` is not a judgement that a voicing sounds equally good on both -- it says the values
    were chosen for one instrument and retuned for the other, which is true of every voicing that
    predates the bass-native block. `bass` says the opposite: these were reasoned from bass
    circuits, and `makeOriginalPreset` still answers for guitar so that a host writing an
    out-of-affinity pair gets a real preset rather than an uninitialised one.
*/
enum class TopologyAffinity { either, bass };

[[nodiscard]] TopologyAffinity topologyAffinity(Topology topology) noexcept;

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
    /** Where the shelf and the mid bell sit. Defaults are the fixed values they used to have.

        They were written into `PreEq::setParameters` as literals, which was fine while the only
        thing that set them was a voicing table wanting a general tilt. It stops being fine the
        moment a switch has a *specific* corner: a −10 dB cut at 500 Hz is the whole character of
        an Ultra Lo, and the same cut at 950 Hz is a different amplifier being unhelpful.
    */
    float lowShelfHz { 140.0f };
    float midEmphasisHz { 950.0f };
    /** A treble shelf ahead of the distortion. Disabled by default and free when it is.

        The pre-EQ had no high shelf at all -- `pickEmphasisDb` is a bell fixed at 2.8 kHz, which
        is a pick-attack control and not somewhere to hide a bright switch. Both of the treble
        switches this exists for sit at 5 kHz and above, so they need their own filter rather than
        a reinterpretation of one that already means something.
    */
    bool highShelfEnabled {};
    float highShelfDb {};
    float highShelfHz { 5000.0f };
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const PreEqParameters&) const = default;
};

/** A switch on the front of a particular amplifier, and nothing more general than that.

    These are not presets. `Ultra Lo` is a network on one amplifier that lifts 2 dB at 40 Hz while
    cutting 10 dB at 500, and the reason it reads as "more bass" is the cut rather than the lift.
    `Deep` and `Bright` are broad 5 dB shelves on a different amplifier. Modelling them as saved
    parameter *states* rather than as recallable presets is what they are on the hardware, and it
    is also why this needed no preset format: a switch is a control, and the project already saves
    controls.

    `none` is every amplifier's default and applies no change at all, so a voicing that offers no
    switches is unaffected and every preset written before this existed reads back identically.
*/
enum class PanelSwitch { none, ultraLo, ultraHi, deep, bright };

inline constexpr std::size_t panelSwitchCount = 5;

[[nodiscard]] std::string_view panelSwitchKey(PanelSwitch value) noexcept;
[[nodiscard]] std::string_view panelSwitchName(PanelSwitch value) noexcept;

/// Whether a voicing has this switch on its panel. `none` is true for every voicing.
[[nodiscard]] bool panelSwitchAppliesTo(Topology topology, PanelSwitch value) noexcept;


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
    /** The curve this stage clips with. `hyperbolicTangent` is a valve and is the default.

        The single thing that separated a valve voicing from a transistor one and could not be
        reached from any parameter: every stage in this amplifier used to be a hyperbolic tangent
        with no way to say otherwise, so a solid-state voicing came out sounding like a soft-knee
        valve however its gain structure was arranged. A transistor stage against a supply rail
        does not have a knee -- it runs linear and then stops -- and `hardClip` is that.

        Defaulted so that every voicing written before this field existed is bit-identical, and
        the two `hyperbolicTangent` loops in `process` are kept as special cases rather than
        folded into the general dispatch for the same reason.
    */
    dsp::Waveshape shape { dsp::Waveshape::hyperbolicTangent };
    /** Take the antiderivative-antialiased form of `shape`. Off by default.

        Oversampling alone does not tame a hard clipper: the amplifier's own filter note records
        that raising the anti-alias taps past eight buys nothing because the residual is not what
        the stopband bounds. The fold-back comes from the curve's corner, and averaging the curve
        across each sample interval is what attacks it directly.

        Not a substitute for oversampling -- the two are meant to be used together, and first-order
        ADAA on its own still leaves audible artefacts on a hard clip. Costs half a sample of delay
        and a little top end, which is why it is opt-in: the seven original voicings predate it and
        must keep sounding exactly as they do. Ignored for shapes `dsp::supportsAntiderivative`
        rejects, rather than silently producing a wrong curve.
    */
    bool antialiasedSaturation {};
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
    /** The curve the output stage clips with. See `PreampStageConfig::shape`.

        Separate from the preamp's because the two halves of a solid-state amplifier do not clip
        alike: a JFET front end can be run well inside its range while the power rails are being
        hit flat, and it is the *rails* that give that class of amplifier its character. Modelling
        both with one shape would lose the distinction that matters.
    */
    dsp::Waveshape shape { dsp::Waveshape::hyperbolicTangent };
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const PowerAmpParameters&) const = default;
};

struct CabinetMetadata
{
    std::string name { "TubeForge 4x12" };
    std::string microphone { "Dynamic 57" };
    float position {};
};

/** One cabinet slot's own settings, as distinct from what the section does with the pair.

    Everything here defaults to *what the slot did before it had these controls*, which is what
    makes them safe to add: unity level, unmuted, in phase, no delay of its own. `pan` is the one
    exception and it is not an exception -- see `CabinetParameters::slots`, where the two defaults
    are the hard split `width` has always performed.
*/
struct CabinetSlotParameters
{
    float levelDb {};
    /** Where this slot sits in the stereo field: -1 hard left, +1 hard right.

        **Only audible once `CabinetParameters::width` is up.** Width interpolates between the
        blended sum and the placed signal, so at the default width of 0 this changes nothing at
        all -- which is precisely why it can be added to a shipped plug-in. What it generalises is
        the split itself: "A left, B right" was hard-coded, and is now what the two defaults
        happen to say.
    */
    float pan {};
    bool phaseInvert {};
    /** This slot's own delay, in samples, up to `CabinetSection::maximumAlignmentSamples`.

        Two microphones at different distances from a speaker are at different *times*, and that
        difference is most of what makes a blend of them sound like anything. Per slot rather than
        only on B so that a delay can be put on the near mic, which is the direction a real
        studio alignment usually goes.
    */
    std::size_t delaySamples {};
    /** Silences the slot, and -- because the section skips a slot contributing nothing -- stops
        paying for its convolution. That second half is the point: muting the idle side of a
        blend is the cheapest thing the cabinet can do.
    */
    bool mute {};
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const CabinetSlotParameters&) const = default;
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
        `monoCompatibility()`. A slot's `delaySamples` is an inter-channel delay once this is up,
        which is wide on speakers and comb-filtered the instant a mix bus sums it.
    */
    float width {};
    float lowCutHz { 70.0f };
    float highCutHz { 10500.0f };
    bool bypass {};
    float bassDiBlend {};
    /// Level after the blend, the width split and the two cuts. The section's own output stage,
    /// so a mic blend can be brought back to where it started once its parts have been changed.
    float outputTrimDb {};
    /** A is 0, B is 1.

        The two `pan` defaults are `-1` and `+1` deliberately: those are the placement `width`
        performed as a hard-coded rule before slots had settings of their own, so a preset saved
        without these fields recombines exactly as it always did. Every other field defaults to
        the neutral value for the same reason. See `CabinetSlotParameters`.
    */
    std::array<CabinetSlotParameters, 2> slots {
        CabinetSlotParameters { 0.0f, -1.0f, false, 0, false },
        CabinetSlotParameters { 0.0f,  1.0f, false, 0, false } };
    /// Value equality, so callers can skip reconfiguring when nothing actually moved.
    bool operator==(const CabinetParameters&) const = default;
};

/** The two-band bass path: a clean bottom under a driven top, either side of a crossover.

    **On the crossover frequency, because the number is the whole design.** At the 120-220 Hz the
    valve voicings use, this is a mud guard: it keeps the fundamental of a low B out of a
    distorting stage that would turn it to porridge. At 400 Hz and above it becomes something
    else entirely -- a genuine bi-amp, where the *whole note body* passes clean and only the
    attack transient and the harmonics are driven. That is the difference between a bass amp
    that is polite about its low end and one built around two power sections, and it is reachable
    from this one control.

    **Per-band gain is set by these parameters and must never track the signal.** The crossover is
    fourth-order Linkwitz-Riley -- two cascaded biquads a side -- which sums flat only while both
    bands are linear and static. Published guidance on dynamic multiband processing is that a
    fourth-order LR recombination can be phase-modulated by up to +/-180 degrees when band levels
    move during playback, and distorting one band already breaks the linearity half of that
    assumption. A real bi-amp head sets its crossover and its two band levels on the front panel
    and leaves them there for a given sound, so holding these static is faithfulness rather than a
    workaround. Anything that makes band gain follow an envelope is a different feature and needs
    its own test.
*/
struct BassPathParameters
{
    float crossoverHz { 180.0f };
    float cleanBlend { 0.55f };
    float lowCompression { 0.45f };
    float highDriveDb { 6.0f };
    float lowMono { 1.0f };
    bool lowSaturation {};
    /** Gain into the low band's saturator, when `lowSaturation` is on.

        2.6 dB is 1.35x, which is the constant this replaced -- it was written into the processing
        loop with no way to reach it, so "the low band saturates" was a yes-or-no question and the
        amount was whatever one number somebody once picked. A bi-amp wants to answer it per
        voicing.
    */
    float lowDriveDb { 2.6f };
    /// Output level of each band before they are summed. Unity is what the path did before they
    /// existed, so a preset saved without them recombines exactly as it always has.
    float lowLevelDb {};
    float highLevelDb {};
    /** The low band's curve. See `PreampStageConfig::shape`.

        A valve bass amp's bottom end rounds; a transistor bi-amp's 300-watt low side is meant to
        stay clean and then stop. Same argument as the preamp's, one band down.
    */
    dsp::Waveshape lowShape { dsp::Waveshape::hyperbolicTangent };
    /** Hold the clean low band back to match the driven high band's group delay. Off by default.

        **This corrects a real defect, and it is off by default anyway.** Only the high band passes
        through the preamp stages, so the two halves of the crossover rejoin 16 to 24 samples
        apart -- and a Linkwitz-Riley split sums flat only while its bands keep the phase
        relationship it was designed around. At the 120-180 Hz the valve voicings split at, the
        skew is about 20 degrees and reads as voicing; at the 500 Hz a bi-amp wants, it is 60 and
        sits audibly in the crossover region.

        Defaulting it on would have been the tempting call and is the wrong one: it changes the
        sound of **every bass preset anyone has already saved**, and the regression guard caught
        exactly that -- all seven original bass voicings moved and none of the guitar ones did.
        A fix that silently re-voices somebody's finished track is not a fix from where they are
        sitting. So the bass-native voicings are born with it on, everything older keeps the sum it
        shipped with, and a player who wants the correction on a valve voicing can have it.
    */
    bool alignBands {};
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
    /** Untouched input summed back over the whole amplifier. 0 is fully wet and is the default.

        The third blend in this file and the only one that is a *parallel clean path*, which is
        worth spelling out because the other two look like it and are not:

        - `BassPathParameters::cleanBlend` blends the **low band** clean, after the crossover, so
          it never carries the fundamental's harmonics.
        - `CabinetParameters::bassDiBlend` blends the cabinet's **input** against its output, so
          what it adds back is still fully distorted -- it bypasses the speaker, not the drive.

        This one is tapped ahead of everything and summed after it, which is what makes a bass
        distortion usable: the fundamental survives at full weight while the upper band is
        destroyed. On the voicing built around it, it is not a refinement but half the circuit.

        **The dry tap is delayed to match the drive path** -- see `AmpVoice::dryDelay`. Summing it
        undelayed is a comb filter in the presence region, and it is the kind of wrong that sounds
        merely disappointing rather than broken.
    */
    float dryBlend {};
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

/** Engages a switch on an already-built parameter set. `none` leaves it untouched.

    Applied after the voicing rather than folded into `makeOriginalPreset`, because the switch is a
    control the player moves and the voicing is where they started. Silently does nothing for a
    switch the voicing does not have, so an automation lane or a preset naming one cannot produce
    an amplifier that never existed.
*/
void applyPanelSwitch(AmpParameters& parameters, PanelSwitch value) noexcept;

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
    dsp::Biquad lowCut, highCut, pick, lowShelf, mid, highShelf;
    bool useLowShelf {};
    bool useMid {};
    bool useHighShelf {};
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
    /** Whether `shape`/`antialiasedSaturation` actually select anything but the valve path.

        Resolved once in `setConfig` rather than tested per sample: `process` picks one of four
        loop bodies for a whole block, and the two hyperbolic-tangent ones have to stay exactly
        the code they were.
    */
    bool usesCustomShape {};
    bool usesAntialiasing {};
    /// One previous sample per channel, for the ADAA path. Unused unless `usesAntialiasing`.
    dsp::AntialiasedWaveshaper antialiased;
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
    /** Antialiasing for the output stage, engaged by any shape that is not the valve one.

        Not opt-in the way the preamp's is, and the asymmetry is deliberate. A preamp stage runs
        inside an oversampler and can lean on it; **this stage has no oversampler at all** and
        cannot get one cheaply -- the supply sag and the global feedback make it an IIR loop
        around the nonlinearity, not a memoryless curve an oversampler can wrap. So a hard clipper
        here folds at the base rate with nothing to catch it, and the antiderivative form is the
        only defence available.

        It is a mitigation rather than a solution: first-order ADAA on a hard clip is a large
        improvement and still short of what oversampling would add. Held to be good enough because
        the voicing that uses it runs the output stage at moderate saturation and gets its
        character from the rails being *reached*, not from living against them.

        Costs nothing when the shape is `hyperbolicTangent`, which is every voicing that predates
        the field: that path does not touch this.
    */
    dsp::AntialiasedWaveshaper antialiased;
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
    /** The cross terms of a true-stereo response, or empty for an ordinary one.

        A true-stereo capture is **four** responses, not two: what the left input does to the left
        output, what it does to the right, and the same pair for the right input. An ordinary
        stereo response is only the two direct terms, and every impulse response most people own is
        one of those -- which is why this is a separate, defaulted argument rather than a wider
        signature everybody has to fill in.

        `leftToRight` and `rightToLeft` are the two cross terms. Supplying either engages the
        second convolver pair for that slot, which roughly doubles what the slot costs; supplying
        neither leaves the slot exactly as it was before this existed, down to the instruction.

        Appended after `crossfadeSamples` rather than beside the direct pair, which reads worse and
        keeps every existing call site compiling unchanged. That is the right trade for an argument
        almost nobody passes.
    */
    struct TrueStereoImpulse
    {
        std::span<const float> leftToRight;
        std::span<const float> rightToLeft;
        [[nodiscard]] bool engaged() const noexcept
        { return ! leftToRight.empty() || ! rightToLeft.empty(); }
    };
    bool loadImpulseA(std::span<const float> left, std::span<const float> right = {},
                      CabinetMetadata metadata = {}, std::size_t crossfadeSamples = 2048,
                      TrueStereoImpulse cross = {});
    bool loadImpulseB(std::span<const float> left, std::span<const float> right = {},
                      CabinetMetadata metadata = {}, std::size_t crossfadeSamples = 2048,
                      TrueStereoImpulse cross = {});
    /// True when that slot is convolving a four-channel true-stereo response.
    [[nodiscard]] bool isTrueStereo(int slot) const noexcept
    { return slot == 0 ? firstTrueStereo : secondTrueStereo; }
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
        a slot delay is comb-filtering the sum.
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
    /** The cross terms, when a slot is convolving a true-stereo response.

        A second convolver per slot rather than a four-channel one, because the machinery for two
        channels already exists and is the part that had to be right: the cross pair is fed the
        input channels *swapped* and loaded with the cross impulses in the matching order, so
        `direct + cross` is the true-stereo matrix product without a single new inner loop.

        Prepared on first use, like the buffered pair and for the same reason: a user who never
        loads a true-stereo response should not carry the memory of one.
    */
    dsp::CrossfadingDirectConvolver firstCross, secondCross;
    dsp::BufferedCrossfadingConvolver firstCrossBuffered, secondCrossBuffered;
    bool crossPrepared {};
    bool firstTrueStereo {}, secondTrueStereo {};
    /// The swapped-input scratch the cross pair reads, and what it produces.
    std::vector<float> crossBuffer;
    /// Prepared on first use rather than up front: a user who never loads a long impulse should
    /// not carry the FFT path's spectra, which are several hundred kilobytes per slot.
    bool bufferedPrepared {};
    /// Read by the audio thread each block; written by whichever thread loads a response.
    std::atomic<bool> usingBuffered {};
    /// Copies of what is loaded, so an implementation switch can reload both sides itself.
    std::vector<float> firstLeft, firstRight, secondLeft, secondRight;
    /// The cross terms' copies, kept for the same reason: an implementation switch reloads both.
    std::vector<float> firstCrossLeft, firstCrossRight, secondCrossLeft, secondCrossRight;
    void selectImplementation(std::size_t crossfadeSamples);
    /// Allocates the cross convolvers the first time a true-stereo response is loaded.
    void prepareCrossPair();
    // Tracked here rather than read back from the convolvers: a staged response is not yet
    // the active one, and the tail has to cover it from the moment it is queued.
    std::size_t firstLength {}, secondLength {};
    dsp::Biquad lowCut, highCut;
    std::vector<float> dryBuffer, firstBuffer, secondBuffer;
    /** One delay ring per slot, each holding `maximumAlignmentSamples + 1` samples per channel.

        Two rather than one because both slots can now be delayed: aligning two microphones
        means moving whichever of them is early, and which one that is depends on the pair.
        A slot at zero reads back the sample it has just written, so an unused ring is exactly
        transparent rather than merely cheap.
    */
    std::array<std::vector<float>, 2> slotDelay;
    std::array<std::array<std::size_t, dsp::maximumChannels>, 2> delayPosition {};
    /// Linear gains derived from the slot levels and mutes, recomputed in `setParameters` so the
    /// per-sample loop never calls `dbToLinear`.
    std::array<float, 2> slotGain { 1.0f, 1.0f };
    /// Per-slot, per-channel placement weights behind the width split; see `setParameters`.
    std::array<std::array<float, 2>, 2> panGain { { { 1.0f, 0.0f }, { 0.0f, 1.0f } } };
    float outputTrimGain { 1.0f };

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
    dsp::SmoothedParameter inputTrim, outputGain, cleanBlend, dryMix;
    std::vector<float> lowBuffer, highBuffer, dryBuffer, dryTap;

    /** Delays that hold the parallel paths in step with the drive path's group delay.

        Every oversampled preamp stage contributes `dsp::antiAliasTapsPerPhase` base-rate samples,
        and the cabinet contributes a partition when a long response is loaded. Anything that
        rejoins the signal *after* those stages, having not passed through them, is that many
        samples early -- and two copies of one signal a few samples apart is a comb filter, not a
        blend. At two or three oversampled stages the first null sits between roughly 1 and
        1.5 kHz, which is the presence region.

        `dryDelay` aligns the parallel clean path behind `AmpParameters::dryBlend`.

        `lowBandDelay` fixes the **same defect in the bass crossover**, which predates the dry
        blend and was found while building it. Only the high band goes through the preamp stages;
        the low band reaches the sum directly, so the two halves of a Linkwitz-Riley split -- which
        sums flat only when its bands stay in their designed phase relationship -- were rejoining
        16 to 24 samples apart. Harmless-looking at the 120-180 Hz the valve voicings split at,
        where it is about 20 degrees; at the 500 Hz a bi-amp wants it is 60 and audible.

        Both are set from `latencySamples()`, so they cannot drift from what the chain actually
        costs, and both are re-derived whenever `stageCount` or a stage's oversampling changes.
    */
    dsp::DelayLine dryDelay, lowBandDelay;
    /// What the delays are currently set to, so `setParameters` only reconfigures on a change.
    std::size_t alignedLatency { std::numeric_limits<std::size_t>::max() };
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
