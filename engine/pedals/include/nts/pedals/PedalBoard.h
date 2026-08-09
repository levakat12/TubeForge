#pragma once

#include <nts/dsp/Common.h>
#include <nts/dsp/DelayLine.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Effects.h>
#include <nts/dsp/Filters.h>
#include <nts/dsp/Nonlinear.h>
#include <nts/dsp/Smoothing.h>
#include <nts/ml/NeuralAmpProcessor.h>

#include <array>
#include <cstddef>
#include <vector>

namespace nts::pedals
{
/** The DSP architecture a model runs on.

    A slot has one of these; a *model* is a row of data that selects one and supplies its
    coefficients. That split is the whole point: fifty modelled pedals are fifty rows over a
    handful of engines, in the same way seven amplifier voicings are seven rows over one
    amplifier graph. Adding a pedal that clips should not add a line of DSP.

    `silent` is the empty slot, and it is why this stage can be absent altogether: a board whose
    slots are all silent leaves the buffer untouched and returns before it touches a filter.
*/
enum class PedalEngine
{
    silent,
    /// Filter, waveshape, filter. Every overdrive, distortion, fuzz and boost.
    shaper,
    compressor,
    /// A Neural Amp Modeler capture. The model file is the voicing.
    neural,
    /// A swept short delay or all-pass chain. Chorus, flanger, vibrato and phaser.
    modulation,
    /// Sample-rate and bit-depth reduction into a resonant filter.
    crush,
    /// A feedback delay, optionally with the bandwidth loss a bucket-brigade line has.
    delay,
    /// A reverberant tail, optionally dispersed the way a spring tank does.
    reverb,
    /// A crossover with parallel clean and driven paths. What a bass DI box is.
    bassPreamp,
    /// Two crossfaded delay taps read at a ratio. Octaves and whammy sweeps.
    pitch
};

/// What a modulation model is built from. The three read as different effects, but two of them
/// are the same delay line at different depths and the third is not a delay at all.
enum class ModulationTopology
{
    /// A long-ish delay swept gently and mixed with the dry path.
    chorus,
    /// A very short delay swept wide, with regeneration. The comb is the sound.
    flanger,
    /// A cascade of all-pass sections swept together. Notches rather than a comb.
    phaser
};

/** A swept modulation. Delay lengths are milliseconds; the controls move rate and depth.

    Chorus and flanger differ by two numbers rather than by algorithm -- a chorus is a 10-to-25
    millisecond delay swept a little with no feedback, a flanger is a sub-millisecond one swept
    across most of its range with a lot. Making them one engine is not a shortcut; it is what
    they are.
*/
struct ModulationVoicing
{
    ModulationTopology topology { ModulationTopology::chorus };
    /// Delay at the centre of the sweep, and how far either side of it the LFO travels.
    float centreMs { 12.0f };
    float depthMs { 3.0f };
    /// What the Rate control sweeps between.
    float minRateHz { 0.1f };
    float maxRateHz { 6.0f };
    /** Regeneration at the top of the Feedback control, -1 to 1.

        Negative inverts the fed-back signal, which moves the comb's first notch to DC and is
        the hollow, through-a-pipe flange rather than the jet-plane one.
    */
    float maxFeedback {};
    /// Phaser only: how many all-pass sections sweep together. Four is the classic; six is deeper.
    int stages { 4 };
    /// Offsets the right channel's LFO by this fraction of a cycle, which is what makes a
    /// mono source come out wide.
    float stereoSpread { 0.25f };
};

/** Sample-rate and bit-depth reduction, then a resonant low-pass to tame what it made.

    The aliasing here is deliberate and is the effect, which makes this the one engine where
    anti-aliasing would be a bug. The filter afterwards is not there to clean that up -- it is
    there because the reduction alone is fatiguing, and a resonant sweep over it is what makes
    it playable.
*/
struct CrushVoicing
{
    /// Bit depth at each end of the Bits control. Fractional depths are meaningful here.
    float minBits { 2.0f };
    float maxBits { 16.0f };
    /// How many input samples each output sample is held for, at each end of the Crush control.
    float minDivisor { 1.0f };
    float maxDivisor { 48.0f };
    /// The resonant low-pass after the reduction.
    double filterMinHz { 300.0 };
    double filterMaxHz { 14000.0 };
    float filterQ { 2.2f };
};

/** A feedback delay. What separates the models is how badly the repeats decay.

    A bucket-brigade line loses bandwidth on every pass, so the fourth repeat of an analogue
    delay is a dull ghost while the fourth repeat of a digital one is the same as the first.
    `repeatDampingHz` is that loss, and it is most of why the two sound like different pedals
    despite being the same delay.
*/
struct DelayVoicing
{
    /// What the Time control sweeps between.
    float minTimeMs { 40.0f };
    float maxTimeMs { 800.0f };
    /// Regeneration at the top of the Feedback control. Held below 1 so the line always decays.
    float maxFeedback { 0.85f };
    /// Bandwidth of the repeat path. Low is a bucket-brigade line; 20 kHz is a clean digital one.
    float repeatDampingHz { 20000.0f };
    /// Fraction of the delay applied to the right channel, for width.
    float stereoSpread {};
};

/** A reverberant tail.

    `springStages` is the one thing here that is not a room: a spring tank disperses high
    frequencies more slowly than low ones, so a transient arrives as a rising chirp rather than
    as a copy of itself. A chain of all-pass sections ahead of the tank is what produces that,
    and it is the entire difference between a spring and a small room.
*/
struct ReverbVoicing
{
    /// What the Decay control sweeps between.
    float minSize { 0.15f };
    float maxSize { 0.95f };
    /// Absorption of the top end, 0 to 1.
    float damping { 0.45f };
    float lowCutHz { 180.0f };
    /// All-pass sections ahead of the tank. Zero is a room; four or more is a spring.
    int springStages {};
    /// Frequency the dispersion is centred on. Only meaningful with springs.
    double springCentreHz { 1500.0 };
};

/** A bass DI: split the band, drive the top, leave the bottom alone, recombine.

    The reason a bass distortion is not just a guitar distortion on a bass. Overdriving the
    fundamental of a low B turns it to mush and loses the note; overdriving only what is above
    the crossover leaves the note intact and puts the grit on top of it. The clean path is the
    point, not a refinement of it.
*/
struct BassPreampVoicing
{
    /// Where the clean and driven paths part company.
    double crossoverHz { 180.0 };
    /// Drive range applied to the upper band only, in dB.
    float minDriveDb { 6.0f };
    float maxDriveDb { 40.0f };
    dsp::Waveshape shape { dsp::Waveshape::softClip };
    /// Voicing of the driven band after the clipper.
    double toneMinHz { 1200.0 };
    double toneMaxHz { 9000.0 };
    /// Static make-up so the models sit at comparable levels.
    float trimDb {};
    /// A fixed low shelf on the clean path, which is what a DI box's "blend" knob really moves.
    float lowShelfDb {};
};

/** Pitch shifting by reading a delay line faster or slower than it is written.

    **Not the phase vocoder the plan specified**, and the difference matters. A vocoder needs an
    FFT, which means a frame of buffering, which means the plug-in has to report latency to the
    host and the dry path has to be delayed to match -- a change that reaches well outside this
    engine for the sake of three models out of fifty-nine.

    Reading a delay line at a ratio does the same job in the time domain. A tap that walks
    towards the write head plays back faster and so sounds higher; one that walks away plays
    slower and sounds lower. The tap has to wrap when it runs out of window, and the wrap is a
    discontinuity -- so there are two taps half a window apart, each faded by a Hann window.
    Hann windows at fifty per cent overlap sum to exactly one, so the crossfade is transparent
    and the wrap is inaudible.

    What it costs: on a chord the two taps are reading different parts of the same waveform, so
    dense material warbles in a way a vocoder does not. That is the honest trade, and it is the
    same one every hardware pitch pedal of this type makes. What it buys is **zero added
    latency** -- the effect is causal and needs no compensation anywhere.
*/
struct PitchVoicing
{
    /// Semitone range the Pitch control sweeps. Negative is down.
    float minSemitones { -12.0f };
    float maxSemitones { 12.0f };
    /** Window length. Longer is smoother on chords and blurrier on transients; shorter tracks a
        single note tightly and warbles on anything else. 50 ms is the usual compromise.
    */
    float windowMs { 50.0f };
    /// A second voice at a fixed interval, in semitones. Zero disables it and costs nothing.
    float secondVoiceSemitones {};
    /// Feeds the shifted output back in, which cascades the interval into an arpeggio.
    float maxFeedback {};
};

/** The built-in archetypes, which are the first entries of the model table.

    These indices are host-visible choice values *and* saved project state, so they must keep
    meaning what they have always meant. Named units append after them, from `firstModelledUnit`
    onwards -- which also gives the picker a good ordering, generic archetypes before specific
    boxes.
*/
enum class PedalKind
{
    none,
    boost,
    overdrive,
    distortion,
    fuzz,
    compressor,
    neuralCapture
};

/// The archetypes above. Not the size of the model table -- see `modelCount`.
inline constexpr std::size_t kindCount = 7;
/// Where the named units start. Everything below this index is a built-in archetype.
inline constexpr int firstModelledUnit = static_cast<int>(kindCount);

/// Slots on the board, in signal order. Four is what fits on the page and is about as many
/// neural captures as a machine will carry alongside an amplifier.
inline constexpr std::size_t slotCount = 4;

/** Continuous controls per slot.

    Six rather than four because four cannot carry a pedal with two voicing knobs alongside
    drive and level. A model names the ones it has and leaves the rest empty; the interface
    hides what a model does not use.
*/
inline constexpr std::size_t controlCount = 6;

/** What makes one clipping pedal sound unlike another.

    Not measurements of any particular box: what reads as a different pedal is the combination
    of where the signal is filtered before the clipper, how hard it is driven, the curve, and
    whether the clipped band is summed back onto the untouched input.
*/
struct ShaperVoicing
{
    /// Ahead of the clipper. What keeps a fuzz from turning low notes to mush.
    double inputHighPassHz { 20.0 };
    /// Drive maps linearly across this range in dB.
    float minDriveDb {}, maxDriveDb {};
    dsp::Waveshape shape { dsp::Waveshape::hyperbolicTangent };
    /// Duty-cycle asymmetry, which is what puts even harmonics in a fuzz.
    float bias {};
    /// The tone control sweeps a low pass across this range.
    double toneMinHz { 20.0 }, toneMaxHz { 20.0 };
    /// Static make-up so models sit at comparable levels at the same Level setting.
    float trimDb {};
    /** Sums the clipped, high-passed signal back onto the untouched input rather than
        replacing it. This is the mid-hump topology a tube screamer gets from clipping inside
        its feedback loop, and it separates an overdrive from a distortion far more than the
        choice of clipping curve does.
    */
    bool parallelClip {};

