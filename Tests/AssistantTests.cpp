#include "TestHarness.h"

#include <nts/assistant/ToneAssistant.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
std::vector<nts::assistant::ParameterValue> neutralParameters()
{
    std::vector<nts::assistant::ParameterValue> values;
    for (const auto& range : nts::assistant::parameterSchema())
        values.push_back({ std::string(range.id), (range.minimum + range.maximum) * 0.5f });
    const auto set = [&values](std::string_view id, float value)
    {
        const auto found = std::find_if(values.begin(), values.end(), [id](const auto& item) { return item.id == id; });
        if (found != values.end()) found->value = value;
    };
    set("input", 0.0f); set("output", 0.0f); set("gain", 5.0f); set("lowCut", 80.0f);
    set("highCut", 12000.0f); set("cleanBlend", 50.0f); set("cabinetAlignment", 32.0f);
    return values;
}

nts::tone::ToneAnalysisResult neutralTone(nts::tone::Instrument instrument = nts::tone::Instrument::guitar)
{
    nts::tone::ToneAnalysisResult tone; tone.success = true;
    tone.report.context.instrument = instrument; tone.report.confidence.aggregate = 0.9f;
    tone.report.gain.value = 0.45f; tone.report.brightness.value = 0.5f;
    tone.report.compression.value = 0.4f; tone.report.cleanLowBlendEstimate.value = 0.5f;
    tone.features.spectral.lowFrequencyExtension = 0.5f; tone.features.spectral.lowMidBuildup = 0.45f;
    tone.features.spectral.spectralCentroidHz = 2400.0f; tone.features.spectral.upperMidAttack = 0.45f;
    tone.features.spectral.highFrequencyRolloff = 0.5f; tone.features.dynamic.transientPreservation = 0.65f;
    tone.features.dynamic.crestFactorDb = 11.0f; tone.features.spatial.channelCorrelation = 0.9f;
    return tone;
}

bool hasProblem(const std::vector<nts::assistant::Recommendation>& recommendations,
                nts::assistant::Problem problem)
{
    return std::any_of(recommendations.begin(), recommendations.end(), [problem](const auto& item)
        { return item.problem == problem; });
}
}

