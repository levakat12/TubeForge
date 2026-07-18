#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tubeforge::dsp
{
class Biquad
{
public:
    void reset() noexcept
    {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    float process(float input) noexcept
    {
        const auto output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;
        return output;
    }

    void setHighPass(double sampleRate, float frequency, float q = 0.70710678f) noexcept
    {
        const auto omega = angularFrequency(sampleRate, frequency);
        const auto cosine = std::cos(omega);
        const auto alpha = std::sin(omega) / (2.0f * q);

        setNormalized((1.0f + cosine) * 0.5f,
                      -(1.0f + cosine),
                      (1.0f + cosine) * 0.5f,
                      1.0f + alpha,
                      -2.0f * cosine,
                      1.0f - alpha);
    }

    void setPeak(double sampleRate, float frequency, float q, float gainDb) noexcept
    {
        const auto amplitude = std::pow(10.0f, gainDb / 40.0f);
        const auto omega = angularFrequency(sampleRate, frequency);
        const auto cosine = std::cos(omega);
        const auto alpha = std::sin(omega) / (2.0f * q);

        setNormalized(1.0f + alpha * amplitude,
                      -2.0f * cosine,
                      1.0f - alpha * amplitude,
                      1.0f + alpha / amplitude,
                      -2.0f * cosine,
                      1.0f - alpha / amplitude);
    }

    void setLowShelf(double sampleRate, float frequency, float gainDb) noexcept
    {
        setShelf(sampleRate, frequency, gainDb, false);
    }

    void setHighShelf(double sampleRate, float frequency, float gainDb) noexcept
    {
        setShelf(sampleRate, frequency, gainDb, true);
    }

private:
    static float angularFrequency(double sampleRate, float frequency) noexcept
    {
        const auto nyquistSafe = std::clamp(frequency, 5.0f, static_cast<float>(sampleRate * 0.475));
        return 2.0f * std::numbers::pi_v<float> * nyquistSafe / static_cast<float>(sampleRate);
    }

    void setShelf(double sampleRate, float frequency, float gainDb, bool highShelf) noexcept
    {
        const auto amplitude = std::pow(10.0f, gainDb / 40.0f);
        const auto omega = angularFrequency(sampleRate, frequency);
        const auto cosine = std::cos(omega);
        const auto sine = std::sin(omega);
        const auto rootA = std::sqrt(amplitude);
        const auto alphaTerm = sine * std::sqrt(2.0f);

        if (highShelf)
        {
            setNormalized(amplitude * ((amplitude + 1.0f) + (amplitude - 1.0f) * cosine + rootA * alphaTerm),
                          -2.0f * amplitude * ((amplitude - 1.0f) + (amplitude + 1.0f) * cosine),
                          amplitude * ((amplitude + 1.0f) + (amplitude - 1.0f) * cosine - rootA * alphaTerm),
                          (amplitude + 1.0f) - (amplitude - 1.0f) * cosine + rootA * alphaTerm,
                          2.0f * ((amplitude - 1.0f) - (amplitude + 1.0f) * cosine),
                          (amplitude + 1.0f) - (amplitude - 1.0f) * cosine - rootA * alphaTerm);
            return;
        }

        setNormalized(amplitude * ((amplitude + 1.0f) - (amplitude - 1.0f) * cosine + rootA * alphaTerm),
                      2.0f * amplitude * ((amplitude - 1.0f) - (amplitude + 1.0f) * cosine),
                      amplitude * ((amplitude + 1.0f) - (amplitude - 1.0f) * cosine - rootA * alphaTerm),
                      (amplitude + 1.0f) + (amplitude - 1.0f) * cosine + rootA * alphaTerm,
                      -2.0f * ((amplitude - 1.0f) + (amplitude + 1.0f) * cosine),
                      (amplitude + 1.0f) + (amplitude - 1.0f) * cosine - rootA * alphaTerm);
    }

    void setNormalized(float numerator0, float numerator1, float numerator2,
                       float denominator0, float denominator1, float denominator2) noexcept
    {
        const auto inverseA0 = 1.0f / denominator0;
        b0 = numerator0 * inverseA0;
        b1 = numerator1 * inverseA0;
        b2 = numerator2 * inverseA0;
        a1 = denominator1 * inverseA0;
        a2 = denominator2 * inverseA0;
    }

    float b0 { 1.0f };
    float b1 { 0.0f };
    float b2 { 0.0f };
    float a1 { 0.0f };
    float a2 { 0.0f };
    float z1 { 0.0f };
    float z2 { 0.0f };
};
} // namespace tubeforge::dsp
