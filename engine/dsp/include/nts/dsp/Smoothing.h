#pragma once

#include "Common.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace nts::dsp
{
enum class SmoothingMode
{
    linear,
    logarithmic
};

class SmoothedParameter
{
public:
    void prepare(double newSampleRate, double milliseconds, SmoothingMode newMode = SmoothingMode::linear) noexcept
    {
        sampleRate = std::max(1.0, newSampleRate);
        mode = newMode;
        setTimeMilliseconds(milliseconds);
    }

    void setTimeMilliseconds(double milliseconds) noexcept
    {
        rampSamples = std::max<std::size_t>(1, static_cast<std::size_t>(sampleRate * milliseconds * 0.001));
    }

    void reset(float value) noexcept
    {
        current = target = sanitize(value);
        remaining = 0;
        step = 0.0f;
    }

    void setTarget(float value) noexcept
    {
        target = sanitize(value);
        remaining = rampSamples;
        const auto from = transformed(current);
        const auto to = transformed(target);
        step = (to - from) / static_cast<float>(remaining);
        transformedCurrent = from;
    }

    [[nodiscard]] float next() noexcept
    {
        if (remaining == 0)
            return current = target;
        transformedCurrent += step;
        --remaining;
        current = inverseTransformed(transformedCurrent);
        if (remaining == 0)
            current = target;
        return current;
    }

    [[nodiscard]] float value() const noexcept { return current; }
    [[nodiscard]] bool isSmoothing() const noexcept { return remaining != 0; }

private:
    [[nodiscard]] float sanitize(float value) const noexcept
    {
        return mode == SmoothingMode::logarithmic ? std::max(value, 1.0e-6f) : value;
    }
    [[nodiscard]] float transformed(float value) const noexcept
    {
        return mode == SmoothingMode::logarithmic ? std::log(value) : value;
    }
    [[nodiscard]] float inverseTransformed(float value) const noexcept
    {
        return mode == SmoothingMode::logarithmic ? std::exp(value) : value;
    }

    double sampleRate { 48000.0 };
    std::size_t rampSamples { 1 };
    std::size_t remaining {};
    SmoothingMode mode { SmoothingMode::linear };
    float current {};
    float target {};
    float transformedCurrent {};
    float step {};
};

class SmoothedGain
{
public:
    void prepare(double sampleRate) noexcept { gain.prepare(sampleRate, rampMilliseconds); }
    void reset() noexcept { gain.reset(1.0f); }
    void setRampMilliseconds(double milliseconds) noexcept
    {
        rampMilliseconds = std::max(0.0, milliseconds);
        gain.setTimeMilliseconds(rampMilliseconds);
    }
    void resetDb(float decibels) noexcept { gain.reset(dbToLinear(decibels)); }
    void setTargetDb(float decibels) noexcept { gain.setTarget(dbToLinear(decibels)); }
    void process(float* samples, std::size_t count) noexcept
    {
        if (samples == nullptr)
            return;
        for (std::size_t index = 0; index < count; ++index)
            samples[index] *= gain.next();
    }
    [[nodiscard]] float currentLinearGain() const noexcept { return gain.value(); }

private:
    SmoothedParameter gain;
    double rampMilliseconds { 20.0 };
};
} // namespace nts::dsp