int main()
{
    TestHarness tests;
    using namespace nts::assistant;
    RecommendationEngine engine;
    PreferenceProfile preferences;
    AssistantInput input; input.parameters = neutralParameters(); input.tone = neutralTone();
    input.signal.frames = 20; input.signal.inputRms = 0.12f; input.signal.inputPeak = 0.5f;
    input.signal.outputRms = 0.15f; input.signal.outputPeak = 0.6f; input.signal.sampleRate = 48000.0;
    auto clean = engine.evaluate(input, preferences);
    tests.expect(clean.empty(), "neutral synthetic rig does not trigger false-positive technical problems");

    input.signal.inputPeak = 1.0f; input.signal.inputClipRate = 0.01f;
    auto clipping = engine.evaluate(input, preferences);
    tests.expect(hasProblem(clipping, Problem::inputClipping), "labelled clipping input is diagnosed");
    input.signal.inputPeak = 0.5f; input.signal.inputClipRate = 0.0f;

    input.tone = neutralTone(); input.tone->features.spectral.lowMidBuildup = 0.88f;
    input.tone->features.spectral.spectralCentroidHz = 1100.0f;
    auto muddy = engine.evaluate(input, preferences);
    tests.expect(hasProblem(muddy, Problem::muddyLowMids), "intentionally muddy guitar is diagnosed");
    const auto mud = std::find_if(muddy.begin(), muddy.end(), [](const auto& item) { return item.problem == Problem::muddyLowMids; });
    tests.expect(mud != muddy.end() && ! mud->evidence.empty() && ! mud->beginnerExplanation.empty()
                 && ! mud->advancedExplanation.empty() && mud->confidence > 0.5f,
                 "every suggestion carries evidence, confidence, and consistent beginner/advanced explanations");
    tests.expect(std::any_of(mud->changes.begin(), mud->changes.end(), [](const auto& item)
        { return item.parameter == "lowCut" && item.to > item.from; }),
        "mud recommendation measurably moves the pre-distortion cutoff in the corrective direction");

    input.tone = neutralTone();
    for (auto& parameter : input.parameters) if (parameter.id == "gain") parameter.value = 9.2f;
    tests.expect(hasProblem(engine.evaluate(input, preferences), Problem::excessivePreampGain),
                 "too-much-gain scenario is diagnosed");
    input.parameters = neutralParameters();

    input.tone = neutralTone(nts::tone::Instrument::bass);
    input.tone->features.spectral.lowFrequencyExtension = 0.1f;
    tests.expect(hasProblem(engine.evaluate(input, preferences), Problem::thinFundamentals),
                 "weak bass fundamental scenario is diagnosed");
    input.tone = neutralTone(nts::tone::Instrument::bass);
    input.tone->report.compression.value = 0.91f;
    tests.expect(hasProblem(engine.evaluate(input, preferences), Problem::overcompressed),
                 "overcompressed bass scenario is diagnosed");
    input.tone = neutralTone(nts::tone::Instrument::bass);
    input.tone->features.spatial.channelCorrelation = -0.3f;
    auto phase = engine.evaluate(input, preferences);
    tests.expect(hasProblem(phase, Problem::cleanBlendPhaseCancellation)
                 && hasProblem(phase, Problem::stereoPhaseProblem),
                 "phase-cancelled bass clean blend produces routing evidence");
    input.tone = neutralTone(); input.tone->features.spectral.upperMidAttack = 0.9f;
    tests.expect(hasProblem(engine.evaluate(input, preferences), Problem::harshUpperMids),
                 "harsh cabinet/upper-mid scenario is diagnosed");
    input.tone = neutralTone(); input.rig.gateThresholdDb = -24.0f; input.signal.inputCrestDb = 15.0f;
    tests.expect(hasProblem(engine.evaluate(input, preferences), Problem::gateChopping),
                 "aggressive gate scenario is diagnosed");

    input.rig.gateThresholdDb = -58.0f; input.signal.inputCrestDb = 8.0f;
    input.goal = Goal::tightRhythm;
    auto goal = engine.evaluate(input, preferences);
    tests.expect(hasProblem(goal, Problem::goalAdjustment), "musician goal maps to constrained parameter macros");

    SafeActionSession session; std::string error; auto current = neutralParameters();
    tests.expect(session.preview(goal.front(), current, error), "valid recommendation creates a temporary preview: " + error);
    const auto exactBefore = current;
    auto previewValues = session.previewState()->after;
    tests.expect(previewValues != exactBefore, "preview state differs without mutating the committed state");
    tests.expect(session.reject(current, preferences, error) && current == exactBefore,
                 "rejection restores the exact previous state: " + error);
    tests.expect(session.preview(goal.front(), current, error), "recommendation can be previewed again");
    current = session.previewState()->after;
    tests.expect(session.accept(current, preferences, error), "accepted preview enters undo history: " + error);
    tests.expect(current != exactBefore && session.undo(current, error) && current == exactBefore,
                 "undo restores the exact committed state");

    auto unsafe = goal.front(); unsafe.changes = { { "gain", 5.0f, 1000.0f } };
    tests.expect(! session.preview(unsafe, exactBefore, error), "out-of-range assistant action is rejected by schema validation");
    unsafe.changes = { { "arbitraryGraphState", 0.0f, 1.0f } };
    tests.expect(! session.preview(unsafe, exactBefore, error), "assistant cannot mutate arbitrary graph state");

    preferences.personalizationEnabled = false; preferences.acceptedActions = { "local-only" };
    const auto json = serializePreferences(preferences);
    const auto restored = deserializePreferences(json, error);
    tests.expect(restored && ! restored->personalizationEnabled && restored->acceptedActions == preferences.acceptedActions,
                 "local preference profile round-trips and can be disabled");
    preferences = {}; tests.expect(preferences.acceptedActions.empty() && preferences.rejectedActions.empty(),
                                   "clearing preferences removes accepted/rejected history");

    SummaryQueue queue; SummaryAccumulator accumulator;
    AudioSummaryFrame frame; frame.inputPeak = 0.4f; frame.inputRms = 0.1f; frame.outputPeak = 0.5f;
    frame.outputRms = 0.12f; frame.samples = 64; frame.sampleRate = 48000.0;
    for (int index = 0; index < 200; ++index) tests.expect(queue.push(frame), "summary ring accepts bounded audio frame");
    AudioSummaryFrame popped; while (queue.pop(popped)) accumulator.add(popped);
    tests.expect(accumulator.observation().frames == 200 && accumulator.observation().inputRms > 0.0f,
                 "off-thread accumulator consumes downsampled summary frames");
    return tests.result();
}
