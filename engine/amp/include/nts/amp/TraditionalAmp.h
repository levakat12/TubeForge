#pragma once

#include <nts/dsp/Common.h>
#include <nts/dsp/Convolution.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Filters.h>
#include <nts/dsp/ModeCrossfader.h>
#include <nts/dsp/Oversampling.h>
#include <nts/dsp/Smoothing.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nts::amp
{
enum class Instrument { guitar, bass };
enum class PickupProfile { passive, active };
enum class Topology { tightModern, vintageBloom };
enum class ToneStackType { passiveCoupled, activeThreeBand, bassSemiParametric };

struct CalibrationProfile
{
    float targetRmsLowDb { -24.0f };
    float targetRmsHighDb { -18.0f };
    float targetPeakDb { -12.0f };
    PickupProfile pickup { PickupProfile::passive };
};

struct CalibrationReading
{
    float peakDb { -120.0f };
    float rmsDb { -120.0f };
    float suggestedTrimDb {};
};

class InputCalibrator
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setProfile(const CalibrationProfile& profile) noexcept { calibration = profile; }
    void process(const float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] CalibrationReading reading() const noexcept { return lastReading; }

private:
    CalibrationProfile calibration;
    CalibrationReading lastReading;
    double smoothedEnergy {};
    double sampleRate { 48000.0 };
};

struct PreEqParameters
{
    float lowCutHz { 80.0f };
    float highCutHz { 18000.0f };
    float tightness { 0.5f };
    float pickEmphasisDb {};
    bool lowShelfEnabled {};
    float lowShelfDb {};
    bool midEmphasisEnabled {};
    float midEmphasisDb {};
};

struct PreampStageConfig
{
    float driveDb { 12.0f };
    float bias {};
    float asymmetry {};
    float lowCutHz { 80.0f };
    float highCutHz { 12000.0f };
    float outputTrimDb { -6.0f };
    int oversamplingFactor { 4 };
    float dynamicBias { 0.25f };
    float frequencySaturation { 0.2f };
    float attackReduction { 0.15f };
    float memoryAmount { 0.25f };
};

struct ToneStackParameters
{
    ToneStackType type { ToneStackType::passiveCoupled };
    float bass { 0.5f };
    float mid { 0.5f };
    float treble { 0.5f };
    float midFrequencyHz { 700.0f };
    float midQ { 0.8f };
};

struct PhaseInverterParameters
{
    float drive { 1.5f };
    float headroom { 0.8f };
    float asymmetry { 0.12f };
    float differentialImbalance { 0.05f };
    float feedback { 0.2f };
};

struct PowerAmpParameters
{
    float masterDb { -3.0f };
    float saturation { 0.45f };
    float damping { 0.6f };
    float sag { 0.35f };
    float biasCharacter {};
    float presence { 0.5f };
    float resonance { 0.5f };
    float feedback { 0.35f };
    float sagAttackMs { 35.0f };
    float sagRecoveryMs { 420.0f };
};

struct CabinetMetadata
{
    std::string name { "TubeForge 4x12" };
    std::string microphone { "Dynamic 57" };
    float position {};
};

struct CabinetParameters
{
    float blend { 0.5f };
    bool phaseInvertB {};
    std::size_t delaySamplesB {};
    float lowCutHz { 70.0f };
    float highCutHz { 10500.0f };
    bool bypass {};
    float bassDiBlend {};
};

struct BassPathParameters
{
    float crossoverHz { 180.0f };
    float cleanBlend { 0.55f };
    float lowCompression { 0.45f };
    float highDriveDb { 6.0f };
    float lowMono { 1.0f };
    bool lowSaturation {};
};

struct AmpParameters
{
    Instrument instrument { Instrument::guitar };
    PickupProfile pickup { PickupProfile::passive };
    Topology topology { Topology::tightModern };
    float manualInputTrimDb {};
    float gateThresholdDb { -58.0f };
    PreEqParameters preEq;
    std::array<PreampStageConfig, 4> stages {};
    std::size_t stageCount { 3 };
    ToneStackParameters toneStack;
    PhaseInverterParameters phaseInverter;
    PowerAmpParameters powerAmp;
    CabinetParameters cabinet;
    BassPathParameters bass;
    float postLowDb {};
    float postMidDb {};
    float postHighDb {};
    float outputGainDb { -6.0f };
    bool loudnessMatch { true };
};

struct AmpPreset
{
    int schemaVersion { 1 };
    std::string name { "Original Tight Guitar" };
    AmpParameters parameters;
    CalibrationProfile calibration;
};

[[nodiscard]] AmpPreset makeOriginalPreset(Topology topology, Instrument instrument);
[[nodiscard]] std::string serializePreset(const AmpPreset& preset, bool pretty = true);
[[nodiscard]] std::optional<AmpPreset> deserializePreset(std::string_view json);

class PreEq
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const PreEqParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

private:
    dsp::ProcessSpec spec;
    dsp::Biquad lowCut, highCut, pick, lowShelf, mid;
    bool useLowShelf {};
    bool useMid {};
};

