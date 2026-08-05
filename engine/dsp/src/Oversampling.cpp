#include "nts/dsp/Oversampling.h"

#include "nts/dsp/Simd.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace nts::dsp
{
namespace { constexpr auto pi = 3.14159265358979323846; }

void Oversampler::prepare(const ProcessSpec& newSpec, OversamplingFactor newFactor,
                          std::vector<float>* sharedWork)
{
    externalWork = sharedWork;
    spec = newSpec;
    spec.channels = std::min(spec.channels, maximumChannels);
    oversamplingFactor = static_cast<std::size_t>(newFactor);
    if (oversamplingFactor != 1 && oversamplingFactor != 2
        && oversamplingFactor != 4 && oversamplingFactor != 8)
        oversamplingFactor = 1;
    designFilter();
    const auto upHistorySize = coefficients.empty() ? 0
        : (coefficients.size() + oversamplingFactor - 1) / oversamplingFactor;
    upPhaseLength = upHistorySize;

    // Deinterleave into per-phase sub-filters, reversed and front-padded to a uniform length.
    // Phase p's taps are coefficients[p], coefficients[p + factor], ... -- tap k of phase p is
    // coefficients[p + k * factor]. Reversed and right-aligned, tap k lands at index
    // upPhaseLength - 1 - k, which leaves the padding at the front where it contributes zero.
    upPhaseCoefficients.assign(upPhaseLength * oversamplingFactor, 0.0f);
    for (std::size_t phase = 0; phase < oversamplingFactor && ! coefficients.empty(); ++phase)
    {
        auto* const destination = upPhaseCoefficients.data() + phase * upPhaseLength;
        for (std::size_t tap = 0, index = phase; index < coefficients.size();
             ++tap, index += oversamplingFactor)
            destination[upPhaseLength - 1 - tap] = coefficients[index];
    }

    downReversedCoefficients.assign(coefficients.rbegin(), coefficients.rend());

    upHistory.assign(2 * upHistorySize * spec.channels, 0.0f);
    downHistory.assign(2 * coefficients.size() * spec.channels, 0.0f);
    // A shared buffer is the caller's to size and clear; only the owned one is allocated here.
    if (externalWork == nullptr)
        work.assign(workFloatsFor(spec, oversamplingFactor), 0.0f);
    else
        work.clear();
}

void Oversampler::reset() noexcept
{
    std::fill(upHistory.begin(), upHistory.end(), 0.0f);
    std::fill(downHistory.begin(), downHistory.end(), 0.0f);
    upPosition.fill(0);
    downPosition.fill(0);
    std::fill(workBuffer().begin(), workBuffer().end(), 0.0f);
}

double Oversampler::filterMagnitude(double normalizedFrequency) const noexcept
{
    if (coefficients.empty()) return 1.0;
    double real {};
    double imaginary {};
    for (std::size_t index = 0; index < coefficients.size(); ++index)
    {
        const auto angle = -2.0 * pi * normalizedFrequency * static_cast<double>(index);
        real += coefficients[index] * std::cos(angle);
        imaginary += coefficients[index] * std::sin(angle);
    }
    return std::hypot(real, imaginary);
}

bool Oversampler::hasLinearPhaseCoefficients() const noexcept
{
    for (std::size_t index = 0; index < coefficients.size() / 2; ++index)
        if (std::abs(coefficients[index] - coefficients[coefficients.size() - 1 - index]) > 1.0e-7f)
            return false;
    return true;
}

void Oversampler::designFilter()
{
    coefficients.clear();
    if (oversamplingFactor == 1)
        return;
    const auto length = antiAliasTapsPerPhase * oversamplingFactor + 1;
    coefficients.resize(length);
    const auto midpoint = static_cast<double>(length - 1) * 0.5;
    const auto cutoff = 0.46 / static_cast<double>(oversamplingFactor);
    for (std::size_t index = 0; index < length; ++index)
    {
        const auto offset = static_cast<double>(index) - midpoint;
        const auto sinc = std::abs(offset) < 1.0e-12
            ? 2.0 * cutoff : std::sin(2.0 * pi * cutoff * offset) / (pi * offset);
        const auto window = 0.42 - 0.5 * std::cos(2.0 * pi * static_cast<double>(index) / (length - 1))
                          + 0.08 * std::cos(4.0 * pi * static_cast<double>(index) / (length - 1));
        coefficients[index] = static_cast<float>(sinc * window);
    }
    const auto sum = std::accumulate(coefficients.begin(), coefficients.end(), 0.0f);
    for (auto& coefficient : coefficients)
        coefficient /= sum;
}

void Oversampler::upsamplePolyphase(const float* input, std::size_t samples, float* output,
                                    std::size_t channel) noexcept
{
    if (coefficients.empty()) return;
    const auto historySize = upPhaseLength;
    auto* const channelHistory = upHistory.data() + channel * 2 * historySize;
    const auto gain = static_cast<float>(oversamplingFactor);
    auto position = upPosition[channel];
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto value = input[sample];
        channelHistory[position] = value;
        channelHistory[position + historySize] = value;
        // One window for every phase: the newest sample sits at the end of the span, and each
        // phase's front padding absorbs the difference in tap count.
        const auto* const window = channelHistory + position + 1;
        auto* const destination = output + sample * oversamplingFactor;
        for (std::size_t phase = 0; phase < oversamplingFactor; ++phase)
            destination[phase] = gain * dotProduct(upPhaseCoefficients.data() + phase * historySize,
                                                   window, historySize);
        // Conditional subtract rather than a modulo: historySize is not a power of two, so this
        // was an integer division per sample -- 20 to 40 cycles on the older CPUs this has to
        // run on, and not pipelined.
        if (++position == historySize) position = 0;
    }
    upPosition[channel] = position;
}

void Oversampler::downsamplePolyphase(const float* input, std::size_t samples, float* output,
                                      std::size_t channel) noexcept
{
    if (coefficients.empty()) return;
    const auto historySize = coefficients.size();
    auto* const channelHistory = downHistory.data() + channel * 2 * historySize;
    auto position = downPosition[channel];
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        for (std::size_t phase = 0; phase < oversamplingFactor; ++phase)
        {
            const auto value = input[sample * oversamplingFactor + phase];
            channelHistory[position] = value;
            channelHistory[position + historySize] = value;
            // Only phase zero produces an output sample -- that is the decimation. Every phase
            // still has to enter the delay line, or the filter would be convolving a signal
            // with holes in it.
            if (phase == 0)
                output[sample] = dotProduct(downReversedCoefficients.data(),
                                            channelHistory + position + 1, historySize);
            // Runs at the oversampled rate, so this division was costing up to eight times per
            // input sample. See upsamplePolyphase.
            if (++position == historySize) position = 0;
        }
    }
    downPosition[channel] = position;
}
} // namespace nts::dsp
