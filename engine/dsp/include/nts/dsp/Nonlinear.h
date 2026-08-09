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
    diode,
    /** Germanium: about half silicon's forward voltage and a much softer knee.

        It starts rounding almost immediately and never reaches a flat top, which is why a
        germanium box sounds compressed and warm where a silicon one sounds like it is clipping.
        Asymmetric, because a matched pair of germanium diodes is a thing that does not exist.
    */
    germanium,
    /** LED clipping: three times a silicon diode's forward voltage, then an abrupt knee.

        The stage stays clean far longer and then clips hard, which is what makes an
        LED-clipped box loud and open rather than compressed.
    */
    ledClip
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
        case Waveshape::germanium:
        {
            const auto positive = std::tanh(1.7f * std::max(0.0f, x));
            const auto negative = -0.82f * std::tanh(2.1f * std::max(0.0f, -x));
            return positive + negative;
        }
        case Waveshape::ledClip:
        {
            // Linear to the forward voltage, then a soft ceiling above it. Bounded at 1 by
            // construction: the linear part reaches 0.82 and the tanh adds at most 0.18.
            constexpr auto forwardVoltage = 1.8f;
            const auto magnitude = std::abs(x);
            if (magnitude <= forwardVoltage) return x * (0.82f / forwardVoltage);
            return std::copysign(0.82f + 0.18f * std::tanh((magnitude - forwardVoltage) * 0.6f), x);
        }
    }
    return input;
}

/** log(cosh(x)), computed so it cannot overflow.

    The direct form dies past about x = 89, where cosh overflows a float and the logarithm of
    infinity is infinity. This identity -- |x| + log1p(e^(-2|x|)) - ln 2 -- is exact, costs the
    same, and is bounded for every input a waveshaper can produce.
*/
[[nodiscard]] inline float logCosh(float x) noexcept
{
    const auto magnitude = std::abs(x);
    return magnitude + std::log1p(std::exp(-2.0f * magnitude)) - 0.69314718056f;
}

/** Whether `shapeAntiderivative` covers a shape.

    `asymmetricPolynomial` is the one it does not. Its outer clamp binds partway up the curve --
    at about c = 0.957, where the polynomial first reaches 1 -- so the antiderivative is
    piecewise around a breakpoint that has to be found by solving a cubic. The shape is already
    soft and bounded, so it aliases far less than a hard clipper does, and an approximate
    antiderivative would be worse than none.
*/
[[nodiscard]] inline constexpr bool supportsAntiderivative(Waveshape shape) noexcept
{
    return shape != Waveshape::asymmetricPolynomial;
}

/** The first antiderivative of `shapeSample(x, shape, 1)`, with F(0) = 0.

    The constant of integration does not matter -- every use takes a difference of two values --
    but pinning it at zero keeps the numbers small and the continuity easy to check by eye.
*/
[[nodiscard]] inline float shapeAntiderivative(float x, Waveshape shape) noexcept
{
    const auto magnitude = std::abs(x);
    switch (shape)
    {
        case Waveshape::hyperbolicTangent: return logCosh(x);

        case Waveshape::arcTangent:
            return 0.63661977236f * (x * std::atan(x) - 0.5f * std::log1p(x * x));

        case Waveshape::hardClip:
            return magnitude <= 1.0f ? x * x * 0.5f : magnitude - 0.5f;

        case Waveshape::softClip:
        {
            // f = x - x^3/6.75 below the clamp, so F = x^2/2 - x^4/27. Above it f is constant
            // at 1, and F(1.5) = 0.9375 is where the linear part picks up.
            if (magnitude <= 1.5f) return x * x * 0.5f - (x * x * x * x) / 27.0f;
            return 0.9375f + (magnitude - 1.5f);
        }

        case Waveshape::asymmetricPolynomial:
            // Not covered; see supportsAntiderivative. Returning the symmetric integral of the
            // linear term keeps the function total rather than undefined, and nothing calls it.
            return x * x * 0.5f;

        case Waveshape::diode:
            // Two exponentials meeting at zero, each shifted so F(0) is exactly 0.
            return x >= 0.0f ? x + std::exp(-x) - 1.0f
                             : -0.72f * x + 0.4965517f * (std::exp(1.45f * x) - 1.0f);

        case Waveshape::germanium:
            return x >= 0.0f ? logCosh(1.7f * x) / 1.7f
                             : (0.82f / 2.1f) * logCosh(2.1f * x);

        case Waveshape::ledClip:
        {
            constexpr auto forwardVoltage = 1.8f;
            constexpr auto slope = 0.82f / forwardVoltage;
            if (magnitude <= forwardVoltage) return slope * x * x * 0.5f;
            // F at the knee, plus the constant part, plus the integral of the tanh tail.
            const auto beyond = magnitude - forwardVoltage;
            return slope * forwardVoltage * forwardVoltage * 0.5f
                 + 0.82f * beyond + 0.3f * logCosh(0.6f * beyond);
        }
    }
    return x * x * 0.5f;
}

