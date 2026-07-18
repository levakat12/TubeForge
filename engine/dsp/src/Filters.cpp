#include "nts/dsp/Filters.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace nts::dsp
{
namespace
{
constexpr auto pi = 3.14159265358979323846;

BiquadCoefficients normalize(double b0, double b1, double b2,
                             double a0, double a1, double a2) noexcept
{
    const auto inverse = 1.0 / a0;
    return { b0 * inverse, b1 * inverse, b2 * inverse, a1 * inverse, a2 * inverse };
}
} // namespace

BiquadCoefficients BiquadCoefficients::make(FilterType type, double sampleRate,
                                             double frequency, double q, double gainDb) noexcept
{
    frequency = clampFrequency(frequency, sampleRate);
    q = std::clamp(q, 0.05, 50.0);
    const auto w0 = 2.0 * pi * frequency / sampleRate;
    const auto cosine = std::cos(w0);
    const auto sine = std::sin(w0);
    const auto alpha = sine / (2.0 * q);
    const auto amplitude = std::pow(10.0, gainDb / 40.0);

    switch (type)
    {
        case FilterType::lowPass:
            return normalize((1.0 - cosine) * 0.5, 1.0 - cosine, (1.0 - cosine) * 0.5,
                             1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
        case FilterType::highPass:
            return normalize((1.0 + cosine) * 0.5, -(1.0 + cosine), (1.0 + cosine) * 0.5,
                             1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
        case FilterType::peaking:
            return normalize(1.0 + alpha * amplitude, -2.0 * cosine, 1.0 - alpha * amplitude,
                             1.0 + alpha / amplitude, -2.0 * cosine, 1.0 - alpha / amplitude);
        case FilterType::notch:
            return normalize(1.0, -2.0 * cosine, 1.0,
                             1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
        case FilterType::allPass:
            return normalize(1.0 - alpha, -2.0 * cosine, 1.0 + alpha,
                             1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
        case FilterType::lowShelf:
        case FilterType::highShelf:
        {
            const auto shelfAlpha = sine * std::sqrt(2.0) * 0.5;
            const auto rootA = std::sqrt(amplitude);
            if (type == FilterType::lowShelf)
                return normalize(amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + 2.0 * rootA * shelfAlpha),
                                 2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
                                 amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - 2.0 * rootA * shelfAlpha),
                                 (amplitude + 1.0) + (amplitude - 1.0) * cosine + 2.0 * rootA * shelfAlpha,
                                 -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
                                 (amplitude + 1.0) + (amplitude - 1.0) * cosine - 2.0 * rootA * shelfAlpha);
            return normalize(amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + 2.0 * rootA * shelfAlpha),
                             -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
                             amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - 2.0 * rootA * shelfAlpha),
                             (amplitude + 1.0) - (amplitude - 1.0) * cosine + 2.0 * rootA * shelfAlpha,
                             2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
                             (amplitude + 1.0) - (amplitude - 1.0) * cosine - 2.0 * rootA * shelfAlpha);
        }
    }
    return {};
}

double BiquadCoefficients::magnitude(double frequency, double sampleRate) const noexcept
{
    const auto omega = -2.0 * pi * frequency / sampleRate;
    const auto z1c = std::polar(1.0, omega);
    const auto z2c = z1c * z1c;
    return std::abs((b0 + b1 * z1c + b2 * z2c) / (1.0 + a1 * z1c + a2 * z2c));
}

void OnePoleFilter::prepare(const ProcessSpec& spec) noexcept
{
    sampleRate = std::max(1.0, spec.sampleRate);
    reset();
    setCutoff(currentType, 1000.0);
}

void OnePoleFilter::reset() noexcept { state.fill(0.0f); }

void OnePoleFilter::setCutoff(Type type, double frequency) noexcept
{
    currentType = type;
    coefficient = static_cast<float>(1.0 - std::exp(-2.0 * pi * clampFrequency(frequency, sampleRate) / sampleRate));
}

void OnePoleFilter::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    for (std::size_t channel = 0; channel < std::min(channelCount, maximumChannels); ++channel)
    {
        if (channels[channel] == nullptr) continue;
        auto low = state[channel];
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto input = channels[channel][sample];
            low += coefficient * (input - low);
            channels[channel][sample] = currentType == Type::lowPass ? low : input - low;
        }
        state[channel] = suppressDenormal(low);
    }
}

void Biquad::prepare(const ProcessSpec&) noexcept { reset(); }
void Biquad::reset() noexcept { z1.fill(0.0); z2.fill(0.0); }

