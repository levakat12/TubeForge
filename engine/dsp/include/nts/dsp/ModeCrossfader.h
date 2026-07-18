#pragma once

#include "Common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace nts::dsp
{
class ModeCrossfader
{
public:
    void prepare(const ProcessSpec& newSpec, std::size_t newModeCount)
    {
        spec = newSpec;
        spec.channels = std::clamp(spec.channels, std::size_t { 1 }, maximumChannels);
        modeCount = std::max<std::size_t>(1, newModeCount);
        oldBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
        newBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
        reset();
    }

    void reset(std::size_t initialMode = 0) noexcept
    {
        const auto mode = std::min(initialMode, modeCount - 1);
        activeMode.store(mode, std::memory_order_relaxed);
        requestedMode.store(noRequest, std::memory_order_relaxed);
        targetMode = mode;
        fadeLength = 1;
        fadeRemaining = 0;
    }

    void requestMode(std::size_t mode, std::size_t crossfadeSamples) noexcept
    {
        if (mode < modeCount)
        {
            requestedFadeLength.store(std::max<std::size_t>(1, crossfadeSamples), std::memory_order_relaxed);
            requestedMode.store(mode, std::memory_order_release);
        }
    }

    [[nodiscard]] std::size_t mode() const noexcept
    {
        return activeMode.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isCrossfading() const noexcept { return fadeRemaining != 0; }

    template <typename Processor>
    void process(float* const* channels, std::size_t channelCount, std::size_t samples,
                 Processor&& processor) noexcept
    {
        const auto count = std::min(channelCount, spec.channels);
        const auto processSamples = std::min(samples, spec.maximumBlockSize);
        if (count == 0 || processSamples == 0)
            return;

        if (fadeRemaining == 0)
        {
            const auto request = requestedMode.exchange(noRequest, std::memory_order_acq_rel);
            const auto current = activeMode.load(std::memory_order_relaxed);
            if (request != noRequest && request != current)
            {
                targetMode = request;
                fadeLength = requestedFadeLength.load(std::memory_order_relaxed);
                fadeRemaining = fadeLength;
            }
        }

        const auto current = activeMode.load(std::memory_order_relaxed);
        if (fadeRemaining == 0)
        {
            std::forward<Processor>(processor)(current, channels, count, processSamples);
            return;
        }

        std::array<float*, maximumChannels> oldPointers {};
        std::array<float*, maximumChannels> newPointers {};
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            oldPointers[channel] = oldBuffer.data() + channel * spec.maximumBlockSize;
            newPointers[channel] = newBuffer.data() + channel * spec.maximumBlockSize;
            std::copy_n(channels[channel], processSamples, oldPointers[channel]);
            std::copy_n(channels[channel], processSamples, newPointers[channel]);
        }

        processor(current, oldPointers.data(), count, processSamples);
        processor(targetMode, newPointers.data(), count, processSamples);
        for (std::size_t sample = 0; sample < processSamples; ++sample)
        {
            const auto progress = static_cast<float>(fadeLength - fadeRemaining + 1)
                                / static_cast<float>(fadeLength);
            for (std::size_t channel = 0; channel < count; ++channel)
                channels[channel][sample] = oldPointers[channel][sample] * (1.0f - progress)
                                          + newPointers[channel][sample] * progress;
            --fadeRemaining;
            if (fadeRemaining == 0)
            {
                activeMode.store(targetMode, std::memory_order_release);
                for (std::size_t tail = sample + 1; tail < processSamples; ++tail)
                    for (std::size_t channel = 0; channel < count; ++channel)
                        channels[channel][tail] = newPointers[channel][tail];
                break;
            }
        }
    }

private:
    static constexpr auto noRequest = std::numeric_limits<std::size_t>::max();
    ProcessSpec spec;
    std::size_t modeCount { 1 };
    std::atomic<std::size_t> activeMode {};
    std::atomic<std::size_t> requestedMode { noRequest };
    std::atomic<std::size_t> requestedFadeLength { 1 };
    std::size_t targetMode {};
    std::size_t fadeLength { 1 };
    std::size_t fadeRemaining {};
    std::vector<float> oldBuffer;
    std::vector<float> newBuffer;
};
} // namespace nts::dsp
