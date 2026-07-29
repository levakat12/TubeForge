#include "TestHarness.h"

#include <nts/tone/ToneProfileDatabase.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace
{
std::vector<float> render(std::size_t samples, double sampleRate, double frequency,
                          float level, float drive, float decay = 0.0f)
{
    std::vector<float> result(samples);
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto time = static_cast<double>(index) / sampleRate;
        const auto envelope = decay > 0.0f ? std::exp(-static_cast<float>(time) * decay) : 1.0f;
        const auto fundamental = std::sin(2.0 * std::numbers::pi * frequency * time);
        const auto pick = 0.12 * std::sin(2.0 * std::numbers::pi * frequency * 5.0 * time)
                        * std::exp(-static_cast<float>(std::fmod(time, 0.25)) * 35.0f);
        result[index] = std::tanh(static_cast<float>(fundamental + pick) * drive) * level * envelope;
    }
    return result;
}

nts::tone::ToneProfile profile(std::string id, const nts::tone::ToneAnalysisResult& analysis)
{
    return { std::move(id), "Profile", analysis.embedding, analysis.features, analysis.report,
             analysis.report.context.instrument, nts::tone::SourceType::pluginRender,
             analysis.report.confidence.aggregate, analysis.analysisVersion, { "test" }, "synthetic-test-data" };
}
}

