#pragma once

#include "Dynamics.h"
#include "Filters.h"
#include "Nonlinear.h"

#include <cstddef>
#include <vector>

namespace nts::dsp
{
class BassSplitProcessor
{
public:
    void prepare(const ProcessSpec& spec);
    void reset() noexcept;
    void setCrossoverFrequency(double frequency, std::size_t interpolationSamples = 0) noexcept;
    void setLowBandCompression(bool enabled, const CompressorParameters& parameters) noexcept;
    void setHighBandWaveshaping(bool enabled, Waveshape shape, float drive) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

private:
    ProcessSpec spec;
    LinkwitzRileyCrossover crossover;
    Compressor lowCompressor;
    BiasableWaveshaper highWaveshaper;
    bool compressLow {};
    bool shapeHigh {};
    std::vector<float> lowBuffer;
    std::vector<float> highBuffer;
};
} // namespace nts::dsp