    /** Slew ceiling ahead of the clipper, in units per sample at 48 kHz. Zero disables it.

        A slow op-amp is not a low-pass filter: it does nothing to small signals and rounds
        large fast ones progressively harder, so it takes the edge off a square wave while
        leaving quiet playing untouched. Scaled by the running sample rate, so a model sounds
        the same at 96 kHz as at 44.1.
    */
    float slewPerSampleAt48k {};

    /** A pair of resonant peaks after the clipper, each swept by one of the aux controls.

        The mid-scooping two-band EQ a gyrator network gives, and the thing that makes the
        harshest models recognisable: a peak at 100 Hz and another around 1.2 kHz, each cut or
        boosted by its own knob. Zero frequency disables a band, so most models pay nothing.
    */
    double gyratorLowHz {};
    double gyratorHighHz {};
    /// How far each aux control moves its band, in dB either side of flat.
    float gyratorRangeDb { 12.0f };
    float gyratorQ { 1.1f };

    /** Integrate the clipping curve across each sample instead of sampling it.

        Removes most of the aliasing a hard edge folds back into the audible band -- see
        `dsp::AntialiasedWaveshaper`. Opt-in rather than always on for one reason: it costs half
        a sample of delay and a little top end, so switching it on changes the sound, and the
        seven built-in archetypes have to keep sounding exactly as they always have.

        Ignored for shapes `dsp::supportsAntiderivative` returns false for.
    */
    bool antiAlias {};
};

/** One pedal, as data.

    Everything that distinguishes a model from its neighbours lives here, so adding one is a
    table row. The engine-specific blocks are plain sub-structs rather than a variant: they are
    small and trivially copyable, and the audio thread reads exactly the one its engine needs
    without branching on a type tag.
*/
struct PedalModel
{
    std::string_view name;
    /// Empty for the built-in archetypes, which are nobody's product.
    std::string_view maker;
    /// One line saying what it does. The interface shows it under the controls and in the
    /// picker, so it lives here rather than in two places that can disagree.
    std::string_view blurb;

