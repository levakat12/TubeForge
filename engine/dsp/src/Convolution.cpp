#include "nts/dsp/Convolution.h"

#include "nts/dsp/Simd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace nts::dsp
{
ImpulseResponse prepareImpulseResponse(const ImpulseResponse& decoded, double targetSampleRate,
                                       const ImpulsePreparationOptions& options)
{
    ImpulseResponse result;
    result.sampleRate = std::max(1.0, targetSampleRate);
    if (decoded.channels.empty() || decoded.channels.front().empty()) return result;
    const auto targetChannels = std::clamp(options.outputChannels, std::size_t { 1 }, maximumChannels);
    const auto ratio = result.sampleRate / std::max(1.0, decoded.sampleRate);
    const auto sourceLength = decoded.channels.front().size();
    const auto destinationLength = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(sourceLength * ratio)));
    result.channels.assign(targetChannels, std::vector<float>(destinationLength));
    for (std::size_t channel = 0; channel < targetChannels; ++channel)
    {
        const auto& source = decoded.channels[std::min(channel, decoded.channels.size() - 1)];
        for (std::size_t index = 0; index < destinationLength; ++index)
        {
            const auto sourcePosition = static_cast<double>(index) / ratio;
            const auto first = std::min(static_cast<std::size_t>(sourcePosition), source.size() - 1);
            const auto second = std::min(first + 1, source.size() - 1);
            const auto fraction = static_cast<float>(sourcePosition - static_cast<double>(first));
            result.channels[channel][index] = source[first] + fraction * (source[second] - source[first]);
        }
    }
    if (options.removeDc)
        for (auto& channel : result.channels)
        {
            const auto mean = std::accumulate(channel.begin(), channel.end(), 0.0) / channel.size();
            for (auto& sample : channel) sample -= static_cast<float>(mean);
        }
    const auto threshold = dbToLinear(options.trimThresholdDb);
    std::size_t trimStart {};
    while (trimStart + 1 < destinationLength)
    {
        bool audible {};
        for (const auto& channel : result.channels)
            audible = audible || std::abs(channel[trimStart]) >= threshold;
        if (audible) break;
        ++trimStart;
    }
    if (trimStart > 0)
        for (auto& channel : result.channels)
            channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(trimStart));
    if (options.normalizePeak)
    {
        float peak {};
        for (const auto& channel : result.channels)
            for (const auto sample : channel) peak = std::max(peak, std::abs(sample));
        if (peak > 0.0f)
        {
            const auto scale = dbToLinear(options.normalizationDb) / peak;
            for (auto& channel : result.channels)
                for (auto& sample : channel) sample *= scale;
        }
    }
    return result;
}

void DirectConvolver::prepare(std::size_t maximumImpulseLength, std::size_t channels)
{
    maximumLength = std::max<std::size_t>(1, maximumImpulseLength);
    configuredChannels = std::clamp(channels, std::size_t { 1 }, maximumChannels);
    impulse.assign(maximumLength * configuredChannels, 0.0f);
    history.assign(2 * maximumLength * configuredChannels, 0.0f);
    reset();
}
void DirectConvolver::reset() noexcept { std::fill(history.begin(), history.end(), 0.0f); writePosition = 0; }
bool DirectConvolver::loadImpulse(std::span<const float> left, std::span<const float> right)
{
    if (left.empty() || left.size() > maximumLength || (! right.empty() && right.size() > maximumLength)) return false;
    activeLength = std::max(left.size(), right.size());
    std::fill(impulse.begin(), impulse.end(), 0.0f);
    for (std::size_t channel = 0; channel < configuredChannels; ++channel)
    {
        const auto source = channel == 0 || right.empty() ? left : right;
        // Reversed, and right-aligned inside the active window. The alignment matters when the
        // two sides differ in length: the shorter one's taps are the *earliest* of the active
        // window, so its zero padding has to land at the front of the reversed layout, not the
        // back, or the response is shifted in time against the other channel.
        auto* destination = impulse.data() + channel * maximumLength
                          + (activeLength - source.size());
        std::reverse_copy(source.begin(), source.end(), destination);
    }
    reset(); return true;
}
void DirectConvolver::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, configuredChannels);
    if (activeLength == 0) return;
    // Channel outer: the dot product below streams two contiguous spans, and interleaving the
    // channels would evict one channel's working set with the other's on every sample.
    const auto startPosition = writePosition;
    auto position = startPosition;
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        auto* const channelHistory = history.data() + channel * 2 * maximumLength;
        // The reversed active window sits at the front of each channel region, so the taps run
        // from offset zero regardless of how much of the region the response actually fills.
        const auto* const channelImpulse = impulse.data() + channel * maximumLength;
        position = startPosition;
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto value = channels[channel][sample];
            channelHistory[position] = value;
            channelHistory[position + maximumLength] = value;
            // The window ends on the sample just written and runs backwards activeLength taps,
            // which in the doubled buffer is always a valid forward span: its lowest index is
            // maximumLength - activeLength + 1 at worst, and its highest 2 * maximumLength - 1.
            const auto* const window = channelHistory + position + maximumLength - activeLength + 1;
            channels[channel][sample] = dotProduct(channelImpulse, window, activeLength);
            if (++position == maximumLength) position = 0;
        }
    }
    writePosition = position;
}

