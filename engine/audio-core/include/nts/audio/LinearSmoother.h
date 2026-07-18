#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace nts::audio
{
class LinearSmoother
{
public:
    void prepare(double sampleRate, double rampSeconds) noexcept
    {
        rampLength = std::max<std::size_t>(1, static_cast<std::size_t>(sampleRate * rampSeconds));
    }

    void reset(float value) noexcept
    {
        current = value;
        target = value;
        step = 0.0f;
        remaining = 0;
    }

    void setTarget(float value) noexcept
    {
        if (std::abs(value - target) < 1.0e-7f)
            return;

        target = value;
        remaining = rampLength;
        step = (target - current) / static_cast<float>(remaining);
    }

    [[nodiscard]] float next() noexcept
    {
        if (remaining == 0)
            return current;

        current += step;
        if (--remaining == 0)
            current = target;
        return current;
    }

    [[nodiscard]] float currentValue() const noexcept { return current; }
    [[nodiscard]] float targetValue() const noexcept { return target; }

private:
    float current { 1.0f };
    float target { 1.0f };
    float step {};
    std::size_t remaining {};
    std::size_t rampLength { 1 };
};
} // namespace nts::audio
