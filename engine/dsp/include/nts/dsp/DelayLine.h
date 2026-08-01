#pragma once

#include "Common.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace nts::dsp
{
/** Fixed-capacity integer-sample delay, one ring per channel.

    Written for latency alignment rather than for modulation: the delay is whole samples
    with no interpolation, and changing it steps rather than glides. That is the right
    trade for its job -- matching a dry path to the processing latency the host has been
    told about -- because the delay only changes when that reported latency changes, which
    is already a discontinuity the host handles.

    Capacity is claimed in prepare, so process allocates nothing and is safe on the audio
    thread.
*/
class DelayLine
{
public:
    void prepare(std::size_t maximumDelaySamples, std::size_t channelCount)
    {
        capacity = maximumDelaySamples + 1;
        channels = std::clamp(channelCount, std::size_t { 1 }, maximumChannels);
        buffer.assign(capacity * channels, 0.0f);
        delaySamples = std::min(delaySamples, maximumDelaySamples);
        reset();
    }

    void reset() noexcept
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writePosition = 0;
    }

    /** Clamped to the prepared capacity, so an unexpectedly large latency cannot read
        outside the ring. It silently shortens instead, which misaligns the dry path but
        keeps the process function in bounds.
    */
    void setDelay(std::size_t samples) noexcept
    {
        delaySamples = capacity == 0 ? 0 : std::min(samples, capacity - 1);
    }

    [[nodiscard]] std::size_t delay() const noexcept { return delaySamples; }

    /** Pushes a block in and reads the delayed block out. Input and output may alias. */
    void process(const float* const* input, float* const* output,
                 std::size_t channelCount, std::size_t samples) noexcept
    {
        if (capacity == 0) return;
        const auto count = std::min(channelCount, channels);

        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto write = (writePosition + sample) % capacity;
            const auto read = (write + capacity - delaySamples) % capacity;
            for (std::size_t channel = 0; channel < count; ++channel)
            {
                auto* ring = buffer.data() + channel * capacity;
                // Read after write, so a delay of zero returns the incoming sample and the
                // aliasing case stays correct.
                ring[write] = input[channel][sample];
                output[channel][sample] = ring[read];
            }
        }
        writePosition = (writePosition + samples) % capacity;
    }

private:
    std::vector<float> buffer;
    std::size_t capacity {};
    std::size_t channels { maximumChannels };
    std::size_t delaySamples {};
    std::size_t writePosition {};
};
} // namespace nts::dsp