void PartitionedConvolver::prepare(std::size_t newBlockSize, std::size_t maximumImpulseLength,
                                   std::size_t channels)
{
    blockSize = std::max<std::size_t>(1, newBlockSize);
    fftSize = nextPowerOfTwo(blockSize * 2);
    maximumPartitions = std::max<std::size_t>(1, (maximumImpulseLength + blockSize - 1) / blockSize);
    configuredChannels = std::clamp(channels, std::size_t { 1 }, maximumChannels);
    fft.prepare(fftSize);
    filters.assign(configuredChannels * maximumPartitions * fftSize, {});
    inputSpectra.assign(configuredChannels * maximumPartitions * fftSize, {});
    overlap.assign(configuredChannels * blockSize, 0.0f);
    work.assign(fftSize, {}); accumulator.assign(fftSize, {});
    reset();
}
void PartitionedConvolver::reset() noexcept
{
    std::fill(inputSpectra.begin(), inputSpectra.end(), std::complex<float> {});
    std::fill(overlap.begin(), overlap.end(), 0.0f); spectrumPosition = 0;
}
bool PartitionedConvolver::loadImpulse(std::span<const float> left, std::span<const float> right)
{
    const auto maximumLength = maximumPartitions * blockSize;
    if (left.empty() || left.size() > maximumLength || (! right.empty() && right.size() > maximumLength)) return false;
    activePartitions = (std::max(left.size(), right.size()) + blockSize - 1) / blockSize;
    std::fill(filters.begin(), filters.end(), std::complex<float> {});
    for (std::size_t channel = 0; channel < configuredChannels; ++channel)
    {
        const auto source = channel == 0 || right.empty() ? left : right;
        for (std::size_t partition = 0; partition < activePartitions; ++partition)
        {
            std::fill(work.begin(), work.end(), std::complex<float> {});
            const auto start = partition * blockSize;
            const auto count = start < source.size() ? std::min(blockSize, source.size() - start) : 0;
            for (std::size_t index = 0; index < count; ++index) work[index] = { source[start + index], 0.0f };
            fft.transform(work);
            auto* destination = filters.data() + (channel * maximumPartitions + partition) * fftSize;
            std::copy(work.begin(), work.end(), destination);
        }
    }
    reset(); return true;
}
void PartitionedConvolver::process(float* const* channels, std::size_t channelCount,
                                   std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, configuredChannels);
    if (activePartitions == 0 || samples == 0) return;
    // Refused rather than truncated. Processing a short block would advance the spectrum
    // history and overlap by a whole block anyway, leaving this instance permanently out of
    // step with its input -- silently, and audibly. See the class comment.
    if (samples != blockSize) return;
    const auto processSamples = blockSize;
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        std::fill(work.begin(), work.end(), std::complex<float> {});
        for (std::size_t index = 0; index < processSamples; ++index) work[index] = { channels[channel][index], 0.0f };
        fft.transform(work);
        auto* spectrum = inputSpectra.data() + (channel * maximumPartitions + spectrumPosition) * fftSize;
        std::copy(work.begin(), work.end(), spectrum);
        // Both operands are transforms of real signals, so both are conjugate-symmetric, and so
        // is their product: P[N-k] = conj(P[k]). Only the lower half plus DC and Nyquist carry
        // information; the upper half is filled by reflection afterwards rather than computed,
        // which halves the multiply-accumulate that dominates this loop.
        const auto half = fftSize / 2;
        std::fill(accumulator.begin(), accumulator.end(), std::complex<float> {});
        for (std::size_t partition = 0; partition < activePartitions; ++partition)
        {
            const auto inputPosition = (spectrumPosition + maximumPartitions - partition) % maximumPartitions;
            const auto* inputSpectrum = inputSpectra.data() + (channel * maximumPartitions + inputPosition) * fftSize;
            const auto* filterSpectrum = filters.data() + (channel * maximumPartitions + partition) * fftSize;
            // Written out rather than using operator*: the complex multiply is four products and
            // two sums, and spelling it means the compiler is free to keep the parts in registers
            // across the accumulate instead of round-tripping a temporary.
            for (std::size_t bin = 0; bin <= half; ++bin)
            {
                const auto inputReal = inputSpectrum[bin].real(), inputImaginary = inputSpectrum[bin].imag();
                const auto filterReal = filterSpectrum[bin].real(), filterImaginary = filterSpectrum[bin].imag();
                accumulator[bin] += std::complex<float> {
                    inputReal * filterReal - inputImaginary * filterImaginary,
                    inputReal * filterImaginary + inputImaginary * filterReal };
            }
        }
        for (std::size_t bin = 1; bin < half; ++bin)
            accumulator[fftSize - bin] = std::conj(accumulator[bin]);
        fft.transform(accumulator, true);
        auto* channelOverlap = overlap.data() + channel * blockSize;
        for (std::size_t index = 0; index < processSamples; ++index)
            channels[channel][index] = accumulator[index].real() + channelOverlap[index];
        for (std::size_t index = 0; index < blockSize; ++index)
            channelOverlap[index] = accumulator[index + blockSize].real();
    }
    spectrumPosition = (spectrumPosition + 1) % maximumPartitions;
}

