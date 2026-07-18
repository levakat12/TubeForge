#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace nts::dsp
{
inline constexpr std::size_t maximumChannels = 2;

struct ProcessSpec
{
    double sampleRate { 48000.0 };
    std::size_t maximumBlockSize { 512 };
    std::size_t channels { 2 };
};

[[nodiscard]] inline float dbToLinear(float decibels) noexcept
{
    return std::pow(10.0f, decibels * 0.05f);
}

[[nodiscard]] inline float linearToDb(float gain) noexcept
{
    return 20.0f * std::log10(std::max(gain, std::numeric_limits<float>::min()));
}

[[nodiscard]] inline double clampFrequency(double frequency, double sampleRate) noexcept
{
    return std::clamp(frequency, 1.0, std::max(1.0, sampleRate * 0.499));
}

[[nodiscard]] inline float suppressDenormal(float value) noexcept
{
    return std::abs(value) < 1.0e-30f ? 0.0f : value;
}
} // namespace nts::dsp
