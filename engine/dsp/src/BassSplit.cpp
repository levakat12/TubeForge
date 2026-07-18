#include "nts/dsp/BassSplit.h"

#include <algorithm>
#include <array>

namespace nts::dsp
{
void BassSplitProcessor::prepare(const ProcessSpec& newSpec)
{
    spec = newSpec; spec.channels = std::min(spec.channels, maximumChannels);
    crossover.prepare(spec); lowCompressor.prepare(spec); highWaveshaper.prepare(spec);
    lowBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
    highBuffer.assign(spec.maximumBlockSize * spec.channels, 0.0f);
}
void BassSplitProcessor::reset() noexcept { crossover.reset(); lowCompressor.reset(); }
void BassSplitProcessor::setCrossoverFrequency(double frequency, std::size_t interpolationSamples) noexcept
{
    crossover.setFrequency(frequency, interpolationSamples);
}
void BassSplitProcessor::setLowBandCompression(bool enabled,
                                               const CompressorParameters& parameters) noexcept
{
    compressLow = enabled; lowCompressor.setParameters(parameters);
}
void BassSplitProcessor::setHighBandWaveshaping(bool enabled, Waveshape shape, float drive) noexcept
{
    shapeHigh = enabled; highWaveshaper.setShape(shape); highWaveshaper.setDrive(drive);
}
void BassSplitProcessor::process(float* const* channels, std::size_t channelCount,
                                 std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, spec.channels);
    const auto processSamples = std::min(samples, spec.maximumBlockSize);
    std::array<const float*, maximumChannels> inputPointers {};
    std::array<float*, maximumChannels> lowPointers {};
    std::array<float*, maximumChannels> highPointers {};
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        inputPointers[channel] = channels[channel];
        lowPointers[channel] = lowBuffer.data() + channel * spec.maximumBlockSize;
        highPointers[channel] = highBuffer.data() + channel * spec.maximumBlockSize;
    }
    crossover.process(inputPointers.data(), lowPointers.data(), highPointers.data(), count, processSamples);
    if (compressLow) lowCompressor.process(lowPointers.data(), count, processSamples);
    if (shapeHigh)
        for (std::size_t channel = 0; channel < count; ++channel)
            highWaveshaper.process(highPointers[channel], processSamples);
    for (std::size_t channel = 0; channel < count; ++channel)
        for (std::size_t sample = 0; sample < processSamples; ++sample)
            channels[channel][sample] = lowPointers[channel][sample] + highPointers[channel][sample];
}
} // namespace nts::dsp