/** A waveshaper that integrates rather than samples, which is most of the aliasing gone.

    Sampling a discontinuous curve produces harmonics above Nyquist that fold straight back into
    the audible band, and oversampling only pushes the problem up rather than removing it -- a
    hard clipper at high gain still folds at 4x. Taking the *average* of the curve across each
    sample interval instead, which is what the difference of the antiderivative computes,
    suppresses that fold by an order of magnitude for the same cost as one extra function
    evaluation and one stored sample per channel.

        y[n] = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1])

    The division is singular when consecutive samples are equal, which is not a rare edge case:
    it happens on silence, on any held DC, and on every sample of a signal quiet enough to
    quantise flat. Inside a small band the midpoint of the curve is used instead, which is what
    the quotient converges to and is continuous with it.

    Costs half a sample of delay and a little high-frequency loss. Both are inaudible next to
    the aliasing they remove, but they are why this is opt-in per model rather than always on --
    the built-in archetypes predate it and must keep sounding exactly as they did.
*/
class AntialiasedWaveshaper
{
public:
    void reset() noexcept { previous.fill(0.0f); }

    [[nodiscard]] float process(float input, Waveshape shape, float drive,
                                std::size_t channel) noexcept
    {
        const auto driven = input * std::max(0.0f, drive);
        if (channel >= previous.size()) return shapeSample(driven, shape, 1.0f);

        auto& last = previous[channel];
        const auto delta = driven - last;
        // 1e-5 rather than an exact zero test: the quotient is already numerically useless well
        // before the denominator reaches zero, and a float subtraction of two nearly equal
        // numbers loses most of its significant digits.
        const auto output = std::abs(delta) < 1.0e-5f
            ? shapeSample((driven + last) * 0.5f, shape, 1.0f)
            : (shapeAntiderivative(driven, shape) - shapeAntiderivative(last, shape)) / delta;
        last = driven;
        return output;
    }

private:
    std::array<float, maximumChannels> previous {};
};

/** Limits how fast the output may move, in units per sample.

    The mechanism behind a slow op-amp's character, and the reason a slow one sounds unlike a
    fast one running the same clipping curve. It is not a filter: there is no cutoff, only a
    ceiling on rate of change, so it does nothing at all to small or slow signals and rounds
    large fast ones progressively harder. That signal dependence is exactly what a low-pass
    cannot reproduce.

    Per channel, and stateful, so it belongs to the stage that owns it rather than being a free
    function like the shapes above.
*/
class SlewLimiter
{
public:
    void reset() noexcept { previous.fill(0.0f); }

    /// Units per sample. Zero disables the limiter entirely, which is the common case.
    void setMaximumStep(float step) noexcept { maximumStep = std::max(0.0f, step); }
    [[nodiscard]] bool engaged() const noexcept { return maximumStep > 0.0f; }

    [[nodiscard]] float process(float input, std::size_t channel) noexcept
    {
        if (maximumStep <= 0.0f || channel >= previous.size()) return input;
        auto& state = previous[channel];
        state += std::clamp(input - state, -maximumStep, maximumStep);
        return state;
    }

private:
    float maximumStep {};
    std::array<float, maximumChannels> previous {};
};

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
