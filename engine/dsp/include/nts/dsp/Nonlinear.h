#pragma once

#include "Common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace nts::dsp
{
/** Rational approximations to tanh and the logistic sigmoid.

    A recurrent model spends most of its time here rather than in its matrix products: an LSTM
    evaluates three sigmoids and two tanh per hidden unit per sample, which at 64 units and
    48 kHz is around fifteen million transcendental calls a second. `std::tanh` and `std::exp`
    are correctly rounded and cost roughly twenty-five cycles each; these cost about ten
    operations with no branch beyond the saturation test.

    Accuracy is a Pade-derived rational, exact to under 1e-6 for |x| below about 4 and bounded
    by 1e-4 everywhere, the error concentrated where the true function is already saturated past
    0.9999. That is far tighter than a gate activation needs -- an LSTM's recurrence is
    contractive, so an error of this size does not accumulate -- but it is not bit-exact, which
    is why the runtime keeps the exact path and takes these only when asked.

    The sigmoid is derived from the tanh rather than approximated separately, because
    `sigmoid(x) = (1 + tanh(x/2)) / 2` is an identity: one approximation, one error bound.
*/
[[nodiscard]] inline float fastTanh(float value) noexcept
{
    // Guards the sixth power below against overflowing float, which would make both halves of
    // the ratio infinite and the result NaN. Well outside any range where tanh is not already 1.
    if (value <= -20.0f) return -1.0f;
    if (value >= 20.0f) return 1.0f;
    const auto square = value * value;
    const auto numerator = value * (135135.0f + square * (17325.0f + square * (378.0f + square)));
    const auto denominator = 135135.0f + square * (62370.0f + square * (3150.0f + 28.0f * square));
    // Clamped on the *output*, not the input. Past about |x| = 5 the rational drifts above
    // unity, and clamping there is not merely safe but more accurate than either continuing the
    // rational or cutting over to a constant earlier: the true function is already within 1e-4
    // of its limit, so the clamp inherits that as its whole error.
    return std::clamp(numerator / denominator, -1.0f, 1.0f);
}

[[nodiscard]] inline float fastSigmoid(float value) noexcept
{
    return 0.5f * (1.0f + fastTanh(0.5f * value));
}

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