int main()
{
    TestHarness tests;
    using namespace nts::tone;
    ToneAnalyzer analyzer;
    constexpr double sampleRate = 16000.0;
    const auto cleanA = render(static_cast<std::size_t>(sampleRate * 5.0), sampleRate, 110.0, 0.45f, 0.8f);
    const auto cleanB = render(static_cast<std::size_t>(sampleRate * 5.0), sampleRate, 220.0, 0.45f, 0.8f);
    const auto distorted = render(static_cast<std::size_t>(sampleRate * 5.0), sampleRate, 110.0, 0.45f, 7.0f);
    const auto cleanResultA = analyzer.analyze({ cleanA, {}, sampleRate, SourceType::pluginRender, 1.0f });
    const auto cleanResultB = analyzer.analyze({ cleanB, {}, sampleRate, SourceType::pluginRender, 1.0f });
    const auto distortedResult = analyzer.analyze({ distorted, {}, sampleRate, SourceType::pluginRender, 1.0f });
    tests.expect(cleanResultA.success && cleanResultB.success && distortedResult.success,
                 "five-second clips produce tone analyses");
    tests.expectEqual(cleanResultA.embedding.version, std::string(currentEmbeddingVersion),
                      "embedding carries a stable version");
    const auto sameRig = ToneSimilarity::compare(profile("a", cleanResultA), profile("b", cleanResultB));
    const auto differentRig = ToneSimilarity::compare(profile("a", cleanResultA), profile("c", distortedResult));
    tests.expect(sameRig.score > differentRig.score,
                 "same-rig pitch variation is closer than a different nonlinear rig");

    auto quiet = cleanA; for (auto& sample : quiet) sample *= 0.1f;
    const auto quietResult = analyzer.analyze({ quiet, {}, sampleRate, SourceType::pluginRender, 1.0f });
    const auto loudnessInvariant = ToneSimilarity::compare(profile("a", cleanResultA), profile("q", quietResult));
    tests.expect(loudnessInvariant.score > 0.995f, "tone similarity is robust to loudness changes");

    const auto shortClip = render(static_cast<std::size_t>(sampleRate * 0.5), sampleRate, 110.0, 0.45f, 0.8f);
    const auto shortResult = analyzer.analyze({ shortClip, {}, sampleRate, SourceType::userRecording, 1.0f });
    tests.expect(shortResult.success && shortResult.report.confidence.aggregate < cleanResultA.report.confidence.aggregate,
                 "short clips lower confidence and still return bounded reports");
    tests.expect(!shortResult.report.warnings.empty(), "short clip emits an uncertainty warning");

    std::vector<float> silence(static_cast<std::size_t>(sampleRate));
    tests.expect(!analyzer.analyze({ silence, {}, sampleRate }).success, "silence is rejected");

    // Residual drum leakage is loud but sparse in time, so it survives an averaged
    // spectrum and dies in a per-bin median. Run at the pipeline's own rate, where
    // the analysis window is short relative to a backbeat.
    {
        constexpr double leakRate = 48000.0;
        const auto leakSamples = static_cast<std::size_t>(leakRate * 3.0);
        const auto sustained = render(leakSamples, leakRate, 110.0, 0.40f, 3.0f);
        auto leaked = sustained;
        for (std::size_t index = 0; index < leaked.size(); ++index)
        {
            const auto time = static_cast<double>(index) / leakRate;
            const auto beat = std::fmod(time, 0.5);
            if (beat < 0.03)
                leaked[index] += static_cast<float>(0.5 * std::exp(-beat * 120.0)
                    * std::sin(2.0 * std::numbers::pi * 3800.0 * time));
        }
        const auto sustainedResult = analyzer.analyze({ sustained, {}, leakRate, SourceType::isolatedStem, 1.0f });
        const auto leakedResult = analyzer.analyze({ leaked, {}, leakRate, SourceType::isolatedStem, 1.0f });
        tests.expect(sustainedResult.success && leakedResult.success, "leakage comparison clips analyze");
        tests.expect(std::abs(sustainedResult.report.brightness.value
                              - leakedResult.report.brightness.value) < 0.06f,
                     "sparse percussive leakage barely moves the brightness descriptor");
        tests.expect(std::abs(sustainedResult.features.spectral.highFrequencyRolloff
                              - leakedResult.features.spectral.highFrequencyRolloff) < 0.06f,
                     "sparse percussive leakage barely moves the high-frequency rolloff");
    }

    auto contaminatedRight = cleanA;
    for (std::size_t index = 0; index < contaminatedRight.size(); ++index)
        contaminatedRight[index] += 0.15f * static_cast<float>(std::sin(
            2.0 * std::numbers::pi * 43.0 * static_cast<double>(index) / sampleRate));
    const auto contaminated = analyzer.analyze({ cleanA, contaminatedRight, sampleRate,
                                                  SourceType::isolatedStem, 0.35f });
    tests.expect(contaminated.report.confidence.aggregate < cleanResultA.report.confidence.aggregate,
                 "contamination and separation artifacts lower confidence");
    tests.expect(contaminated.features.spatial.stereoWidth > 0.0f, "stereo production features are extracted");

    const auto thirtySeconds = render(static_cast<std::size_t>(sampleRate * 30.0), sampleRate, 82.0, 0.4f, 1.6f);
    const auto longResult = analyzer.analyze({ thirtySeconds, {}, sampleRate, SourceType::physicalAmp, 1.0f });
    tests.expect(longResult.success && longResult.features.durationSeconds > 29.9f,
                 "thirty-second product clip is analyzed offline");
    tests.expect(std::isfinite(longResult.features.spectral.spectralCentroidHz),
                 "multiresolution feature values remain finite");

    std::array<float, toneEmbeddingDimensions * toneFeatureVectorDimensions> learnedWeights {};
    for (std::size_t index = 0; index < learnedWeights.size(); ++index)
        learnedWeights[index] = std::sin(static_cast<float>(index) * 0.013f);
    std::string error;
    tests.expect(analyzer.encoder().loadProjection(learnedWeights, "learned-test-v2", error),
                 "learned projection weights load with an explicit version: " + error);
    const auto learnedResult = analyzer.analyze({ cleanA, {}, sampleRate });
    tests.expectEqual(learnedResult.embedding.version, std::string { "learned-test-v2" },
                      "loaded learned encoder version propagates to embeddings");

    ToneProfileDatabase database;
    tests.expect(database.addOrReplace(profile("clean-a", cleanResultA), error), "profile is added: " + error);
    tests.expect(database.addOrReplace(profile("clean-b", cleanResultB), error), "second profile is added: " + error);
    tests.expect(database.addOrReplace(profile("driven", distortedResult), error), "different profile is added: " + error);
    const auto nearest = database.search(profile("query", cleanResultA), 2);
    tests.expect(nearest.size() == 2 && nearest.front().profileId == "clean-a",
                 "exact nearest-profile search returns the closest rig");
    const auto databaseJson = database.serialize();
    ToneProfileDatabase restored;
    tests.expect(restored.deserialize(databaseJson, error), "tone profile database restores: " + error);
    tests.expectEqual(restored.size(), database.size(), "profile database preserves every record");
    const auto restoredSearch = restored.search(profile("query", cleanResultA), 1);
    tests.expect(!restoredSearch.empty() && restoredSearch.front().similarity.score > 0.99f,
                 "restored embeddings and descriptive features remain searchable");

    const auto report = serializeToneReport(cleanResultA);
    tests.expect(report.find("uncertainty") != std::string::npos
                 && report.find("instrumentConfidence") != std::string::npos,
                 "interpretable JSON report includes uncertainty and classifier confidence");
    return tests.result();
}
