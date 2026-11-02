#pragma once

#include "Common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace nts::dsp
{
enum class OversamplingFactor : std::size_t { x1 = 1, x2 = 2, x4 = 4, x8 = 8 };

/** Anti-alias FIR taps per polyphase branch.

    The filter is `antiAliasTapsPerPhase * factor + 1` taps long, so this value is
    both the cost per output sample and the round-trip latency in base-rate
    samples (the up and down filters each contribute half).

    8 taps per phase measures about -30 dB of fold-back rejection at 4x (see the
    aliasing probe in AmpTests). Raising it to 16, 24, or 32 was measured and did
    not improve that figure, while the benchmark cost of oversampler4x rose from
    0.35 to 0.62, 0.96, and 1.21. The residual is not bounded by this filter's
    stopband, so spending taps on it buys nothing.

    **Re-measured on an instrument that works, and the conclusion holds for a better reason.**
    Two probes have since been thrown away for having floors of their own -- the last one leaked a
    0.7-amplitude fundamental across every bin and bottomed out at -37 dB, which made oversampling
    look as though it stopped paying past 2x. `worstFoldbackDb` in AmpTests now places every probe
    on an exact DFT bin, so there is no floor above the arithmetic. What this stage actually does
    at 30 dB drive:

        curve      ADAA    1x      2x      4x      8x
        tanh       off   -22.0   -34.7   -52.4   -57.9
        tanh       on    -31.5   -44.6   -56.2   -57.9
        hardClip   off   -21.3   -32.2   -48.5   -56.3
        hardClip   on    -31.0   -43.0   -55.1   -56.3

    So the factor is worth about **12 dB per doubling up to 4x**, which is a great deal more than
    the earlier numbers claimed, and this filter is not what bounds it.

    A three-stage halfband cascade -- 61/25/17 taps, roughly 80 dB of stopband against this
    design's ~30 -- was built and measured against the *previous* probe. It moved fold-back by
    less than 1.5 dB, and at 2x, where that probe was above its floor and the comparison was
    sound, it was no better at all. It cost about 1.6x the filter work and four times the
    round-trip latency, so it is not here. The 4x comparison was taken at the floor and proved
    nothing either way; anyone revisiting it now has an instrument that could answer properly.

    The reason a better filter buys so little: a saturator driven at 30 dB puts harmonics above the
    **oversampled** Nyquist as well, and those fold inside the high-rate domain before the
    decimator exists to stop them. The anti-alias pair only ever governs the band between the base
    Nyquist and the oversampled one, and that is not where the residual lives.

    The two levers that do work are the factor itself, and
    `PreampStageConfig::antialiasedSaturation` -- which is worth **9 to 11 dB at 1x and 2x**,
    because it band-limits the harmonics as the curve generates them instead of resampling to
    outrun them afterwards. `4x + ADAA` lands within 2 dB of plain `8x` for half the work.
*/
inline constexpr std::size_t antiAliasTapsPerPhase = 8;

class Oversampler
{
public:
    /** Prepares the filters and delay lines.

        `sharedWork` optionally supplies the high-rate scratch buffer instead of the instance
        allocating its own. A stage that keeps one Oversampler per selectable factor -- which is
        how the preamp avoids allocating when the factor changes under audio -- otherwise carries
        1+2+4+8 = fifteen times the block size in scratch, of which only one is ever in use. The
        caller must size it for the worst factor it will ask for and must not share it between
        instances that can run concurrently; the preamp cannot, because only one factor is
        selected at a time.

        Passing nullptr keeps the self-contained behaviour.
    */
    void prepare(const ProcessSpec& spec, OversamplingFactor factor,
                 std::vector<float>* sharedWork = nullptr);

    /// Scratch floats a given configuration needs, so a caller can size a shared buffer.
    [[nodiscard]] static constexpr std::size_t workFloatsFor(const ProcessSpec& spec,
                                                             std::size_t factor) noexcept
    { return spec.maximumBlockSize * factor * std::min(spec.channels, maximumChannels); }
    void reset() noexcept;
    [[nodiscard]] std::size_t factor() const noexcept { return oversamplingFactor; }
    [[nodiscard]] std::size_t latencySamples() const noexcept
    {
        // Group delay of the up and down filters combined, expressed at the base
        // rate. Derived from the coefficients so it cannot drift from the design.
        return coefficients.empty() ? 0 : (coefficients.size() - 1) / oversamplingFactor;
    }
    [[nodiscard]] double exactLatencySamples() const noexcept { return static_cast<double>(latencySamples()); }
    [[nodiscard]] std::size_t filterLength() const noexcept { return coefficients.size(); }
    [[nodiscard]] double filterMagnitude(double normalizedFrequency) const noexcept;
    [[nodiscard]] bool hasLinearPhaseCoefficients() const noexcept;

    /** As `process`, but the nonlinearity is told which channel it is running on.

        For a *stateful* curve -- antiderivative anti-aliasing keeps one previous sample per
        channel -- and for nothing else. The plain `process` below traverses channel-major, so a
        caller could in principle recover the channel by counting calls and dividing by the block
        length; that works today and breaks silently the moment this loop is restructured or
        vectorised across channels. Passing the index costs nothing and cannot go quietly wrong.

        `nonlinear` is invoked as `nonlinear(sample, channel)`.
    */
    template <typename NonlinearFunction>
    void processIndexed(float* const* channels, std::size_t channelCount, std::size_t samples,
                        NonlinearFunction&& nonlinear) noexcept
    {
        const auto count = std::min({ channelCount, spec.channels, maximumChannels });
        if (oversamplingFactor == 1)
        {
            for (std::size_t channel = 0; channel < count; ++channel)
                for (std::size_t sample = 0; sample < samples; ++sample)
                    channels[channel][sample] = nonlinear(channels[channel][sample], channel);
            return;
        }

        const auto highRateSamples = samples * oversamplingFactor;
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            auto* highRate = workBuffer().data() + channel * spec.maximumBlockSize * oversamplingFactor;
            upsamplePolyphase(channels[channel], samples, highRate, channel);
            for (std::size_t sample = 0; sample < highRateSamples; ++sample)
                highRate[sample] = nonlinear(highRate[sample], channel);
            downsamplePolyphase(highRate, samples, channels[channel], channel);
        }
    }

    template <typename NonlinearFunction>
    void process(float* const* channels, std::size_t channelCount, std::size_t samples,
                 NonlinearFunction&& nonlinear) noexcept
    {
        const auto count = std::min({ channelCount, spec.channels, maximumChannels });
        if (oversamplingFactor == 1)
        {
            for (std::size_t channel = 0; channel < count; ++channel)
                for (std::size_t sample = 0; sample < samples; ++sample)
                    channels[channel][sample] = nonlinear(channels[channel][sample]);
            return;
        }

        const auto highRateSamples = samples * oversamplingFactor;
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            auto* highRate = workBuffer().data() + channel * spec.maximumBlockSize * oversamplingFactor;
            upsamplePolyphase(channels[channel], samples, highRate, channel);
            for (std::size_t sample = 0; sample < highRateSamples; ++sample)
                highRate[sample] = nonlinear(highRate[sample]);
            downsamplePolyphase(highRate, samples, channels[channel], channel);
        }
    }

