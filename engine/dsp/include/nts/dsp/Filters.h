#pragma once

#include "Common.h"

#include <array>
#include <cstddef>

namespace nts::dsp
{
enum class FilterType
{
    lowPass,
    highPass,
    peaking,
    lowShelf,
    highShelf,
    notch,
    allPass
};

struct BiquadCoefficients
{
    double b0 { 1.0 };
    double b1 {};
    double b2 {};
    double a1 {};
    double a2 {};

    [[nodiscard]] static BiquadCoefficients make(FilterType type,
                                                  double sampleRate,
                                                  double frequency,
                                                  double q = 0.7071067811865476,
                                                  double gainDb = 0.0) noexcept;
    [[nodiscard]] double magnitude(double frequency, double sampleRate) const noexcept;
};

class OnePoleFilter
{
public:
    enum class Type { lowPass, highPass };
    void prepare(const ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setCutoff(Type type, double frequency) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

private:
    std::array<float, maximumChannels> state {};
    double sampleRate { 48000.0 };
    float coefficient {};
    Type currentType { Type::lowPass };
};

class Biquad
{
public:
    void prepare(const ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setCoefficients(const BiquadCoefficients& coefficients,
                         std::size_t interpolationSamples = 0) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

private:
    void advanceCoefficients() noexcept;
    std::array<double, maximumChannels> z1 {};
    std::array<double, maximumChannels> z2 {};
    BiquadCoefficients current;
    BiquadCoefficients target;
    BiquadCoefficients delta;
    std::size_t remaining {};
};

enum class StateVariableOutput { lowPass, bandPass, highPass, notch };

class StateVariableFilter
{
public:
    void prepare(const ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(double frequency, double q, StateVariableOutput output) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

private:
    std::array<double, maximumChannels> integrator1 {};
    std::array<double, maximumChannels> integrator2 {};
    double sampleRate { 48000.0 };
    double g {};
    double k { 1.4142135623730951 };
    StateVariableOutput outputType { StateVariableOutput::lowPass };
};

class LinkwitzRileyCrossover
{
public:
    void prepare(const ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setFrequency(double frequency, std::size_t interpolationSamples = 0) noexcept;
    void process(const float* const* input, float* const* low, float* const* high,
                 std::size_t channelCount, std::size_t samples) noexcept;

private:
    ProcessSpec currentSpec;
    Biquad low1, low2, high1, high2;
};
} // namespace nts::dsp