void CrossfadingDirectConvolver::prepare(std::size_t maximumImpulseLength, std::size_t channels,
                                         std::size_t blockSize)
{
    maximumBlockSize = std::max<std::size_t>(1, blockSize);
    configuredChannels = std::clamp(channels, std::size_t { 1 }, maximumChannels);
    convolvers[0].prepare(maximumImpulseLength, configuredChannels);
    convolvers[1].prepare(maximumImpulseLength, configuredChannels);
    oldBuffer.assign(maximumBlockSize * configuredChannels, 0.0f);
    newBuffer.assign(maximumBlockSize * configuredChannels, 0.0f);
    reset();
}
void CrossfadingDirectConvolver::reset() noexcept
{
    // Clears history, not the loaded response -- matching DirectConvolver::reset, which a
    // caller reasonably expects to silence the tail without forgetting the cabinet. Resetting
    // the active index here instead would discard a response staged just before the reset,
    // leaving nothing loaded and the convolution silently passing its input through.
    //
    // Any swap that was staged or part-way through completes immediately: history is being
    // cleared regardless, so there is nothing left to fade from.
    if (swapRequested.exchange(false, std::memory_order_acq_rel) || crossfadeRemaining != 0)
        activeIndex.store(1 - activeIndex.load(std::memory_order_relaxed), std::memory_order_release);
    convolvers[0].reset(); convolvers[1].reset();
    crossfadeActive.store(false, std::memory_order_release);
    crossfadeRemaining = 0;
}
bool CrossfadingDirectConvolver::loadInactiveImpulse(std::span<const float> left,
                                                     std::span<const float> right)
{
    if (crossfadeActive.load(std::memory_order_acquire)
        || swapRequested.load(std::memory_order_acquire)) return false;
    return convolvers[1 - activeIndex.load(std::memory_order_relaxed)].loadImpulse(left, right);
}
void CrossfadingDirectConvolver::requestSwap(std::size_t crossfadeSamples) noexcept
{
    fadeLength = std::max<std::size_t>(1, crossfadeSamples);
    swapRequested.store(true, std::memory_order_release);
}
void CrossfadingDirectConvolver::process(float* const* channels, std::size_t channelCount,
                                         std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, configuredChannels);
    const auto processSamples = std::min(samples, maximumBlockSize);
    if (swapRequested.exchange(false, std::memory_order_acq_rel))
    {
        crossfadeRemaining = fadeLength;
        crossfadeActive.store(true, std::memory_order_release);
    }
    const auto current = activeIndex.load(std::memory_order_relaxed);
    if (crossfadeRemaining == 0)
    {
        convolvers[current].process(channels, count, processSamples);
        return;
    }
    std::array<float*, maximumChannels> oldPointers {};
    std::array<float*, maximumChannels> newPointers {};
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        oldPointers[channel] = oldBuffer.data() + channel * maximumBlockSize;
        newPointers[channel] = newBuffer.data() + channel * maximumBlockSize;
        std::copy_n(channels[channel], processSamples, oldPointers[channel]);
        std::copy_n(channels[channel], processSamples, newPointers[channel]);
    }
    // Both sides must be driven with the same input every block, crossfade or not, so the
    // incoming response's history is already filled when it becomes the only one sounding.
    convolvers[current].process(oldPointers.data(), count, processSamples);
    convolvers[1 - current].process(newPointers.data(), count, processSamples);
    for (std::size_t sample = 0; sample < processSamples; ++sample)
    {
        const auto progress = 1.0f - static_cast<float>(crossfadeRemaining) / static_cast<float>(fadeLength);
        for (std::size_t channel = 0; channel < count; ++channel)
            channels[channel][sample] = oldPointers[channel][sample] * (1.0f - progress)
                                      + newPointers[channel][sample] * progress;
        if (crossfadeRemaining > 0) --crossfadeRemaining;
    }
    if (crossfadeRemaining == 0)
    {
        activeIndex.store(1 - current, std::memory_order_release);
        crossfadeActive.store(false, std::memory_order_release);
    }
}

