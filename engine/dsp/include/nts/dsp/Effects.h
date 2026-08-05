#pragma once

#include "Common.h"
#include "Filters.h"
#include "Smoothing.h"

#include <array>
#include <cstddef>
#include <vector>

namespace nts::dsp
{
struct DelayParameters
{
    float timeMs { 375.0f };
    /// 0 to 0.95. Clamped below unity so the line always decays.
    float feedback { 0.35f };
    /// Wet level, 0 to 1. The dry path is left at unity: this is a send, not a crossfade.
    float mix { 0.25f };
    /// Rolls the repeats off, which is what stops a bright amp turning them into hiss.
    float dampingHz { 6000.0f };
    /// Fraction of the delay applied to the right channel, for a wider stereo image.
    float stereoSpread { 0.0f };
};

/** Feedback delay with a damped repeat path.

    The delay time is smoothed rather than jumped so that turning the control does not click,
    which means the read position moves fractionally and is interpolated.
*/
class Delay
{
public:
    void prepare(const ProcessSpec& spec, float maximumTimeMs = 2000.0f);
    void reset() noexcept;
    void setParameters(const DelayParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    /// Longest audible decay, for tail reporting.
    [[nodiscard]] std::size_t tailSamples() const noexcept;

private:
    ProcessSpec spec;
    DelayParameters parameters;
    std::size_t capacity {};
    std::vector<float> buffer;
    std::array<std::size_t, maximumChannels> writePosition {};
    std::array<float, maximumChannels> damped {};
    std::array<SmoothedParameter, maximumChannels> delaySamples;
    float dampingCoefficient {};
    /// The cutoff dampingCoefficient was derived from, so the exp is not repeated per block.
    float dampingCoefficientHz { -1.0f };
    bool engaged { true };
    bool needsReset {};
};

/** Mix level at which a send stops being worth computing, with hysteresis so a control resting
    on the boundary cannot oscillate.

    At zero mix the wet signal is multiplied away, and for the reverb that is four comb filters
    and two all-passes per channel computed for nothing. Skipping leaves the delay lines holding
    whatever was in them, so re-engaging clears them first and lets the tail build from silence
    -- which is continuous by construction, and is also what a listener expects when they bring a
    send up. The alternative, replaying however many seconds of frozen audio the line happened to
    be holding, is what the reverb did before.
*/
inline constexpr float sendEngageThreshold = 0.002f;
inline constexpr float sendDisengageThreshold = 0.001f;
void updateSendEngagement(bool& engaged, bool& needsReset, float mix) noexcept;

struct ReverbParameters
{
    /// 0 to 1, small room through to a long hall.
    float size { 0.5f };
    /// 0 to 1. Higher values absorb the top end faster, as a real room does.
    float damping { 0.5f };
    float mix { 0.2f };
    /// Rolls off the bottom of the tail so it does not muddy a bass guitar.
    float lowCutHz { 180.0f };
};

/** Schroeder reverberator: four damped parallel combs into two series all-passes.

    Chosen over convolution deliberately. The cabinet's convolver is time-domain and capped at
    4096 taps, which is 85 ms -- shorter than the shortest useful reverb tail, and the cost is
    linear in tap count. A recursive network gives seconds of decay for a fixed handful of
    operations per sample, which is the right trade for something running alongside an
    oversampled amplifier.
*/
class Reverb
{
public:
    void prepare(const ProcessSpec& spec);
    void reset() noexcept;
    void setParameters(const ReverbParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::size_t tailSamples() const noexcept;

private:
    static constexpr std::size_t combCount = 4;
    static constexpr std::size_t allPassCount = 2;

    struct Line
    {
        std::vector<float> buffer;
        std::size_t position {};
        float store {};
    };

    ProcessSpec spec;
    ReverbParameters parameters;
    std::array<std::array<Line, combCount>, maximumChannels> combs;
    std::array<std::array<Line, allPassCount>, maximumChannels> allPasses;
    /// One instance: Biquad keeps per-channel state internally.
    Biquad lowCut;
    /// The wet signal is built here so the low cut lands on the tail alone. Filtering the
    /// summed output instead would strip the bottom off the amplifier itself.
    std::vector<float> wetBuffer;
    float feedback {};
    float damping {};
    bool engaged { true };
    bool needsReset {};
    /// The cutoff the lowCut coefficients were derived from, so the design is not repeated.
    float lowCutCoefficientHz { -1.0f };
};
} // namespace nts::dsp
