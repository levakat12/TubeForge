#pragma once

#include "Common.h"
#include "Smoothing.h"

#include <array>
#include <cstddef>
#include <vector>

namespace nts::dsp
{
enum class GateState { closed, opening, open, holding, closing };

struct NoiseGateParameters
{
    float thresholdDb { -50.0f };
    float rangeDb { -80.0f };
    double attackMs { 2.0 };
    double holdMs { 20.0 };
    double releaseMs { 80.0 };
    float hysteresisDb { 3.0f };
    double sidechainHighPassHz { 80.0 };
};

class NoiseGate
{
public:
    void prepare(const ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const NoiseGateParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] GateState state() const noexcept { return currentState; }
    [[nodiscard]] float gainReductionDb() const noexcept { return -linearToDb(std::max(gain, 1.0e-8f)); }

private:
    void updateCoefficients() noexcept;
    NoiseGateParameters parameters;
    double sampleRate { 48000.0 };
    float highPassCoefficient {};
    std::array<float, maximumChannels> sidechainLow {};
    float envelope {};
    float gain {};
    float attackCoefficient {};
    float releaseCoefficient {};
    SmoothedParameter thresholdSmoother;
    SmoothedParameter rangeSmoother;
    SmoothedParameter hysteresisSmoother;
    std::size_t holdSamples {};
    std::size_t holdRemaining {};
    GateState currentState { GateState::closed };
};

enum class DetectorMode { peak, rms };

struct CompressorParameters
{
    float thresholdDb { -18.0f };
    float ratio { 4.0f };
    float kneeDb { 6.0f };
    double attackMs { 10.0 };
    double releaseMs { 100.0 };
    float makeupDb {};
    DetectorMode detector { DetectorMode::rms };
    bool stereoLink { true };
    double sidechainHighPassHz { 0.0 };
};

class Compressor
{
public:
    void prepare(const ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const CompressorParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] float gainReductionDb() const noexcept { return lastGainReductionDb; }

private:
    [[nodiscard]] static float gainComputer(float levelDb, float thresholdDb,
                                            float ratio, float kneeDb) noexcept;
    void updateCoefficients() noexcept;
    CompressorParameters parameters;
    double sampleRate { 48000.0 };
    float attackCoefficient {};
    float releaseCoefficient {};
    float sidechainCoefficient {};
    std::array<float, maximumChannels> sidechainLow {};
    std::array<float, maximumChannels> detectorEnvelope {};
    std::array<float, maximumChannels> smoothedGainDb {};
    float lastGainReductionDb {};
    SmoothedParameter thresholdSmoother;
    SmoothedParameter ratioSmoother;
    SmoothedParameter kneeSmoother;
    SmoothedParameter makeupSmoother;
};

struct LimiterParameters
{
    float ceilingDb { -0.3f };
    double releaseMs { 60.0 };
    double lookaheadMs { 1.0 };
};

class PeakLimiter
{
public:
    void prepare(const ProcessSpec& spec, double maximumLookaheadMs = 20.0);
    void reset() noexcept;
    void setParameters(const LimiterParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::size_t latencySamples() const noexcept { return lookaheadSamples; }
    [[nodiscard]] float gainReductionDb() const noexcept { return -linearToDb(std::max(gain, 1.0e-8f)); }

private:
    ProcessSpec spec;
    LimiterParameters parameters;
    std::vector<float> delay;
    std::size_t delayCapacity {};
    std::size_t writePosition {};
    std::size_t lookaheadSamples {};
    float gain { 1.0f };
    float releaseCoefficient {};
    SmoothedParameter ceilingSmoother;
};
} // namespace nts::dsp