void CrossfadingConvolver::prepare(std::size_t blockSize, std::size_t maximumImpulseLength,
                                   std::size_t channels)
{
    maximumBlockSize = blockSize;
    configuredChannels = std::clamp(channels, std::size_t { 1 }, maximumChannels);
    convolvers[0].prepare(blockSize, maximumImpulseLength, configuredChannels);
    convolvers[1].prepare(blockSize, maximumImpulseLength, configuredChannels);
    oldBuffer.assign(maximumBlockSize * configuredChannels, 0.0f);
    newBuffer.assign(maximumBlockSize * configuredChannels, 0.0f);
    reset();
}
bool CrossfadingConvolver::loadInactiveImpulse(std::span<const float> left, std::span<const float> right)
{
    if (crossfadeActive.load(std::memory_order_acquire)
        || swapRequested.load(std::memory_order_acquire)) return false;
    return convolvers[1 - activeIndex.load(std::memory_order_relaxed)].loadImpulse(left, right);
}
void CrossfadingConvolver::requestSwap(std::size_t crossfadeSamples) noexcept
{
    fadeLength = std::max<std::size_t>(1, crossfadeSamples);
    swapRequested.store(true, std::memory_order_release);
}
void CrossfadingConvolver::reset() noexcept
{
    convolvers[0].reset(); convolvers[1].reset(); activeIndex.store(0);
    swapRequested.store(false); crossfadeActive.store(false); crossfadeRemaining = 0;
}
void CrossfadingConvolver::process(float* const* channels, std::size_t channelCount,
                                   std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, configuredChannels);
    const auto processSamples = std::min(samples, maximumBlockSize);
    if (swapRequested.exchange(false, std::memory_order_acq_rel))
    {
        crossfadeRemaining = fadeLength;
        crossfadeActive.store(true, std::memory_order_release);
    }
    const auto current = activeIndex.load(std::memory_order_relaxed);
    if (crossfadeRemaining == 0)
    {
        convolvers[current].process(channels, count, processSamples);
        return;
    }
    std::array<float*, maximumChannels> oldPointers {};
    std::array<float*, maximumChannels> newPointers {};
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        oldPointers[channel] = oldBuffer.data() + channel * maximumBlockSize;
        newPointers[channel] = newBuffer.data() + channel * maximumBlockSize;
        std::copy_n(channels[channel], processSamples, oldPointers[channel]);
        std::copy_n(channels[channel], processSamples, newPointers[channel]);
    }
    convolvers[current].process(oldPointers.data(), count, processSamples);
    convolvers[1 - current].process(newPointers.data(), count, processSamples);
    for (std::size_t sample = 0; sample < processSamples; ++sample)
    {
        const auto progress = 1.0f - static_cast<float>(crossfadeRemaining) / static_cast<float>(fadeLength);
        for (std::size_t channel = 0; channel < count; ++channel)
            channels[channel][sample] = oldPointers[channel][sample] * (1.0f - progress)
                                      + newPointers[channel][sample] * progress;
        if (crossfadeRemaining > 0) --crossfadeRemaining;
    }
    if (crossfadeRemaining == 0)
    {
        activeIndex.store(1 - current, std::memory_order_release);
        crossfadeActive.store(false, std::memory_order_release);
    }
}
void BufferedCrossfadingConvolver::prepare(std::size_t partitionSize, std::size_t maximumImpulseLength,
                                           std::size_t channels)
{
    partition = std::max<std::size_t>(1, partitionSize);
    configuredChannels = std::clamp(channels, std::size_t { 1 }, maximumChannels);
    convolver.prepare(partition, maximumImpulseLength, configuredChannels);
    inputBuffer.assign(partition * configuredChannels, 0.0f);
    outputBuffer.assign(partition * configuredChannels, 0.0f);
    reset();
}