private:
    void designFilter();
    void upsamplePolyphase(const float* input, std::size_t samples, float* output,
                           std::size_t channel) noexcept;
    void downsamplePolyphase(const float* input, std::size_t samples, float* output,
                             std::size_t channel) noexcept;

    ProcessSpec spec;
    std::size_t oversamplingFactor { 1 };
    /// The designed filter, kept in natural order because filterMagnitude and the linear-phase
    /// check are both stated in terms of it. The two layouts below are derived from it.
    std::vector<float> coefficients;

    /** The upsampler's polyphase sub-filters, deinterleaved and reversed.

        Phase `p` takes taps `coefficients[p], coefficients[p + factor], ...`, which is a strided
        read of the natural layout. Deinterleaving at design time makes each phase contiguous;
        reversing it makes the delay-line window run forward, so the whole inner loop collapses
        to dsp::dotProduct. Every phase is padded at the *front* to a uniform `historySize`, so
        all phases share one window and the shorter ones contribute leading zeros rather than
        needing their own bounds.

        Laid out phase-major: phase `p` occupies `[p * upPhaseLength, (p + 1) * upPhaseLength)`.
    */
    std::vector<float> upPhaseCoefficients;
    std::size_t upPhaseLength {};
    /// The full filter, reversed, for the decimator's single dot product per output sample.
    std::vector<float> downReversedCoefficients;

    /** Both delay lines are doubled -- every sample written at `position` and at
        `position + size` -- so the taps a given output needs are always one contiguous forward
        span. See DirectConvolver, which uses the same layout for the same reason.
    */
    std::vector<float> upHistory;
    std::vector<float> downHistory;
    std::array<std::size_t, maximumChannels> upPosition {};
    std::array<std::size_t, maximumChannels> downPosition {};
    /// Owned scratch, used only when no shared buffer was supplied.
    std::vector<float> work;
    std::vector<float>* externalWork {};
    [[nodiscard]] std::vector<float>& workBuffer() noexcept
    { return externalWork != nullptr ? *externalWork : work; }
};
} // namespace nts::dsp
