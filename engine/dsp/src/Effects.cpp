#include "nts/dsp/Effects.h"

#include <algorithm>
#include <cmath>

namespace nts::dsp
{
namespace
{
/// Comb lengths in samples at 44.1 kHz, from Schroeder's original mutually-prime set. Scaled
/// to the running rate in prepare. Mutual primeness is what stops the echoes lining up into
/// an audible pitch.
constexpr std::array<std::size_t, 4> combSeeds { 1557, 1617, 1491, 1422 };
constexpr std::array<std::size_t, 2> allPassSeeds { 225, 556 };
/// Offsets the right channel's lines so the two sides decorrelate into a stereo image.
constexpr std::size_t stereoOffset = 23;
} // namespace

void Delay::prepare(const ProcessSpec& newSpec, float maximumTimeMs)
{
    spec = newSpec;
    spec.channels = std::clamp(spec.channels, std::size_t { 1 }, maximumChannels);
    capacity = static_cast<std::size_t>(spec.sampleRate * maximumTimeMs * 0.001) + 4;
    buffer.assign(capacity * maximumChannels, 0.0f);
    for (auto& smoother : delaySamples)
    {
        // Slow: a delay time that races to its target sweeps the pitch of everything already
        // in the line. Twelfth of a second is quick enough to feel responsive without warbling.
        smoother.prepare(spec.sampleRate, 80.0);
        smoother.reset(static_cast<float>(spec.sampleRate * parameters.timeMs * 0.001));
    }
    reset();
}

void Delay::reset() noexcept
{
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    writePosition.fill(0);
    damped.fill(0.0f);
}

void Delay::setParameters(const DelayParameters& newParameters) noexcept
{
    parameters = newParameters;
    parameters.feedback = std::clamp(parameters.feedback, 0.0f, 0.95f);
    parameters.mix = std::clamp(parameters.mix, 0.0f, 1.0f);
    parameters.stereoSpread = std::clamp(parameters.stereoSpread, -0.5f, 0.5f);

    const auto maximumSamples = static_cast<float>(capacity > 4 ? capacity - 4 : 1);
    for (std::size_t channel = 0; channel < maximumChannels; ++channel)
    {
        const auto spread = channel == 1 ? 1.0f + parameters.stereoSpread : 1.0f;
        const auto target = static_cast<float>(spec.sampleRate * parameters.timeMs * 0.001) * spread;
        delaySamples[channel].setTarget(std::clamp(target, 1.0f, maximumSamples));
    }

    const auto cutoff = clampFrequency(parameters.dampingHz, spec.sampleRate);
    dampingCoefficient = static_cast<float>(
        std::exp(-2.0 * 3.14159265358979323846 * cutoff / spec.sampleRate));
}

void Delay::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    if (capacity == 0 || buffer.empty()) return;
    const auto count = std::min(channelCount, spec.channels);

    for (std::size_t channel = 0; channel < count; ++channel)
    {
        auto* line = buffer.data() + channel * capacity;
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto distance = delaySamples[channel].next();
            const auto whole = static_cast<std::size_t>(distance);
            const auto fraction = distance - static_cast<float>(whole);

            // Read fractionally: a smoothed delay time lands between samples, and rounding it
            // would step the pitch as the control moves.
            const auto first = (writePosition[channel] + capacity - whole) % capacity;
            const auto second = (first + capacity - 1) % capacity;
            const auto delayed = line[first] * (1.0f - fraction) + line[second] * fraction;

            damped[channel] = delayed + dampingCoefficient * (damped[channel] - delayed);
            line[writePosition[channel]] = suppressDenormal(
                channels[channel][sample] + damped[channel] * parameters.feedback);
            writePosition[channel] = (writePosition[channel] + 1) % capacity;

            // A send rather than a crossfade: the dry signal stays at unity so adding delay
            // never quietens the amplifier.
            channels[channel][sample] += delayed * parameters.mix;
        }
    }
}

std::size_t Delay::tailSamples() const noexcept
{
    if (parameters.mix <= 0.0f) return 0;
    const auto oneDelay = spec.sampleRate * parameters.timeMs * 0.001;
    if (parameters.feedback <= 0.0f) return static_cast<std::size_t>(oneDelay);
    // Repeats to reach -60 dB, which is where the tail stops mattering.
    const auto repeats = std::log(0.001) / std::log(std::max(1.0e-6f, parameters.feedback));
    return static_cast<std::size_t>(oneDelay * std::clamp(repeats, 1.0, 64.0));
}

