#include "nts/dsp/Metering.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nts::dsp
{
namespace
{
float energyToLufs(double energy) noexcept
{
    return energy > 1.0e-12 ? static_cast<float>(-0.691 + 10.0 * std::log10(energy)) : -120.0f;
}
}

BiquadCoefficients makeKWeightShelf(double sampleRate) noexcept
{
    constexpr auto frequency = 1681.974450955533;
    constexpr auto gainDb = 3.999843853973347;
    constexpr auto q = 0.7071752369554196;
    const auto k = std::tan(std::numbers::pi * frequency / sampleRate);
    const auto vh = std::pow(10.0, gainDb / 20.0);
    const auto vb = std::pow(vh, 0.4996667741545416);
    const auto denominator = 1.0 + k / q + k * k;
    return { (vh + vb * k / q + k * k) / denominator,
             2.0 * (k * k - vh) / denominator,
             (vh - vb * k / q + k * k) / denominator,
             2.0 * (k * k - 1.0) / denominator,
             (1.0 - k / q + k * k) / denominator };
}

BiquadCoefficients makeKWeightHighPass(double sampleRate) noexcept
{
    constexpr auto frequency = 38.13547087602444;
    constexpr auto q = 0.5003270373238773;
    const auto k = std::tan(std::numbers::pi * frequency / sampleRate);
    const auto denominator = 1.0 + k / q + k * k;
    return { 1.0 / denominator, -2.0 / denominator, 1.0 / denominator,
             2.0 * (k * k - 1.0) / denominator,
             (1.0 - k / q + k * k) / denominator };
}

void MeterBank::prepare(const ProcessSpec& spec)
{
    sampleRate = std::max(1.0, spec.sampleRate);
    kWeightShelf = makeKWeightShelf(sampleRate);
    kWeightHighPass = makeKWeightHighPass(sampleRate);
    shortWindowSamples = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(sampleRate * 3.0)));
    blockWindowSamples = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(sampleRate * 0.4)));
    loudnessHopSamples = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(sampleRate * 0.1)));
    shortEnergyHistory.assign(shortWindowSamples * maximumChannels, 0.0);
    blockEnergyHistory.assign(blockWindowSamples * maximumChannels, 0.0);
    reset();
}
void MeterBank::reset() noexcept
{
    std::fill(shortEnergyHistory.begin(), shortEnergyHistory.end(), 0.0);
    std::fill(blockEnergyHistory.begin(), blockEnergyHistory.end(), 0.0);
    shortEnergySum.fill(0.0); blockEnergySum.fill(0.0);
    shortPosition.fill(0); blockPosition.fill(0); shortCount.fill(0); blockCount.fill(0); hopCounter.fill(0);
    for (std::size_t channel = 0; channel < maximumChannels; ++channel)
    {
        shelfState[channel].reset(); highPassState[channel].reset();
        loudnessBlockCount[channel].fill(0); loudnessEnergySum[channel].fill(0.0);
        peak[channel].store(0.0f); rms[channel].store(0.0f); crest[channel].store(0.0f);
        shortLoudness[channel].store(-120.0f); integratedLoudness[channel].store(-120.0f);
        reduction[channel].store(0.0f); inputClip[channel].store(false); outputClip[channel].store(false);
    }
}

float MeterBank::calculateIntegratedLufs(std::size_t channel) const noexcept
{
    double absoluteEnergy {};
    std::uint64_t absoluteCount {};
    for (std::size_t bin = 0; bin < loudnessHistogramBins; ++bin)
    {
        absoluteEnergy += loudnessEnergySum[channel][bin];
        absoluteCount += loudnessBlockCount[channel][bin];
    }
    if (absoluteCount == 0)
        return -120.0f;
    const auto relativeGate = energyToLufs(absoluteEnergy / static_cast<double>(absoluteCount)) - 10.0f;
    double gatedEnergy {};
    std::uint64_t gatedCount {};
    for (std::size_t bin = 0; bin < loudnessHistogramBins; ++bin)
    {
        const auto binLufs = -100.0f + static_cast<float>(bin) * 0.1f;
        if (binLufs >= relativeGate && binLufs >= -70.0f)
        {
            gatedEnergy += loudnessEnergySum[channel][bin];
            gatedCount += loudnessBlockCount[channel][bin];
        }
    }
    return gatedCount == 0 ? -120.0f
        : energyToLufs(gatedEnergy / static_cast<double>(gatedCount));
}

