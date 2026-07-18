#include "nts/dsp/Regression.h"

#include "nts/dsp/Analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>

namespace nts::dsp
{
std::vector<float> StimulusGenerator::impulse(std::size_t samples, float amplitude)
{
    std::vector<float> result(samples, 0.0f); if (! result.empty()) result[0] = amplitude; return result;
}
std::vector<float> StimulusGenerator::sineSweep(std::size_t samples, double sampleRate,
                                                 double startHz, double endHz)
{
    std::vector<float> result(samples); double phase {};
    const auto ratio = std::pow(endHz / startHz, 1.0 / std::max<std::size_t>(1, samples - 1));
    auto frequency = startHz;
    for (auto& sample : result)
    {
        sample = static_cast<float>(std::sin(phase));
        phase += 2.0 * std::numbers::pi * frequency / sampleRate; frequency *= ratio;
    }
    return result;
}
std::vector<float> StimulusGenerator::multiTone(std::size_t samples, double sampleRate,
                                                 std::span<const double> frequencies)
{
    std::vector<float> result(samples, 0.0f);
    for (std::size_t index = 0; index < samples; ++index)
        for (const auto frequency : frequencies)
            result[index] += static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * index / sampleRate)
                                                / std::max<std::size_t>(1, frequencies.size()));
    return result;
}
std::vector<float> StimulusGenerator::whiteNoise(std::size_t samples, std::uint32_t seed)
{
    std::mt19937 generator(seed); std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
    std::vector<float> result(samples); for (auto& sample : result) sample = distribution(generator); return result;
}
std::vector<float> StimulusGenerator::pinkNoise(std::size_t samples, std::uint32_t seed)
{
    const auto white = whiteNoise(samples, seed); std::vector<float> result(samples); float b0 {}, b1 {}, b2 {};
    for (std::size_t index = 0; index < samples; ++index)
    {
        b0 = 0.99765f * b0 + white[index] * 0.0990460f;
        b1 = 0.96300f * b1 + white[index] * 0.2965164f;
        b2 = 0.57000f * b2 + white[index] * 1.0526913f;
        result[index] = (b0 + b1 + b2 + white[index] * 0.1848f) * 0.05f;
    }
    return result;
}
std::vector<float> StimulusGenerator::guitarDi(std::size_t samples, double sampleRate)
{
    std::vector<float> result(samples); const double notes[] { 82.41, 123.47, 164.81, 246.94 };
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto time = static_cast<double>(index) / sampleRate; const auto envelope = std::exp(-time * 2.8);
        for (const auto note : notes) result[index] += static_cast<float>(0.12 * envelope * std::sin(2.0 * std::numbers::pi * note * time));
    }
    return result;
}
std::vector<float> StimulusGenerator::bassDi(std::size_t samples, double sampleRate)
{
    std::vector<float> result(samples);
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto time = static_cast<double>(index) / sampleRate; const auto envelope = std::exp(-time * 1.5);
        result[index] = static_cast<float>(0.65 * envelope * (std::sin(2.0 * std::numbers::pi * 41.2 * time)
                         + 0.22 * std::sin(2.0 * std::numbers::pi * 82.4 * time)));
    }
    return result;
}
std::vector<float> StimulusGenerator::palmMute(std::size_t samples, double sampleRate)
{
    std::vector<float> result(samples); const auto noise = whiteNoise(samples, 0x50414c4dU);
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto time = static_cast<double>(index) / sampleRate;
        result[index] = static_cast<float>(std::exp(-time * 45.0)
            * (0.7 * std::sin(2.0 * std::numbers::pi * 82.41 * time) + 0.08 * noise[index]));
    }
    return result;
}
std::vector<float> StimulusGenerator::transient(std::size_t samples, double sampleRate)
{
    std::vector<float> result(samples);
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto time = static_cast<double>(index) / sampleRate;
        result[index] = static_cast<float>(std::exp(-time * 120.0) * std::sin(2.0 * std::numbers::pi * 1800.0 * time));
    }
    return result;
}

RegressionMetrics compareAudio(std::span<const float> reference, std::span<const float> actual)
{
    RegressionMetrics metrics; const auto count = std::min(reference.size(), actual.size()); if (count == 0) return metrics;
    double squared {}; double dc {};
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto error = static_cast<double>(actual[index]) - reference[index];
        metrics.maximumAbsoluteError = std::max(metrics.maximumAbsoluteError, std::abs(error));
        squared += error * error; dc += actual[index];
    }
    metrics.rmsError = std::sqrt(squared / count); metrics.dcOffset = dc / count;
    const auto search = std::min<std::size_t>(count / 2, 4096); double best = -1.0e300;
    for (std::ptrdiff_t offset = -static_cast<std::ptrdiff_t>(search); offset <= static_cast<std::ptrdiff_t>(search); ++offset)
    {
        double correlation {};
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto shifted = static_cast<std::ptrdiff_t>(index) + offset;
            if (shifted >= 0 && shifted < static_cast<std::ptrdiff_t>(count)) correlation += reference[index] * actual[static_cast<std::size_t>(shifted)];
        }
        if (correlation > best) { best = correlation; metrics.latencyOffset = offset; }
    }
    const auto fftSize = nextPowerOfTwo(count); Fft fft; fft.prepare(fftSize);
    std::vector<std::complex<float>> referenceSpectrum(fftSize), actualSpectrum(fftSize);
    for (std::size_t index = 0; index < count; ++index) { referenceSpectrum[index] = reference[index]; actualSpectrum[index] = actual[index]; }
    fft.transform(referenceSpectrum); fft.transform(actualSpectrum); double spectralSquared {};
    for (std::size_t bin = 0; bin <= fftSize / 2; ++bin)
    {
        const auto referenceDb = 20.0 * std::log10(std::max(1.0e-12f, std::abs(referenceSpectrum[bin])));
        const auto actualDb = 20.0 * std::log10(std::max(1.0e-12f, std::abs(actualSpectrum[bin])));
        const auto difference = actualDb - referenceDb; spectralSquared += difference * difference;
    }
    metrics.spectralErrorDb = std::sqrt(spectralSquared / (fftSize / 2 + 1)); return metrics;
}
std::uint64_t hashAudio(std::span<const float> samples) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto sample : samples)
    {
        const auto quantized = static_cast<std::int32_t>(std::clamp(sample, -1.0f, 1.0f) * 8388607.0f);
        for (int byte = 0; byte < 4; ++byte) { hash ^= static_cast<std::uint8_t>(quantized >> (byte * 8)); hash *= 1099511628211ULL; }
    }
    return hash;
}
} // namespace nts::dsp
