#pragma once

#include <nts/amp/TraditionalAmp.h>
#include <nts/diagnostics/SpscRingBuffer.h>
#include <nts/tone/ToneAnalysis.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nts::assistant
{
inline constexpr std::string_view currentAssistantVersion = "tubeforge-assistant-v1";

enum class Goal
{
    diagnose, tightRhythm, cleanBassSupport, aggressivePickedBass, smoothLead,
    warmClean, lessHarsh, preserveLowEnd, matchReference
};

enum class Problem
{
    inputTooQuiet, inputClipping, excessiveNoise, intermittentSignal, activePickupOverload,
    poorCalibration, excessivePreampGain, insufficientModelLevel, postStageClipping,
    outputClipping, noisyGainStaging, muddyLowMids, thinFundamentals, harshUpperMids,
    fizzyHighs, boomyLows, buriedPickAttack, excessiveStringNoise, inconsistentLevel,
    overcompressed, weakTransient, excessiveSustain, gateChopping, bassLowEndPumping,
    bassFundamentalLoss, cleanBlendPhaseCancellation, cabinetMismatch, duplicatedCabinet,
    stereoPhaseProblem, excessiveLatency, goalAdjustment
};

struct AudioSummaryFrame
{
    float inputPeak {}, inputRms {}, outputPeak {}, outputRms {};
    float zeroCrossingRate {};
    float channelCorrelation { 1.0f };
    std::uint32_t inputClipped {}, outputClipped {}, samples {};
    int latencySamples {};
    double sampleRate { 48000.0 };
};

class SummaryQueue
{
public:
    bool push(const AudioSummaryFrame& frame) noexcept { return queue.push(frame); }
    bool pop(AudioSummaryFrame& frame) noexcept { return queue.pop(frame); }
private:
    nts::diagnostics::SpscRingBuffer<AudioSummaryFrame, 512> queue;
};

struct SignalObservation
{
    float inputPeak {}, inputRms {}, outputPeak {}, outputRms {};
    float inputCrestDb {}, outputCrestDb {}, noiseFloorRms { 1.0f };
    float zeroCrossingRate {}, channelCorrelation { 1.0f }, intermittentRatio {};
    float inputClipRate {}, outputClipRate {};
    int latencySamples {};
    double sampleRate { 48000.0 };
    std::size_t frames {};
};

class SummaryAccumulator
{
public:
    void add(const AudioSummaryFrame& frame) noexcept;
    [[nodiscard]] SignalObservation observation() const noexcept;
    void reset() noexcept;
private:
    SignalObservation state;
    float previousInputRms {};
};

struct ParameterValue { std::string id; float value {}; bool operator==(const ParameterValue&) const = default; };
struct ParameterRange { std::string_view id; float minimum {}, maximum {}, maximumPreviewDelta {}; };
[[nodiscard]] std::span<const ParameterRange> parameterSchema() noexcept;

struct ParameterChange
{
    std::string parameter;
    float from {}, to {};
};

struct Recommendation
{
    std::string actionId;
    Problem problem { Problem::goalAdjustment };
    std::string diagnosis;
    std::string beginnerExplanation;
    std::string advancedExplanation;
    float confidence {};
    std::vector<std::string> evidence;
    std::vector<ParameterChange> changes;
    std::string expectedEffect;
    bool measurableProblem { true };
};

struct PreferenceProfile
{
    bool personalizationEnabled { true };
    nts::tone::Instrument preferredInstrument { nts::tone::Instrument::guitar };
    float preferredGain { 0.5f };
    float preferredBrightness { 0.5f };
    float preferredCleanBlend { 0.5f };
    std::vector<std::string> frequentlyUsedCabinets;
    std::vector<std::string> acceptedActions;
    std::vector<std::string> rejectedActions;
};

struct AssistantInput
{
    SignalObservation signal;
    nts::amp::AmpParameters rig;
    std::vector<ParameterValue> parameters;
    std::optional<nts::tone::ToneAnalysisResult> tone;
    std::optional<nts::tone::ToneAnalysisResult> reference;
    Goal goal { Goal::diagnose };
};

class RecommendationEngine
{
public:
    [[nodiscard]] std::vector<Recommendation> evaluate(const AssistantInput& input,
                                                       const PreferenceProfile& preferences,
                                                       std::size_t maximumResults = 6) const;
};

struct PreviewState
{
    std::string actionId;
    std::vector<ParameterValue> before;
    std::vector<ParameterValue> after;
};

class SafeActionSession
{
public:
    [[nodiscard]] bool preview(const Recommendation& recommendation,
                               std::span<const ParameterValue> current,
                               std::string& error);
    [[nodiscard]] bool accept(std::vector<ParameterValue>& current, PreferenceProfile& preferences,
                              std::string& error);
    [[nodiscard]] bool reject(std::vector<ParameterValue>& current, PreferenceProfile& preferences,
                              std::string& error);
    [[nodiscard]] bool undo(std::vector<ParameterValue>& current, std::string& error);
    [[nodiscard]] bool hasPreview() const noexcept { return activePreview.has_value(); }
    [[nodiscard]] const std::optional<PreviewState>& previewState() const noexcept { return activePreview; }
    void clearHistory() noexcept { history.clear(); activePreview.reset(); }
private:
    std::optional<PreviewState> activePreview;
    std::vector<std::vector<ParameterValue>> history;
};

[[nodiscard]] std::string serializePreferences(const PreferenceProfile& profile, bool pretty = true);
[[nodiscard]] std::optional<PreferenceProfile> deserializePreferences(std::string_view json,
                                                                     std::string& error);
[[nodiscard]] std::string serializeRecommendation(const Recommendation& recommendation,
                                                  bool pretty = true);
[[nodiscard]] std::string_view toString(Goal value) noexcept;
[[nodiscard]] std::string_view toString(Problem value) noexcept;
} // namespace nts::assistant
