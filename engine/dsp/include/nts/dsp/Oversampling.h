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

class Oversampler
{
public:
    void prepare(const ProcessSpec& spec, OversamplingFactor factor);
    void reset() noexcept;
    [[nodiscard]] std::size_t factor() const noexcept { return oversamplingFactor; }
    [[nodiscard]] std::size_t latencySamples() const noexcept { return oversamplingFactor == 1 ? 0 : 8; }
    [[nodiscard]] double exactLatencySamples() const noexcept { return static_cast<double>(latencySamples()); }
    [[nodiscard]] std::size_t filterLength() const noexcept { return coefficients.size(); }
    [[nodiscard]] double filterMagnitude(double normalizedFrequency) const noexcept;
    [[nodiscard]] bool hasLinearPhaseCoefficients() const noexcept;

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
            auto* highRate = work.data() + channel * spec.maximumBlockSize * oversamplingFactor;
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
    std::vector<float> coefficients;
    std::vector<float> upHistory;
    std::vector<float> downHistory;
    std::array<std::size_t, maximumChannels> upPosition {};
    std::array<std::size_t, maximumChannels> downPosition {};
    std::vector<float> work;
};
} // namespace nts::dsp