void Reverb::prepare(const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.channels = std::clamp(spec.channels, std::size_t { 1 }, maximumChannels);
    const auto scale = spec.sampleRate / 44100.0;

    for (std::size_t channel = 0; channel < maximumChannels; ++channel)
    {
        for (std::size_t index = 0; index < combCount; ++index)
        {
            const auto length = static_cast<std::size_t>(
                static_cast<double>(combSeeds[index] + (channel == 1 ? stereoOffset : 0)) * scale);
            combs[channel][index].buffer.assign(std::max<std::size_t>(2, length), 0.0f);
        }
        for (std::size_t index = 0; index < allPassCount; ++index)
        {
            const auto length = static_cast<std::size_t>(
                static_cast<double>(allPassSeeds[index] + (channel == 1 ? stereoOffset : 0)) * scale);
            allPasses[channel][index].buffer.assign(std::max<std::size_t>(2, length), 0.0f);
        }
    }
    lowCut.prepare(spec);
    wetBuffer.assign(spec.maximumBlockSize * maximumChannels, 0.0f);
    setParameters(parameters);
    reset();
}

void Reverb::reset() noexcept
{
    for (auto& channel : combs)
        for (auto& line : channel)
        {
            std::fill(line.buffer.begin(), line.buffer.end(), 0.0f);
            line.position = 0; line.store = 0.0f;
        }
    for (auto& channel : allPasses)
        for (auto& line : channel)
        {
            std::fill(line.buffer.begin(), line.buffer.end(), 0.0f);
            line.position = 0; line.store = 0.0f;
        }
    lowCut.reset();
}

void Reverb::setParameters(const ReverbParameters& newParameters) noexcept
{
    parameters = newParameters;
    parameters.size = std::clamp(parameters.size, 0.0f, 1.0f);
    parameters.damping = std::clamp(parameters.damping, 0.0f, 1.0f);
    parameters.mix = std::clamp(parameters.mix, 0.0f, 1.0f);

    // Held below unity at the top of the range: a comb at exactly one is an oscillator.
    feedback = 0.70f + 0.28f * parameters.size;
    damping = 0.20f + 0.60f * parameters.damping;

    lowCut.setCoefficients(BiquadCoefficients::make(
        FilterType::highPass, spec.sampleRate,
        clampFrequency(parameters.lowCutHz, spec.sampleRate), 0.707));
}

void Reverb::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    // Guarded rather than assumed: a process call before prepare would otherwise index empty
    // comb buffers and read through a null pointer, which is exactly what happened once.
    if (parameters.mix <= 0.0f || wetBuffer.empty() || combs[0][0].buffer.empty()) return;
    const auto count = std::min(channelCount, spec.channels);
    const auto processSamples = std::min(samples, spec.maximumBlockSize);

    std::array<float*, maximumChannels> wet {};
    for (std::size_t channel = 0; channel < count; ++channel)
        wet[channel] = wetBuffer.data() + channel * spec.maximumBlockSize;

    for (std::size_t channel = 0; channel < count; ++channel)
    {
        for (std::size_t sample = 0; sample < processSamples; ++sample)
        {
            const auto input = channels[channel][sample] * 0.25f;
            auto accumulated = 0.0f;

            for (auto& comb : combs[channel])
            {
                const auto delayed = comb.buffer[comb.position];
                // One-pole inside the loop: each pass round the comb loses a little more top
                // end, which is what makes a tail sound like a room rather than a metal pipe.
                comb.store = delayed * (1.0f - damping) + comb.store * damping;
                comb.buffer[comb.position] = suppressDenormal(input + comb.store * feedback);
                comb.position = (comb.position + 1) % comb.buffer.size();
                accumulated += delayed;
            }

            for (auto& allPass : allPasses[channel])
            {
                const auto delayed = allPass.buffer[allPass.position];
                const auto output = delayed - accumulated;
                allPass.buffer[allPass.position] = suppressDenormal(accumulated + delayed * 0.5f);
                allPass.position = (allPass.position + 1) % allPass.buffer.size();
                accumulated = output;
            }

            wet[channel][sample] = accumulated;
        }
    }

    // Filter the tail only, then send it in. The dry path is never touched.
    lowCut.process(wet.data(), count, processSamples);
    for (std::size_t channel = 0; channel < count; ++channel)
        for (std::size_t sample = 0; sample < processSamples; ++sample)
            channels[channel][sample] += wet[channel][sample] * parameters.mix;
}

std::size_t Reverb::tailSamples() const noexcept
{
    if (parameters.mix <= 0.0f) return 0;
    // Longest comb decaying to -60 dB.
    const auto longest = static_cast<double>(combs[0][1].buffer.size());
    const auto repeats = std::log(0.001) / std::log(std::max(1.0e-6f, feedback));
    return static_cast<std::size_t>(longest * std::clamp(repeats, 1.0, 256.0));
}
} // namespace nts::dsp
