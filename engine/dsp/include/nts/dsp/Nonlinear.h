#pragma once

#include "Common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace nts::dsp
{
enum class Waveshape
{
    hyperbolicTangent,
    arcTangent,
    hardClip,
    softClip,
    asymmetricPolynomial,
    diode
};

[[nodiscard]] inline float shapeSample(float input, Waveshape shape, float drive = 1.0f) noexcept
{
    const auto x = input * std::max(0.0f, drive);
    switch (shape)
    {
        case Waveshape::hyperbolicTangent: return std::tanh(x);
        case Waveshape::arcTangent: return 0.63661977236f * std::atan(x);
        case Waveshape::hardClip: return std::clamp(x, -1.0f, 1.0f);
        case Waveshape::softClip:
        {
            const auto clamped = std::clamp(x, -1.5f, 1.5f);
            return clamped - (clamped * clamped * clamped) / 6.75f;
        }
        case Waveshape::asymmetricPolynomial:
        {
            const auto clamped = std::clamp(x, -1.0f, 1.0f);
            return std::clamp(clamped + 0.22f * clamped * clamped - 0.18f * clamped * clamped * clamped,
                              -1.0f, 1.0f);
        }
        case Waveshape::diode:
        {
            const auto positive = 1.0f - std::exp(-std::max(0.0f, x));
            const auto negative = -0.72f * (1.0f - std::exp(-1.45f * std::max(0.0f, -x)));
            return positive + negative;
        }
    }
    return input;
}

class BiasableWaveshaper
{
public:
    void prepare(const ProcessSpec&) noexcept { reset(); }
    void reset() noexcept {}
    void setShape(Waveshape newShape) noexcept { shape = newShape; }
    void setDrive(float newDrive) noexcept { drive = std::clamp(newDrive, 0.0f, 50.0f); }
    void setBias(float newBias) noexcept { bias = std::clamp(newBias, -1.0f, 1.0f); }
    [[nodiscard]] float processSample(float input) const noexcept
    {
        return shapeSample(input + bias, shape, drive) - shapeSample(bias, shape, drive);
    }
    void process(float* samples, std::size_t count) const noexcept
    {
        for (std::size_t index = 0; index < count; ++index)
            samples[index] = processSample(samples[index]);
    }

private:
    Waveshape shape { Waveshape::hyperbolicTangent };
    float drive { 1.0f };
    float bias {};
};

class DynamicWaveshaper
{
public:
    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = std::max(1.0, spec.sampleRate);
        reset();
        updateCoefficients();
    }
    void reset() noexcept { envelope.fill(0.0f); }
    void setParameters(float baseDrive, float envelopeAmount, double attackMs, double releaseMs) noexcept
    {
        drive = std::clamp(baseDrive, 0.0f, 50.0f);
        amount = std::clamp(envelopeAmount, -2.0f, 2.0f);
        attack = std::max(0.01, attackMs);
        release = std::max(0.01, releaseMs);
        updateCoefficients();
    }
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
    {
        for (std::size_t channel = 0; channel < std::min(channelCount, maximumChannels); ++channel)
        {
            auto env = envelope[channel];
            for (std::size_t sample = 0; sample < samples; ++sample)
            {
                const auto input = channels[channel][sample];
                const auto detector = std::abs(input);
                const auto coefficient = detector > env ? attackCoefficient : releaseCoefficient;
                env = detector + coefficient * (env - detector);
                const auto dynamicDrive = std::max(0.0f, drive * (1.0f + amount * env));
                channels[channel][sample] = std::tanh(input * dynamicDrive);
            }
            envelope[channel] = suppressDenormal(env);
        }
    }

private:
    void updateCoefficients() noexcept
    {
        attackCoefficient = static_cast<float>(std::exp(-1.0 / (sampleRate * attack * 0.001)));
        releaseCoefficient = static_cast<float>(std::exp(-1.0 / (sampleRate * release * 0.001)));
    }
    std::array<float, maximumChannels> envelope {};
    double sampleRate { 48000.0 };
    double attack { 1.0 };
    double release { 80.0 };
    float attackCoefficient {};
    float releaseCoefficient {};
    float drive { 1.0f };
    float amount {};
};
} // namespace nts::dsp
