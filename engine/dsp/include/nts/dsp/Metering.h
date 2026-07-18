#pragma once

#include "Analysis.h"
#include "Common.h"
#include "Filters.h"

#include <array>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nts::dsp
{
struct MeterReading
{
    float samplePeak {};
    float rms {};
    float crestFactor {};
    float shortTermLufs { -120.0f };
    float integratedLufs { -120.0f };
    float gainReductionDb {};
    bool inputClipped {};
    bool outputClipped {};
};

class MeterBank
{
public:
    void prepare(const ProcessSpec& spec);
    void reset() noexcept;
    void process(const float* const* input, const float* const* output,
                 std::size_t channelCount, std::size_t samples,
                 float gainReductionDb = 0.0f) noexcept;
    [[nodiscard]] MeterReading reading(std::size_t channel) const noexcept;

private:
    struct FilterState
    {
        double z1 {};
        double z2 {};
        [[nodiscard]] float process(float input, const BiquadCoefficients& coefficients) noexcept
        {
            const auto output = coefficients.b0 * input + z1;
            z1 = coefficients.b1 * input - coefficients.a1 * output + z2;
            z2 = coefficients.b2 * input - coefficients.a2 * output;
            return static_cast<float>(output);
        }
        void reset() noexcept { z1 = z2 = 0.0; }
    };

    static constexpr std::size_t loudnessHistogramBins = 1401;
    [[nodiscard]] float calculateIntegratedLufs(std::size_t channel) const noexcept;
    double sampleRate { 48000.0 };
    BiquadCoefficients kWeightShelf;
    BiquadCoefficients kWeightHighPass;
    std::array<FilterState, maximumChannels> shelfState {};
    std::array<FilterState, maximumChannels> highPassState {};
    std::vector<double> shortEnergyHistory;
    std::vector<double> blockEnergyHistory;
    std::array<double, maximumChannels> shortEnergySum {};
    std::array<double, maximumChannels> blockEnergySum {};
    std::array<std::size_t, maximumChannels> shortPosition {};
    std::array<std::size_t, maximumChannels> blockPosition {};
    std::array<std::size_t, maximumChannels> shortCount {};
    std::array<std::size_t, maximumChannels> blockCount {};
    std::array<std::size_t, maximumChannels> hopCounter {};
    std::array<std::array<std::uint64_t, loudnessHistogramBins>, maximumChannels> loudnessBlockCount {};
    std::array<std::array<double, loudnessHistogramBins>, maximumChannels> loudnessEnergySum {};
    std::size_t shortWindowSamples { 144000 };
    std::size_t blockWindowSamples { 19200 };
    std::size_t loudnessHopSamples { 4800 };
    std::array<std::atomic<float>, maximumChannels> peak {};
    std::array<std::atomic<float>, maximumChannels> rms {};
    std::array<std::atomic<float>, maximumChannels> crest {};
    std::array<std::atomic<float>, maximumChannels> shortLoudness {};
    std::array<std::atomic<float>, maximumChannels> integratedLoudness {};
    std::array<std::atomic<float>, maximumChannels> reduction {};
    std::array<std::atomic<bool>, maximumChannels> inputClip {};
    std::array<std::atomic<bool>, maximumChannels> outputClip {};
};

class SpectrumAnalyzer
{
public:
    static constexpr std::size_t maximumFftSize = 8192;
    static constexpr std::size_t maximumBins = maximumFftSize / 2 + 1;

    void prepare(std::size_t fftSize, std::size_t hopSize);
    void reset() noexcept;
    void process(std::span<const float> monoSamples) noexcept;
    [[nodiscard]] std::size_t copyMagnitudes(std::span<float> destination) const noexcept;
    [[nodiscard]] std::size_t bins() const noexcept { return activeBins; }

private:
    Fft fft;
    std::size_t fftSize {};
    std::size_t hopSize {};
    std::size_t activeBins {};
    std::size_t fifoPosition {};
    std::size_t samplesSinceTransform {};
    std::vector<float> fifo;
    std::vector<float> window;
    std::vector<std::complex<float>> work;
    std::array<std::atomic<float>, maximumBins> magnitudes {};
};

class WaveformHistory
{
public:
    static constexpr std::size_t capacity = 4096;
    void reset() noexcept;
    void push(std::span<const float> samples, std::size_t decimation = 16) noexcept;
    [[nodiscard]] std::size_t copyLatest(std::span<float> destination) const noexcept;

private:
    std::array<std::atomic<float>, capacity> values {};
    std::atomic<std::size_t> writePosition {};
};
} // namespace nts::dsp