void Biquad::setCoefficients(const BiquadCoefficients& coefficients,
                             std::size_t interpolationSamples) noexcept
{
    target = coefficients;
    remaining = interpolationSamples;
    if (remaining == 0)
    {
        current = target;
        delta = {};
        return;
    }
    const auto divisor = static_cast<double>(remaining);
    delta = { (target.b0 - current.b0) / divisor, (target.b1 - current.b1) / divisor,
              (target.b2 - current.b2) / divisor, (target.a1 - current.a1) / divisor,
              (target.a2 - current.a2) / divisor };
}

void Biquad::advanceCoefficients() noexcept
{
    if (remaining == 0) return;
    current.b0 += delta.b0; current.b1 += delta.b1; current.b2 += delta.b2;
    current.a1 += delta.a1; current.a2 += delta.a2;
    if (--remaining == 0) current = target;
}

void Biquad::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, maximumChannels);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        advanceCoefficients();
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            if (channels[channel] == nullptr) continue;
            const auto input = static_cast<double>(channels[channel][sample]);
            const auto output = current.b0 * input + z1[channel];
            z1[channel] = current.b1 * input - current.a1 * output + z2[channel];
            z2[channel] = current.b2 * input - current.a2 * output;
            channels[channel][sample] = suppressDenormal(static_cast<float>(output));
        }
    }
}

void StateVariableFilter::prepare(const ProcessSpec& spec) noexcept
{
    sampleRate = std::max(1.0, spec.sampleRate);
    reset();
    setParameters(1000.0, 0.707, outputType);
}
void StateVariableFilter::reset() noexcept { integrator1.fill(0.0); integrator2.fill(0.0); }
void StateVariableFilter::setParameters(double frequency, double q, StateVariableOutput output) noexcept
{
    g = std::tan(pi * clampFrequency(frequency, sampleRate) / sampleRate);
    k = 1.0 / std::clamp(q, 0.05, 50.0);
    outputType = output;
}
void StateVariableFilter::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    for (std::size_t channel = 0; channel < std::min(channelCount, maximumChannels); ++channel)
    {
        if (channels[channel] == nullptr) continue;
        auto ic1 = integrator1[channel];
        auto ic2 = integrator2[channel];
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            const auto input = static_cast<double>(channels[channel][sample]);
            const auto v1 = (g * (input - ic2) + ic1) / (1.0 + g * (g + k));
            const auto v2 = ic2 + g * v1;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            const auto high = input - k * v1 - v2;
            double output {};
            switch (outputType)
            {
                case StateVariableOutput::lowPass: output = v2; break;
                case StateVariableOutput::bandPass: output = v1; break;
                case StateVariableOutput::highPass: output = high; break;
                case StateVariableOutput::notch: output = high + v2; break;
            }
            channels[channel][sample] = suppressDenormal(static_cast<float>(output));
        }
        integrator1[channel] = std::abs(ic1) < 1.0e-30 ? 0.0 : ic1;
        integrator2[channel] = std::abs(ic2) < 1.0e-30 ? 0.0 : ic2;
    }
}

void LinkwitzRileyCrossover::prepare(const ProcessSpec& spec) noexcept
{
    currentSpec = spec;
    low1.prepare(spec); low2.prepare(spec); high1.prepare(spec); high2.prepare(spec);
    setFrequency(1000.0);
}
void LinkwitzRileyCrossover::reset() noexcept { low1.reset(); low2.reset(); high1.reset(); high2.reset(); }
void LinkwitzRileyCrossover::setFrequency(double frequency, std::size_t interpolationSamples) noexcept
{
    const auto low = BiquadCoefficients::make(FilterType::lowPass, currentSpec.sampleRate, frequency);
    const auto high = BiquadCoefficients::make(FilterType::highPass, currentSpec.sampleRate, frequency);
    low1.setCoefficients(low, interpolationSamples); low2.setCoefficients(low, interpolationSamples);
    high1.setCoefficients(high, interpolationSamples); high2.setCoefficients(high, interpolationSamples);
}
void LinkwitzRileyCrossover::process(const float* const* input, float* const* low, float* const* high,
                                     std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(channelCount, maximumChannels);
    for (std::size_t channel = 0; channel < count; ++channel)
        for (std::size_t sample = 0; sample < samples; ++sample)
            low[channel][sample] = high[channel][sample] = input[channel][sample];
    low1.process(low, count, samples); low2.process(low, count, samples);
    high1.process(high, count, samples); high2.process(high, count, samples);
}
} // namespace nts::dsp