void MeterBank::process(const float* const* input, const float* const* output,
                        std::size_t channelCount, std::size_t samples, float gainReductionDb) noexcept
{
    const auto count = std::min(channelCount, maximumChannels);
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        float blockPeak {};
        double energy {};
        bool inClip = inputClip[channel].load(std::memory_order_relaxed);
        bool outClip = outputClip[channel].load(std::memory_order_relaxed);
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto in = input[channel][sample];
            const auto out = output[channel][sample];
            blockPeak = std::max(blockPeak, std::abs(out));
            energy += static_cast<double>(out) * out;
            inClip = inClip || std::abs(in) >= 1.0f;
            outClip = outClip || std::abs(out) >= 1.0f;
            const auto weighted = highPassState[channel].process(
                shelfState[channel].process(out, kWeightShelf), kWeightHighPass);
            const auto weightedEnergy = static_cast<double>(weighted) * weighted;
            auto* shortHistory = shortEnergyHistory.data() + channel * shortWindowSamples;
            auto* blockHistory = blockEnergyHistory.data() + channel * blockWindowSamples;
            shortEnergySum[channel] += weightedEnergy - shortHistory[shortPosition[channel]];
            blockEnergySum[channel] += weightedEnergy - blockHistory[blockPosition[channel]];
            shortHistory[shortPosition[channel]] = weightedEnergy;
            blockHistory[blockPosition[channel]] = weightedEnergy;
            shortPosition[channel] = (shortPosition[channel] + 1) % shortWindowSamples;
            blockPosition[channel] = (blockPosition[channel] + 1) % blockWindowSamples;
            shortCount[channel] = std::min(shortCount[channel] + 1, shortWindowSamples);
            blockCount[channel] = std::min(blockCount[channel] + 1, blockWindowSamples);
            if (++hopCounter[channel] >= loudnessHopSamples)
            {
                hopCounter[channel] = 0;
                if (blockCount[channel] == blockWindowSamples)
                {
                    const auto blockMean = blockEnergySum[channel] / static_cast<double>(blockWindowSamples);
                    const auto blockLufs = energyToLufs(blockMean);
                    if (blockLufs >= -70.0f)
                    {
                        const auto bin = static_cast<std::size_t>(std::clamp(
                            std::lround((blockLufs + 100.0f) * 10.0f), 0L,
                            static_cast<long>(loudnessHistogramBins - 1)));
                        ++loudnessBlockCount[channel][bin];
                        loudnessEnergySum[channel][bin] += blockMean;
                    }
                    integratedLoudness[channel].store(calculateIntegratedLufs(channel), std::memory_order_relaxed);
                }
            }
        }
        const auto blockRms = samples > 0 ? static_cast<float>(std::sqrt(energy / samples)) : 0.0f;
        peak[channel].store(blockPeak, std::memory_order_relaxed);
        rms[channel].store(blockRms, std::memory_order_relaxed);
        crest[channel].store(blockRms > 0.0f ? blockPeak / blockRms : 0.0f, std::memory_order_relaxed);
        const auto shortMean = shortEnergySum[channel] / static_cast<double>(std::max<std::size_t>(1, shortCount[channel]));
        shortLoudness[channel].store(energyToLufs(shortMean), std::memory_order_relaxed);
        reduction[channel].store(gainReductionDb, std::memory_order_relaxed);
        inputClip[channel].store(inClip, std::memory_order_relaxed);
        outputClip[channel].store(outClip, std::memory_order_relaxed);
    }
}
MeterReading MeterBank::reading(std::size_t channel) const noexcept
{
    if (channel >= maximumChannels) return {};
    return { peak[channel].load(std::memory_order_relaxed), rms[channel].load(std::memory_order_relaxed),
             crest[channel].load(std::memory_order_relaxed), shortLoudness[channel].load(std::memory_order_relaxed),
             integratedLoudness[channel].load(std::memory_order_relaxed), reduction[channel].load(std::memory_order_relaxed),
             inputClip[channel].load(std::memory_order_relaxed), outputClip[channel].load(std::memory_order_relaxed) };
}

void SpectrumAnalyzer::prepare(std::size_t requestedFftSize, std::size_t requestedHopSize)
{
    fftSize = std::min(maximumFftSize, nextPowerOfTwo(std::max<std::size_t>(16, requestedFftSize)));
    hopSize = std::clamp(requestedHopSize, std::size_t { 1 }, fftSize);
    activeBins = fftSize / 2 + 1; fft.prepare(fftSize);
    fifo.assign(fftSize, 0.0f); window.resize(fftSize); work.resize(fftSize);
    for (std::size_t index = 0; index < fftSize; ++index)
        window[index] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(index) / static_cast<float>(fftSize - 1));
    reset();
}
void SpectrumAnalyzer::reset() noexcept
{
    std::fill(fifo.begin(), fifo.end(), 0.0f); fifoPosition = 0; samplesSinceTransform = 0;
    for (auto& magnitude : magnitudes) magnitude.store(0.0f, std::memory_order_relaxed);
}
void SpectrumAnalyzer::process(std::span<const float> monoSamples) noexcept
{
    for (const auto sample : monoSamples)
    {
        fifo[fifoPosition] = sample;
        fifoPosition = (fifoPosition + 1) % fftSize;
        if (++samplesSinceTransform < hopSize) continue;
        samplesSinceTransform = 0;
        for (std::size_t index = 0; index < fftSize; ++index)
        {
            const auto source = (fifoPosition + index) % fftSize;
            work[index] = { fifo[source] * window[index], 0.0f };
        }
        fft.transform(work);
        for (std::size_t bin = 0; bin < activeBins; ++bin)
            magnitudes[bin].store(std::abs(work[bin]) * 2.0f / static_cast<float>(fftSize), std::memory_order_relaxed);
    }
}
std::size_t SpectrumAnalyzer::copyMagnitudes(std::span<float> destination) const noexcept
{
    const auto count = std::min(destination.size(), activeBins);
    for (std::size_t index = 0; index < count; ++index)
        destination[index] = magnitudes[index].load(std::memory_order_relaxed);
    return count;
}
void WaveformHistory::reset() noexcept
{
    for (auto& value : values) value.store(0.0f, std::memory_order_relaxed);
    writePosition.store(0, std::memory_order_relaxed);
}
void WaveformHistory::push(std::span<const float> samples, std::size_t decimation) noexcept
{
    decimation = std::max<std::size_t>(1, decimation);
    auto position = writePosition.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < samples.size(); index += decimation)
    {
        values[position].store(samples[index], std::memory_order_relaxed);
        position = (position + 1) % capacity;
    }
    writePosition.store(position, std::memory_order_release);
}
std::size_t WaveformHistory::copyLatest(std::span<float> destination) const noexcept
{
    const auto count = std::min(destination.size(), capacity);
    const auto end = writePosition.load(std::memory_order_acquire);
    const auto start = (end + capacity - count) % capacity;
    for (std::size_t index = 0; index < count; ++index)
        destination[index] = values[(start + index) % capacity].load(std::memory_order_relaxed);
    return count;
}
} // namespace nts::dsp
