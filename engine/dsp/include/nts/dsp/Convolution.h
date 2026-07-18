#pragma once

#include "Analysis.h"
#include "Common.h"

#include <atomic>
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace nts::dsp
{
struct ImpulseResponse
{
    double sampleRate { 48000.0 };
    std::vector<std::vector<float>> channels;
};

struct ImpulsePreparationOptions
{
    std::size_t outputChannels { 2 };
    float trimThresholdDb { -80.0f };
    bool removeDc { true };
    bool normalizePeak { true };
    float normalizationDb { -1.0f };
};

[[nodiscard]] ImpulseResponse prepareImpulseResponse(const ImpulseResponse& decoded,
                                                      double targetSampleRate,
                                                      const ImpulsePreparationOptions& options);

class DirectConvolver
{
public:
    void prepare(std::size_t maximumImpulseLength, std::size_t channels);
    void reset() noexcept;
    bool loadImpulse(std::span<const float> left, std::span<const float> right = {});
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::size_t impulseLength() const noexcept { return activeLength; }

private:
    std::size_t maximumLength {};
    std::size_t activeLength {};
    std::size_t configuredChannels {};
    std::size_t writePosition {};
    std::vector<float> impulse;
    std::vector<float> history;
};

class PartitionedConvolver
{
public:
    void prepare(std::size_t blockSize, std::size_t maximumImpulseLength, std::size_t channels);
    void reset() noexcept;
    bool loadImpulse(std::span<const float> left, std::span<const float> right = {});
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::size_t latencySamples() const noexcept { return 0; }
    [[nodiscard]] std::size_t partitions() const noexcept { return activePartitions; }

private:
    Fft fft;
    std::size_t blockSize {};
    std::size_t fftSize {};
    std::size_t maximumPartitions {};
    std::size_t activePartitions {};
    std::size_t configuredChannels {};
    std::size_t spectrumPosition {};
    std::vector<std::complex<float>> filters;
    std::vector<std::complex<float>> inputSpectra;
    std::vector<std::complex<float>> work;
    std::vector<std::complex<float>> accumulator;
    std::vector<float> overlap;
};

class CrossfadingConvolver
{
public:
    void prepare(std::size_t blockSize, std::size_t maximumImpulseLength, std::size_t channels);
    bool loadInactiveImpulse(std::span<const float> left, std::span<const float> right = {});
    void requestSwap(std::size_t crossfadeSamples) noexcept;
    void reset() noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] bool isCrossfading() const noexcept
    {
        return crossfadeActive.load(std::memory_order_acquire);
    }

private:
    PartitionedConvolver convolvers[2];
    std::atomic<int> activeIndex {};
    std::atomic<bool> swapRequested {};
    std::atomic<bool> crossfadeActive {};
    std::size_t fadeLength { 1 };
    std::size_t crossfadeRemaining {};
    std::size_t configuredChannels {};
    std::size_t maximumBlockSize {};
    std::vector<float> oldBuffer;
    std::vector<float> newBuffer;
};
} // namespace nts::dsp
