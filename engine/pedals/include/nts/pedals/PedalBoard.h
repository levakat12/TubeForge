#pragma once

#include <nts/dsp/Common.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Filters.h>
#include <nts/dsp/Nonlinear.h>
#include <nts/dsp/Smoothing.h>
#include <nts/ml/NeuralAmpProcessor.h>

#include <array>
#include <cstddef>
#include <vector>

namespace nts::pedals
{
/** What a slot is running.

    `none` is the default and is the reason this whole stage can be absent: a board whose
    slots are all `none` leaves the buffer untouched and returns before it touches a filter,
    so the rig really is amplifier and cabinet alone rather than a chain of unity-gain
    effects pretending to be nothing.

    The order is part of the saved state and of the host-visible choice parameter, so new
    kinds are appended rather than inserted.
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

inline constexpr std::size_t kindCount = 7;

/// Slots on the board, in signal order. Four is what fits on the page and is about as many
/// neural captures as a machine will carry alongside an amplifier.
inline constexpr std::size_t slotCount = 4;

/** One slot's settings.

    The four continuous controls mean the same thing in every kind -- drive into the
    nonlinearity, tone above it, output level, and how much of the result is blended back
    against the dry signal -- so the page needs one set of knobs rather than seven.
*/
struct PedalParameters
{
    PedalKind kind { PedalKind::none };
    /// A momentary A/B, distinct from `none`: it keeps the slot's settings and its loaded model.
    bool bypassed {};
    /// 0 to 10.
    float drive { 5.0f };
    /// 0 to 10.
    float tone { 5.0f };
    /// Output trim, in dB, applied to the wet signal before the blend.
    float levelDb {};
    /// 0 to 100 percent wet. At 0 the slot is exactly transparent whatever else it is doing.
    float mix { 100.0f };
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
        return requested.kind != PedalKind::none && ! requested.bypassed;
    }
    [[nodiscard]] PedalKind kind() const noexcept { return requested.kind; }

private:
    /// The fixed part of a kind: everything about it that no control changes.
    struct Voicing
    {
        /// Ahead of the clipper. What keeps a fuzz from turning low notes to mush.
        double inputHighPassHz;
        /// Drive maps linearly across this range in dB.
        float minDriveDb, maxDriveDb;
        dsp::Waveshape shape;
        /// Duty-cycle asymmetry, which is what puts even harmonics in a fuzz.
        float bias;
        /// The tone control sweeps a low pass across this range.
        double toneMinHz, toneMaxHz;
        /// Static make-up so the kinds sit at comparable levels at the same Level setting.
        float trimDb;
        /// Sums the clipped, high-passed signal back onto the untouched input rather than
        /// replacing it. This is the mid-hump topology a tube screamer gets from clipping
        /// inside its feedback loop, and it separates an overdrive from a distortion far
        /// more than the choice of clipping curve does.
        bool parallelClip;
    };

    [[nodiscard]] static const Voicing& voicingFor(PedalKind kind) noexcept;
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

    dsp::ProcessSpec spec;
    /// What the controls currently ask for. `activeKind` catches up once the blend is down.
    PedalParameters requested;
    PedalKind activeKind { PedalKind::none };
    bool switching {};
    /// Guards the coefficient recomputation, which costs far more than the compare.
    float filteredTone { -1.0f };
    PedalKind filteredKind { PedalKind::none };
    /// Last value handed to each smoother; see retarget.
    float cachedDrive { -1.0f }, cachedLevel { -1.0f }, cachedMix { -1.0f };

    dsp::Biquad inputFilter;
    dsp::Biquad toneFilter;
    dsp::Compressor compressor;
    ml::NeuralAmpProcessor neural;

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

private:
    std::array<PedalSlot, slotCount> slots;
};
} // namespace nts::pedals
