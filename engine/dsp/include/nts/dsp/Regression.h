#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nts::dsp
{
class StimulusGenerator
{
public:
    [[nodiscard]] static std::vector<float> impulse(std::size_t samples, float amplitude = 1.0f);
    [[nodiscard]] static std::vector<float> sineSweep(std::size_t samples, double sampleRate,
                                                      double startHz, double endHz);
    [[nodiscard]] static std::vector<float> multiTone(std::size_t samples, double sampleRate,
                                                      std::span<const double> frequencies);
    [[nodiscard]] static std::vector<float> whiteNoise(std::size_t samples, std::uint32_t seed);
    [[nodiscard]] static std::vector<float> pinkNoise(std::size_t samples, std::uint32_t seed);
    [[nodiscard]] static std::vector<float> guitarDi(std::size_t samples, double sampleRate);
    [[nodiscard]] static std::vector<float> bassDi(std::size_t samples, double sampleRate);
    [[nodiscard]] static std::vector<float> palmMute(std::size_t samples, double sampleRate);
    [[nodiscard]] static std::vector<float> transient(std::size_t samples, double sampleRate);
};

struct RegressionMetrics
{
    double maximumAbsoluteError {};
    double rmsError {};
    double spectralErrorDb {};
    std::ptrdiff_t latencyOffset {};
    double dcOffset {};
};

[[nodiscard]] RegressionMetrics compareAudio(std::span<const float> reference,
                                             std::span<const float> actual);
[[nodiscard]] std::uint64_t hashAudio(std::span<const float> samples) noexcept;
} // namespace nts::dsp