    PedalEngine engine { PedalEngine::silent };
    /** Modelling complexity, 1 to 5, as a rough cost weight.

        Reported rather than acted on for now; the performance tier will use it once there are
        engines expensive enough for four of them at once to matter.
    */
    int tier { 1 };

    /// What this model calls each control. An empty entry means the model does not have it.
    std::array<std::string_view, controlCount> controls {};

    ShaperVoicing shaper {};
    ModulationVoicing modulation {};
    CrushVoicing crush {};
    DelayVoicing delay {};
    ReverbVoicing reverb {};
    BassPreampVoicing bassPreamp {};
    PitchVoicing pitch {};
};

/** The model at an index, clamped. Index 0 is always the empty slot.

    Indices are the saved value and the host-visible choice, so entries are appended and never
    reordered.
*/
[[nodiscard]] const PedalModel& pedalModel(int index) noexcept;

/// How many models the table holds.
[[nodiscard]] std::size_t modelCount() noexcept;

/// The index of a built-in archetype, for the call sites that mean a specific one.
[[nodiscard]] inline constexpr int modelIndexOf(PedalKind kind) noexcept
{
    return static_cast<int>(kind);
}

/** One slot's settings.

    The controls mean roughly the same thing in every model -- drive into the nonlinearity, tone
    above it, output level, how much is blended back, and up to two model-specific voicing
    controls -- so the page needs one set of knobs rather than one per model.
*/
struct PedalParameters
{
    /// Index into the model table. 0 is the empty slot.
    int model {};
    /// A momentary A/B, distinct from an empty slot: it keeps the settings and the loaded model.
    bool bypassed {};
    /// 0 to 10.
    float drive { 5.0f };
    /// 0 to 10.
    float tone { 5.0f };
    /// Output trim, in dB, applied to the wet signal before the blend.
    float levelDb {};
    /// 0 to 100 percent wet. At 0 the slot is exactly transparent whatever else it is doing.
    float mix { 100.0f };
    /// Model-specific voicing controls, 0 to 10. Ignored by models that do not name them.
    float auxA { 5.0f };
    float auxB { 5.0f };
};

/** One pedal.

    Everything is sized in `prepare`; `process` allocates nothing and takes no locks, so it
    is safe on the audio thread. A neural slot holds its own `NeuralAmpProcessor`, which is
    what gives it staged loading, test-vector validation and a click-free model swap for
    free rather than a second implementation of all three.

    Changing kind, and switching bypass, both ride the wet/dry blend down to silence first
    and only then swap. A pedal appearing or vanishing under the signal is a step change,
    and stepping is exactly what a footswitch is not allowed to do.
*/
class PedalSlot
{
public:
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    /// Called once per block from the audio thread, in the same way the amplifier is driven.
    void setParameters(const PedalParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

    /// The model host for this slot. Staging a model is message-thread work and happens here.
    [[nodiscard]] ml::NeuralAmpProcessor& model() noexcept { return neural; }
    [[nodiscard]] const ml::NeuralAmpProcessor& model() const noexcept { return neural; }

    /// True when the slot is set to something and switched in.
    [[nodiscard]] bool active() const noexcept
    {
        return requested.model != 0 && ! requested.bypassed;
    }
    /// Named `modelIndex` and not `model` because `model()` above is the neural host.
    [[nodiscard]] int modelIndex() const noexcept { return requested.model; }

private:
    /** Points a smoother at a new value only when the value has actually moved.

        Not an optimisation. `SmoothedParameter::setTarget` restarts its ramp, so a
        smoother retargeted every block never finishes one: it converges geometrically but
        `isSmoothing()` stays true forever, and this class decides when a slot has reached
        full dry -- and may therefore be skipped or swapped -- from exactly that.
    */
    static void retarget(dsp::SmoothedParameter& parameter, float& cached, float value) noexcept;
    [[nodiscard]] float driveTarget() const noexcept;
    [[nodiscard]] float levelTarget() const noexcept;
    [[nodiscard]] float mixTarget() const noexcept;
    void resetVoice() noexcept;
    /// Opts the slot's model host into input compensation and hands it the slot's controls.
    void configureNeural() noexcept;
    void updateFilters() noexcept;
    void renderShaped(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void renderCompressor(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void renderNeural(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void renderModulation(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void renderCrush(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void renderDelay(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void renderReverb(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void renderBassPreamp(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void renderPitch(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

    dsp::ProcessSpec spec;
    /// What the controls currently ask for. `activeModel` catches up once the blend is down.
    PedalParameters requested;
    int activeModel {};
    bool switching {};
    /// Guards the coefficient recomputation, which costs far more than the compare.
    float filteredTone { -1.0f };
    float filteredAuxA { -1.0f }, filteredAuxB { -1.0f };
    int filteredModel { -1 };
    /// Last value handed to each smoother; see retarget.
    float cachedDrive { -1.0f }, cachedLevel { -1.0f }, cachedMix { -1.0f };

    dsp::Biquad inputFilter;
    dsp::Biquad toneFilter;
    /// The two gyrator peaks, idle unless the model names their frequencies.
    dsp::Biquad gyratorLow, gyratorHigh;
    dsp::SlewLimiter slewLimiter;
    dsp::AntialiasedWaveshaper antialiased;
    dsp::Compressor compressor;
    ml::NeuralAmpProcessor neural;

    /// Modulation: one swept line, plus the all-pass chain a phaser uses instead of it.
    static constexpr std::size_t maximumPhaserStages = 8;
    dsp::ModulatedDelayLine modulationLine;

    /** One first-order all-pass section, which is what a phaser stage actually is.

        Not a `dsp::Biquad`: that is a block processor with interpolated coefficients, and a
        phaser needs its coefficient to move *within* a block or the sweep steps. A real phaser
        stage is one JFET and one capacitor -- first order -- so second-order machinery would
        also be modelling something that is not there.
    */
    struct AllPass
    {
        float previousInput {}, previousOutput {};

        [[nodiscard]] float process(float input, float coefficient) noexcept
        {
            const auto output = coefficient * input + previousInput - coefficient * previousOutput;
            previousInput = input;
            previousOutput = output;
            return output;
        }
    };

    std::array<std::array<AllPass, maximumPhaserStages>, dsp::maximumChannels> phaserStages;
    std::array<float, dsp::maximumChannels> modulationFeedback {};
    /// LFO phase in cycles, 0 to 1. One oscillator, offset per channel for width.
    double lfoPhase {};

    /// Crush: the held sample and how long it has been held, per channel.
    std::array<float, dsp::maximumChannels> crushHeld {};
    std::array<float, dsp::maximumChannels> crushCounter {};
    dsp::Biquad crushFilter;

    /// Delay and reverb wrap the shared effects rather than reimplementing them; the spring
    /// dispersion is the only thing `dsp::Reverb` does not already provide.
    dsp::Delay delayLine;
    dsp::Reverb reverbTank;
    static constexpr std::size_t maximumSpringStages = 6;
    std::array<std::array<AllPass, maximumSpringStages>, dsp::maximumChannels> springStages;

    /// Bass preamp: the crossover, and scratch for the two bands it splits into.
    dsp::LinkwitzRileyCrossover bassCrossover;
    std::array<std::vector<float>, dsp::maximumChannels> lowBand, highBand;
    std::array<float*, dsp::maximumChannels> lowPointers {}, highPointers {};
    dsp::Biquad bassToneFilter;

    /// Pitch: its own line, because its window is far longer than the modulation one's sweep.
    dsp::ModulatedDelayLine pitchLine;
    /// Where each voice's tap sits in its window, 0 to 1. One phase per voice, shared across
    /// channels so a stereo signal shifts coherently rather than drifting apart.
    double pitchPhase {}, pitchPhaseSecond {};
    std::array<float, dsp::maximumChannels> pitchFeedback {};

    dsp::SmoothedParameter driveGain;
    dsp::SmoothedParameter outputGain;
    dsp::SmoothedParameter wetMix;

    /// The untouched input, kept so the blend has something to blend against.
    std::array<std::vector<float>, dsp::maximumChannels> dryScratch;
    /// The high-passed copy a parallel clipper works on.
    std::array<std::vector<float>, dsp::maximumChannels> bandScratch;
    std::array<float*, dsp::maximumChannels> bandPointers {};
};

/** The slots in series, ahead of the amplifier.

    Held as one object rather than four loose slots so the processor has a single thing to
    prepare, reset and run, and so "is there any pedal at all" is one question.
*/
class PedalBoard
{
public:
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void setParameters(std::size_t slot, const PedalParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

    [[nodiscard]] PedalSlot& slot(std::size_t index) noexcept { return slots[index]; }
    [[nodiscard]] const PedalSlot& slot(std::size_t index) const noexcept { return slots[index]; }
    /// False when every slot is `none` or bypassed, which is the default rig.
    [[nodiscard]] bool anyActive() const noexcept;

    /** What the board currently costs, as the sum of its active models' tiers.

        A rough weight, not a measurement: tier 5 is not literally five times tier 1. It exists
        so the interface can tell a user that four of the most expensive engines at once is more
        than the chosen performance tier is meant to carry -- and *tell* them rather than
        silently degrade something they deliberately chose, which is the worse failure.
    */
    [[nodiscard]] int activeCost() const noexcept;

private:
    std::array<PedalSlot, slotCount> slots;
};
} // namespace nts::pedals
