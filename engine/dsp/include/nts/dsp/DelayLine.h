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

/** A short delay whose length moves under the signal, read with cubic interpolation.

    A separate class from `DelayLine` above rather than an option on it, because the two want
    opposite things. That one delays by whole samples and steps when the length changes, which
    is right for latency alignment and wrong for anything that sweeps: a chorus stepping between
    integer delays produces a click on every sample the length crosses a boundary.

    Four-point Hermite rather than linear. Linear interpolation on a swept delay is a low-pass
    whose cutoff moves with the sweep, so a chorus audibly dulls at one end of its travel and
    brightens at the other -- the artefact is the modulation, which is exactly where it is most
    obvious. Hermite costs three more multiplies and removes it.

    Capacity is claimed in `prepare`, so `processSample` allocates nothing.
*/
class ModulatedDelayLine
{
public:
    void prepare(std::size_t maximumDelaySamples, std::size_t channelCount)
    {
        // Three samples of margin: the interpolator reads one before and two after the
        // fractional position, so the deepest legal delay still has neighbours to read.
        capacity = maximumDelaySamples + 4;
        channels = std::clamp(channelCount, std::size_t { 1 }, maximumChannels);
        buffer.assign(capacity * channels, 0.0f);
        writePosition.fill(0);
        reset();
    }

    void reset() noexcept
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writePosition.fill(0);
    }

    [[nodiscard]] std::size_t capacitySamples() const noexcept
    {
        return capacity < 4 ? 0 : capacity - 4;
    }

    /** Writes one sample. Call before any `readAt` for that channel and sample.

        Split from reading because a pitch shifter needs several taps into one history: it
        writes once and reads two or four times at different delays. Rolling them together
        would mean either writing the same sample repeatedly or keeping a second copy of the
        line per tap.
    */
    void write(float input, std::size_t channel) noexcept
    {
        if (capacity == 0 || channel >= channels) return;
        buffer[channel * capacity + writePosition[channel]] = input;
    }

    /// Reads `delaySamples` back from the most recent write, interpolated. Const: any number of
    /// taps may read the same history in any order.
    [[nodiscard]] float readAt(float delaySamples, std::size_t channel) const noexcept
    {
        if (capacity == 0 || channel >= channels) return 0.0f;
        const auto* ring = buffer.data() + channel * capacity;
        const auto write = writePosition[channel];

        const auto wanted = std::clamp(delaySamples, 1.0f, static_cast<float>(capacitySamples()));
        const auto whole = static_cast<std::size_t>(wanted);
        const auto fraction = wanted - static_cast<float>(whole);

        // Read positions counted back from the write head, wrapped by hand rather than with %
        // so the arithmetic stays in std::size_t and cannot go negative.
        const auto at = [&](std::size_t back)
        {
            return ring[(write + capacity - back) % capacity];
        };
        const auto x0 = at(whole == 0 ? 0 : whole - 1);
        const auto x1 = at(whole);
        const auto x2 = at(whole + 1);
        const auto x3 = at(whole + 2);

        // Four-point, third-order Hermite.
        const auto c0 = x1;
        const auto c1 = 0.5f * (x2 - x0);
        const auto c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        const auto c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
        return ((c3 * fraction + c2) * fraction + c1) * fraction + c0;
    }

    /// Moves the write head on. Call once per sample per channel, after every `readAt`.
    void advance(std::size_t channel) noexcept
    {
        if (capacity == 0 || channel >= channels) return;
        writePosition[channel] = (writePosition[channel] + 1) % capacity;
    }

    /// Write, read one tap, advance. The single-tap case, which is most callers.
    [[nodiscard]] float processSample(float input, float delaySamples, std::size_t channel) noexcept
    {
        if (capacity == 0 || channel >= channels) return input;
        write(input, channel);
        const auto output = readAt(delaySamples, channel);
        advance(channel);
        return output;
    }

private:
    std::vector<float> buffer;
    std::size_t capacity {};
    std::size_t channels { maximumChannels };
    std::array<std::size_t, maximumChannels> writePosition {};
};
} // namespace nts::dsp