class ResponsivePreampStage
{
public:
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void setConfig(const PreampStageConfig& config, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] float biasState(std::size_t channel) const noexcept;
    [[nodiscard]] const PreampStageConfig& configuration() const noexcept { return config; }
    [[nodiscard]] std::size_t latencySamples() const noexcept;

private:
    [[nodiscard]] dsp::Oversampler& selectedOversampler() noexcept;
    dsp::ProcessSpec spec;
    PreampStageConfig config;
    dsp::Biquad lowCut, highCut, dcBlock;
    std::array<dsp::Oversampler, 4> oversamplers;
    dsp::SmoothedParameter drive;
    dsp::SmoothedParameter trim;
    std::array<float, dsp::maximumChannels> envelope {};
    std::array<float, dsp::maximumChannels> previousEnvelope {};
    std::array<float, dsp::maximumChannels> biasMemory {};
    std::array<float, dsp::maximumChannels> recoveryGain {};
    std::array<float, dsp::maximumChannels> frequencyState {};
};

class ToneStack
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const ToneStackParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::array<dsp::BiquadCoefficients, 3> coefficients() const noexcept { return generated; }

private:
    dsp::ProcessSpec spec;
    ToneStackParameters parameters;
    std::array<dsp::BiquadCoefficients, 3> generated {};
    dsp::Biquad low, middle, high;
};

class PhaseInverter
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const PhaseInverterParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;

private:
    PhaseInverterParameters parameters;
    double sampleRate { 48000.0 };
    dsp::Biquad lowCut, shaping;
    std::array<float, dsp::maximumChannels> feedbackState {};
};

class PowerAmp
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept;
    void reset() noexcept;
    void setParameters(const PowerAmpParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] float supplyState(std::size_t channel) const noexcept;

private:
    dsp::ProcessSpec spec;
    PowerAmpParameters parameters;
    dsp::Biquad presence, resonance;
    dsp::SmoothedParameter master;
    std::array<float, dsp::maximumChannels> supply { 1.0f, 1.0f };
    std::array<float, dsp::maximumChannels> energy {};
    std::array<float, dsp::maximumChannels> feedbackState {};
    float sagAttackCoefficient {};
    float sagRecoveryCoefficient {};
};

class CabinetSection
{
public:
    static constexpr std::size_t maximumIrLength = 4096;
    static constexpr std::size_t maximumAlignmentSamples = 256;
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void setParameters(const CabinetParameters& parameters, std::size_t interpolationSamples = 64) noexcept;
    bool loadImpulseA(std::span<const float> left, std::span<const float> right = {}, CabinetMetadata metadata = {});
    bool loadImpulseB(std::span<const float> left, std::span<const float> right = {}, CabinetMetadata metadata = {});
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] const CabinetMetadata& metadataA() const noexcept { return firstMetadata; }
    [[nodiscard]] const CabinetMetadata& metadataB() const noexcept { return secondMetadata; }

private:
    dsp::ProcessSpec spec;
    CabinetParameters parameters;
    CabinetMetadata firstMetadata;
    CabinetMetadata secondMetadata;
    dsp::DirectConvolver first, second;
    dsp::Biquad lowCut, highCut;
    std::vector<float> dryBuffer, firstBuffer, secondBuffer, delay;
    std::array<std::size_t, dsp::maximumChannels> delayPosition {};
};

class AmpVoice
{
public:
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void configure(const AmpPreset& preset) noexcept;
    void setParameters(const AmpParameters& parameters) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] CalibrationReading calibrationReading() const noexcept { return calibrator.reading(); }
    [[nodiscard]] std::size_t latencySamples() const noexcept;

private:
    void processDrivenPath(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    dsp::ProcessSpec spec;
    AmpParameters parameters;
    InputCalibrator calibrator;
    dsp::NoiseGate gate;
    PreEq preEq;
    std::array<ResponsivePreampStage, 4> stages;
    ToneStack toneStack;
    PhaseInverter phaseInverter;
    PowerAmp powerAmp;
    CabinetSection cabinet;
    dsp::LinkwitzRileyCrossover bassCrossover;
    dsp::Compressor bassCompressor;
    dsp::Biquad postLow, postMid, postHigh;
    dsp::SmoothedParameter inputTrim, outputGain, cleanBlend;
    std::vector<float> lowBuffer, highBuffer, dryBuffer;
    std::array<double, dsp::maximumChannels> inputEnergy {};
    std::array<double, dsp::maximumChannels> outputEnergy {};
    std::array<float, dsp::maximumChannels> matchGain { 1.0f, 1.0f };
};

class TraditionalAmpProcessor
{
public:
    void prepare(const dsp::ProcessSpec& spec);
    void reset() noexcept;
    void setParameters(const AmpParameters& parameters) noexcept;
    void setParametersImmediately(const AmpParameters& parameters) noexcept;
    void loadPreset(const AmpPreset& preset, std::size_t crossfadeSamples = 2048);
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] CalibrationReading calibrationReading() const noexcept;
    [[nodiscard]] std::size_t latencySamples() const noexcept;
    [[nodiscard]] const AmpPreset& currentPreset() const noexcept { return presets[crossfader.mode()]; }

private:
    dsp::ProcessSpec spec;
    std::array<AmpVoice, 2> voices;
    std::array<AmpPreset, 2> presets;
    dsp::ModeCrossfader crossfader;
    AmpParameters requestedParameters;
    std::size_t transitionTarget {};
    bool transitionPending {};
};

[[nodiscard]] std::vector<float> renderOffline(TraditionalAmpProcessor& processor,
                                                std::span<const float> monoInput,
                                                std::size_t blockSize);
} // namespace nts::amp
