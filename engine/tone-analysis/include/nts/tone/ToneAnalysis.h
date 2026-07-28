#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace nts::tone
{
inline constexpr std::size_t toneEmbeddingDimensions = 128;
inline constexpr std::size_t toneFeatureVectorDimensions = 64;
inline constexpr std::string_view currentEmbeddingVersion = "tubeforge-tone-128-v1";
inline constexpr std::string_view currentAnalysisVersion = "tubeforge-analysis-v1";

enum class Instrument { unknown, guitar, bass };
enum class Technique { unknown, pick, fingerstyle };
enum class GainCategory { clean, edgeOfBreakup, crunch, highGain, fuzz };
enum class SourceType { isolatedStem, pairedCapture, pluginRender, physicalAmp, userRecording };

struct SpectralFeatures
{
    float lowFrequencyExtension {};
    float lowMidBuildup {};
    float midEmphasis {};
    float upperMidAttack {};
    float highFrequencyRolloff {};
    float resonantPeakStrength {};
    float spectralCentroidHz {};
    float spectralSlopeDbPerOctave {};
    float spectralFlatness {};
};

struct DynamicFeatures
{
    float crestFactorDb {};
    float transientPreservation {};
    float compressionAmount {};
    float attackMilliseconds {};
    float releaseMilliseconds {};
    float sustain {};
    float gainReductionEstimateDb {};
    float loudnessRangeDb {};
    float integratedLoudnessDb {};
};

struct NonlinearFeatures
{
    std::array<float, 8> harmonicDistribution {};
    float oddEvenBalance {};
    float intermodulationIndicator {};
    float saturationOnset {};
    float clippingAsymmetry {};
    float levelDependentSpectralChange {};
};

struct SpatialFeatures
{
    float stereoWidth {};
    float roomReverbEstimate {};
    float doubleTrackingLikelihood {};
    float panning {};
    float modulation {};
    float delayPresence {};
    float channelCorrelation { 1.0f };
};

struct InstrumentContext
{
    Instrument instrument { Instrument::unknown };
    Technique technique { Technique::unknown };
    GainCategory gainCategory { GainCategory::clean };
    float instrumentConfidence {};
    float techniqueConfidence {};
    float cleanDistortedConfidence {};
    float approximateRegisterHz {};
};

struct ConfidenceBreakdown
{
    float aggregate {};
    float signalToLeakage {};
    float classifierCertainty {};
    float duration {};
    float spectralCoverage {};
    float polyphony {};
    float separationQuality {};
    float modelDomainProximity {};
};

struct Descriptor
{
    float value {};
    float uncertainty { 1.0f };
};

struct ToneReport
{
    InstrumentContext context;
    Descriptor gain;
    Descriptor brightness;
    Descriptor tightness;
    Descriptor compression;
    Descriptor cleanLowBlendEstimate;
    Descriptor cabinetDarkness;
    Descriptor roomAmount;
    ConfidenceBreakdown confidence;
    std::vector<std::string> warnings;
};

struct ToneFeatures
{
    SpectralFeatures spectral;
    DynamicFeatures dynamic;
    NonlinearFeatures nonlinear;
    SpatialFeatures spatial;
    float durationSeconds {};
    float peak {};
    float rms {};
    float zeroCrossingRate {};
};

struct ToneEmbedding
{
    std::string version { currentEmbeddingVersion };
    std::array<float, toneEmbeddingDimensions> values {};
};

struct AnalysisInput
{
    std::span<const float> left;
    std::span<const float> right;
    double sampleRate { 48000.0 };
    SourceType sourceType { SourceType::userRecording };
    float sourceSeparationQuality { 1.0f };
};

struct ToneAnalysisResult
{
    bool success {};
    std::string error;
    std::string analysisVersion { currentAnalysisVersion };
    ToneFeatures features;
    ToneReport report;
    ToneEmbedding embedding;
};

class ToneEncoder
{
public:
    ToneEncoder();
    bool loadProjection(std::span<const float> weights, std::string version, std::string& error);
    [[nodiscard]] ToneEmbedding encode(std::span<const float, toneFeatureVectorDimensions> features) const noexcept;
    [[nodiscard]] const std::string& version() const noexcept { return embeddingVersion; }

private:
    std::array<float, toneEmbeddingDimensions * toneFeatureVectorDimensions> projection {};
    std::string embeddingVersion { currentEmbeddingVersion };
};

class ToneAnalyzer
{
public:
    [[nodiscard]] ToneAnalysisResult analyze(const AnalysisInput& input) const;
    [[nodiscard]] ToneEncoder& encoder() noexcept { return toneEncoder; }
    [[nodiscard]] const ToneEncoder& encoder() const noexcept { return toneEncoder; }

private:
    ToneEncoder toneEncoder;
};

[[nodiscard]] std::array<float, toneFeatureVectorDimensions> makeFeatureVector(
    const ToneFeatures& features, const ToneReport& report) noexcept;
[[nodiscard]] std::string serializeToneReport(const ToneAnalysisResult& result, bool pretty = true);
[[nodiscard]] std::string_view toString(Instrument value) noexcept;
[[nodiscard]] std::string_view toString(Technique value) noexcept;
[[nodiscard]] std::string_view toString(GainCategory value) noexcept;
[[nodiscard]] std::string_view toString(SourceType value) noexcept;
} // namespace nts::tone
