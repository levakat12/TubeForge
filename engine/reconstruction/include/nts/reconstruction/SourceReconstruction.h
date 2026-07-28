#pragma once

#include <nts/amp/TraditionalAmp.h>
#include <nts/tone/ToneProfileDatabase.h>

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace nts::reconstruction
{
inline constexpr std::string_view currentSeparationVersion = "tubeforge-cpu-stems-v1";
inline constexpr std::string_view currentReconstructionVersion = "tubeforge-reconstruction-v1";

enum class StemKind { vocals, drums, bass, other, guitar };
enum class TargetInstrument { guitar, bass };
enum class StereoMode { fullStereo, left, right, mid, side, pannedEstimate };
enum class GainCharacter { clean, crunch, distorted };

struct StereoAudio
{
    std::vector<float> left;
    std::vector<float> right;
    double sampleRate { 48000.0 };
    [[nodiscard]] std::size_t samples() const noexcept { return left.size(); }
    [[nodiscard]] bool stereo() const noexcept { return ! right.empty(); }
};

struct SeparationOptions
{
    std::string modelVersion { currentSeparationVersion };
    std::size_t chunkSamples { 262144 };
    std::size_t overlapSamples { 16384 };
    bool preferGpu {};
};

struct SeparationProgress
{
    float fraction {};
    std::string stage;
};
using ProgressCallback = std::function<bool(const SeparationProgress&)>;

struct StemSet
{
    bool success {};
    std::string error;
    StereoAudio vocals, drums, bass, other, guitar, piano;
    std::string cacheKey;
    std::string modelVersion { currentSeparationVersion };
    float reconstructionError {};
    bool usedGpu {};
    std::vector<std::string> warnings;
};

class StemSeparator
{
public:
    [[nodiscard]] StemSet separate(const StereoAudio& mixture,
                                   const SeparationOptions& options = {},
                                   const ProgressCallback& progress = {},
                                   std::stop_token stopToken = {}) const;
    [[nodiscard]] static std::string deterministicCacheKey(const StereoAudio& mixture,
                                                           const SeparationOptions& options);
};

struct RegionQuality
{
    double startSeconds {};
    double endSeconds {};
    float targetEnergy {};
    float vocalLeakage {};
    float drumLeakage {};
    float stereoStability {};
    float clipping {};
    float reverbAmount {};
    float duration {};
    float polyphonicDensity {};
    float confidence {};
    std::vector<std::string> warnings;
};

struct PlayableRegion
{
    RegionQuality quality;
    GainCharacter gainCharacter { GainCharacter::clean };
    float gainScore {};
    std::string dominantPitch { "Unknown pitch" };
    float dominantFrequencyHz {};
    float pitchConfidence {};
    int lowestPitchMidi { -1 };
    std::string estimatedTuning { "Tuning uncertain" };
    float tuningOffsetCents {};
};

[[nodiscard]] StereoAudio selectStem(const StemSet& stems, TargetInstrument target);
[[nodiscard]] StereoAudio applyStereoMode(const StereoAudio& audio, StereoMode mode);
[[nodiscard]] std::vector<RegionQuality> scoreRegions(const StemSet& stems, TargetInstrument target,
                                                      double regionSeconds = 5.0,
                                                      double hopSeconds = 2.5);
[[nodiscard]] RegionQuality recommendRegion(const StemSet& stems, TargetInstrument target,
                                            double regionSeconds = 5.0);
[[nodiscard]] std::vector<PlayableRegion> analyzePlayableRegions(
    const StereoAudio& targetAudio, std::span<const RegionQuality> qualityRegions,
    TargetInstrument target, std::size_t maximumRegions = 18);
[[nodiscard]] StereoAudio extractRegion(const StereoAudio& audio, double startSeconds, double endSeconds);

struct ReferenceNormalization
{
    StereoAudio audio;
    float originalLoudnessDb { -120.0f };
    float normalizedLoudnessDb { -18.0f };
    std::array<float, 3> broadProductionEqDb {};
    bool roomTailReduced {};
};

[[nodiscard]] ReferenceNormalization normalizeReference(const StereoAudio& audio,
                                                        bool reduceRoomTail = false);

struct DiAdaptation
{
    float inputTrimDb {};
    float lowShelfDb {};
    float midEqDb {};
    float highShelfDb {};
    float dynamicRangeScale { 1.0f };
    float outputGainDb {};
};

struct ReconstructionReference
{
    std::string sourceHash;
    double regionStartSeconds {};
    double regionEndSeconds {};
    std::string separationModelVersion { currentSeparationVersion };
    float analysisConfidence {};
    TargetInstrument target { TargetInstrument::guitar };
    std::string dominantPitch { "Unknown pitch" };
    std::string estimatedTuning { "Tuning uncertain" };
    std::string gainCharacter { "unknown" };
    float pitchConfidence {};
    float tuningOffsetCents {};
    nts::tone::ToneAnalysisResult tone;
    RegionQuality quality;
};

struct RigCandidate
{
    nts::amp::AmpPreset rigPreset;
    DiAdaptation adaptation;
    float toneSimilarity {};
    float recordingSimilarity {};
    float complexityPenalty {};
    float confidence {};
    std::vector<std::string> warnings;
};

struct ReconstructionResult
{
    bool success {};
    std::string error;
    std::string reconstructionVersion { currentReconstructionVersion };
    ReconstructionReference reference;
    std::array<float, nts::tone::toneEmbeddingDimensions> neuralConditioning {};
    std::vector<RigCandidate> candidates;
    std::vector<std::string> warnings;
};

class RigReconstructor
{
public:
    [[nodiscard]] ReconstructionResult reconstruct(const ReconstructionReference& reference,
                                                   std::span<const float> userDi,
                                                   double sampleRate,
                                                   std::size_t candidateCount = 4,
                                                   const ProgressCallback& progress = {},
                                                   std::stop_token stopToken = {}) const;
};

[[nodiscard]] std::string serializeResult(const ReconstructionResult& result, bool pretty = true);
[[nodiscard]] std::string_view toString(TargetInstrument value) noexcept;
[[nodiscard]] std::string_view toString(StereoMode value) noexcept;
[[nodiscard]] std::string_view toString(GainCharacter value) noexcept;
} // namespace nts::reconstruction