void BufferedCrossfadingConvolver::reset() noexcept
{
    convolver.reset();
    std::fill(inputBuffer.begin(), inputBuffer.end(), 0.0f);
    std::fill(outputBuffer.begin(), outputBuffer.end(), 0.0f);
    fill = 0;
}

bool BufferedCrossfadingConvolver::loadInactiveImpulse(std::span<const float> left,
                                                       std::span<const float> right)
{
    if (! convolver.loadInactiveImpulse(left, right)) return false;
    activeLength = std::max(left.size(), right.size());
    return true;
}

void BufferedCrossfadingConvolver::requestSwap(std::size_t crossfadeSamples) noexcept
{
    convolver.requestSwap(crossfadeSamples);
}

void BufferedCrossfadingConvolver::process(float* const* channels, std::size_t channelCount,
                                           std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, configuredChannels);
    if (count == 0 || partition == 0) return;

    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            auto* const input = inputBuffer.data() + channel * partition;
            auto* const output = outputBuffer.data() + channel * partition;
            // Read before write, and at the same index: the slot holds the result produced a
            // whole partition ago, which is exactly the latency this class reports.
            const auto processed = output[fill];
            input[fill] = channels[channel][sample];
            channels[channel][sample] = processed;
        }
        if (++fill < partition) continue;

        // A full partition. Convolve it in place and hand the result to the output side; the
        // wrapped convolver is only ever called with the block size it was prepared for, which
        // is the contract that makes it usable at all.
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            auto* const input = inputBuffer.data() + channel * partition;
            auto* const output = outputBuffer.data() + channel * partition;
            std::copy_n(input, partition, output);
            partitionPointers[channel] = output;
        }
        convolver.process(partitionPointers.data(), count, partition);
        fill = 0;
    }
}

} // namespace nts::dsp
