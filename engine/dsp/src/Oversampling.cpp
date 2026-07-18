#include "nts/dsp/Oversampling.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace nts::dsp
{
namespace { constexpr auto pi = 3.14159265358979323846; }

void Oversampler::prepare(const ProcessSpec& newSpec, OversamplingFactor newFactor)
{
    spec = newSpec;
    spec.channels = std::min(spec.channels, maximumChannels);
    oversamplingFactor = static_cast<std::size_t>(newFactor);
    if (oversamplingFactor != 1 && oversamplingFactor != 2
        && oversamplingFactor != 4 && oversamplingFactor != 8)
        oversamplingFactor = 1;
    designFilter();
    const auto upHistorySize = coefficients.empty() ? 0
        : (coefficients.size() + oversamplingFactor - 1) / oversamplingFactor;
    upHistory.assign(upHistorySize * spec.channels, 0.0f);
    downHistory.assign(coefficients.size() * spec.channels, 0.0f);
    work.assign(spec.maximumBlockSize * oversamplingFactor * spec.channels, 0.0f);
}

void Oversampler::reset() noexcept
{
    std::fill(upHistory.begin(), upHistory.end(), 0.0f);
    std::fill(downHistory.begin(), downHistory.end(), 0.0f);
    upPosition.fill(0);
    downPosition.fill(0);
    std::fill(work.begin(), work.end(), 0.0f);
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
    const auto length = 8 * oversamplingFactor + 1;
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
    const auto historySize = (coefficients.size() + oversamplingFactor - 1) / oversamplingFactor;
    auto* channelHistory = upHistory.data() + channel * historySize;
    auto position = upPosition[channel];
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        channelHistory[position] = input[sample];
        for (std::size_t phase = 0; phase < oversamplingFactor; ++phase)
        {
            double value {};
            auto historyPosition = position;
            for (std::size_t tap = phase; tap < coefficients.size(); tap += oversamplingFactor)
            {
                value += coefficients[tap] * channelHistory[historyPosition];
                historyPosition = historyPosition == 0 ? historySize - 1 : historyPosition - 1;
            }
            output[sample * oversamplingFactor + phase]
                = static_cast<float>(value * static_cast<double>(oversamplingFactor));
        }
        position = (position + 1) % historySize;
    }
    upPosition[channel] = position;
}

void Oversampler::downsamplePolyphase(const float* input, std::size_t samples, float* output,
                                      std::size_t channel) noexcept
{
    if (coefficients.empty()) return;
    const auto historySize = coefficients.size();
    auto* channelHistory = downHistory.data() + channel * historySize;
    auto position = downPosition[channel];
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        for (std::size_t phase = 0; phase < oversamplingFactor; ++phase)
        {
            channelHistory[position] = input[sample * oversamplingFactor + phase];
            if (phase == 0)
            {
                double value {};
                auto historyPosition = position;
                for (const auto coefficient : coefficients)
                {
                    value += coefficient * channelHistory[historyPosition];
                    historyPosition = historyPosition == 0 ? historySize - 1 : historyPosition - 1;
                }
                output[sample] = static_cast<float>(value);
            }
            position = (position + 1) % historySize;
        }
    }
    downPosition[channel] = position;
}
} // namespace nts::dsp
