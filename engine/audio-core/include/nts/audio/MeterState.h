#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace nts::audio
{
class MeterState
{
public:
    static constexpr std::size_t maximumChannels = 2;

    void publish(std::size_t channel, float inputPeakValue, float outputPeakValue) noexcept
    {
        if (channel >= maximumChannels)
            return;
        inputPeaks[channel].store(inputPeakValue, std::memory_order_relaxed);
        outputPeaks[channel].store(outputPeakValue, std::memory_order_relaxed);
    }

    [[nodiscard]] float inputPeak(std::size_t channel) const noexcept
    {
        return channel < maximumChannels ? inputPeaks[channel].load(std::memory_order_relaxed) : 0.0f;
    }

    [[nodiscard]] float outputPeak(std::size_t channel) const noexcept
    {
        return channel < maximumChannels ? outputPeaks[channel].load(std::memory_order_relaxed) : 0.0f;
    }

private:
    std::array<std::atomic<float>, maximumChannels> inputPeaks {};
    std::array<std::atomic<float>, maximumChannels> outputPeaks {};
};
} // namespace nts::audio
