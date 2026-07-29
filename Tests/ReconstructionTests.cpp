#include "TestHarness.h"

#include <nts/reconstruction/SourceReconstruction.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <string>
#include <vector>

namespace
{
nts::reconstruction::StereoAudio makeSong(double sampleRate, double seconds, bool stereo = true)
{
    nts::reconstruction::StereoAudio song;
    song.sampleRate = sampleRate;
    const auto samples = static_cast<std::size_t>(sampleRate * seconds);
    song.left.resize(samples); if (stereo) song.right.resize(samples);
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto time = static_cast<double>(index) / sampleRate;
        const auto bass = 0.34 * std::sin(2.0 * std::numbers::pi * 82.0 * time);
        const auto guitar = 0.22 * std::tanh(2.5 * std::sin(2.0 * std::numbers::pi * 330.0 * time));
        const auto vocal = 0.10 * std::sin(2.0 * std::numbers::pi * 720.0 * time);
        const auto beatPhase = std::fmod(time, 0.25);
        const auto drums = 0.28 * std::exp(-beatPhase * 75.0) * std::sin(2.0 * std::numbers::pi * 3100.0 * time);
        song.left[index] = static_cast<float>(bass + guitar + vocal + drums);
        if (stereo) song.right[index] = static_cast<float>(bass + guitar * 0.84 - vocal * 0.06 + drums * 0.9);
    }
    return song;
}

double energy(const std::vector<float>& samples)
{
    return std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
}
}

int main()
{
    TestHarness tests;
    using namespace nts::reconstruction;
    constexpr double sampleRate = 16000.0;
    const auto song = makeSong(sampleRate, 8.0);
    StemSeparator separator;
    std::vector<float> progress;
    SeparationOptions options; options.chunkSamples = 8192; options.overlapSamples = 2048;
    const auto stems = separator.separate(song, options, [&progress](const SeparationProgress& state)
        { progress.push_back(state.fraction); return true; });
    tests.expect(stems.success, "stereo song separates with the CPU fallback: " + stems.error);
    tests.expect(stems.reconstructionError < 1.0e-5f, "four stems reconstruct the input consistently");
    tests.expect(! stems.cacheKey.empty() && stems.modelVersion == currentSeparationVersion,
                 "separation has a deterministic cache key and model version");
    tests.expect(! progress.empty() && progress.back() >= 0.99f, "separation publishes progress");
    const auto second = separator.separate(song, options);
    tests.expectEqual(second.cacheKey, stems.cacheKey, "identical source and model options reuse the cache identity");
    tests.expect(energy(stems.bass.left) > 0.05 * energy(song.left), "bass-heavy mixture produces a non-empty bass stem");

    auto cancelled = separator.separate(song, options, [](const SeparationProgress&) { return false; });
    tests.expect(! cancelled.success && cancelled.error.find("cancelled") != std::string::npos,
                 "separation can be cancelled between overlap chunks");
    auto gpuOptions = options; gpuOptions.preferGpu = true;
    const auto cpuFallback = separator.separate(song, gpuOptions);
    tests.expect(cpuFallback.success && ! cpuFallback.usedGpu && ! cpuFallback.warnings.empty(),
                 "optional GPU request has an explicit deterministic CPU fallback");

    const auto regions = scoreRegions(stems, TargetInstrument::bass, 3.0, 1.0);
    const auto recommended = recommendRegion(stems, TargetInstrument::bass, 3.0);
    tests.expect(regions.size() > 2 && recommended.duration >= 2.9f && recommended.confidence > 0.0f,
                 "region scorer recommends a bounded bass reference with confidence");
    const auto guitar = selectStem(stems, TargetInstrument::guitar);
    tests.expect(guitar.samples() == song.samples(), "guitar-supported subset is extracted from the other stem");
    auto neuralStems = stems;
    neuralStems.guitar = makeSong(sampleRate, 1.0);
    const auto directGuitar = selectStem(neuralStems, TargetInstrument::guitar);
    tests.expect(directGuitar.left == neuralStems.guitar.left,
                 "a direct neural guitar stem takes priority over subtractive other-stem extraction");
    StemSet sparseNeuralStems;
    sparseNeuralStems.success = true;
    sparseNeuralStems.modelVersion = "demucs-test";
    sparseNeuralStems.guitar = makeSong(sampleRate, 3.0);
    sparseNeuralStems.vocals = sparseNeuralStems.guitar;
    sparseNeuralStems.drums = sparseNeuralStems.guitar;
    sparseNeuralStems.bass = {};
    sparseNeuralStems.other = {};
    const auto sparseRegions = scoreRegions(sparseNeuralStems, TargetInstrument::guitar, 2.0, 1.0);
    tests.expect(! sparseRegions.empty(),
                 "region scoring accepts a target-only neural stem set without indexing absent stems");
    const auto mid = applyStereoMode(guitar, StereoMode::mid);
    const auto side = applyStereoMode(guitar, StereoMode::side);
    const auto panned = applyStereoMode(guitar, StereoMode::pannedEstimate);
    tests.expect(mid.left == mid.right && side.left == side.right && panned.left == panned.right,
                 "mid, side, and panned-source stereo modes produce stable mono analysis views");

    StereoAudio cleanPitch;
    cleanPitch.sampleRate = sampleRate;
    cleanPitch.left.resize(static_cast<std::size_t>(sampleRate * 2.0));
    cleanPitch.right.resize(cleanPitch.left.size());
    StereoAudio drivenPitch = cleanPitch;
    for (std::size_t index = 0; index < cleanPitch.samples(); ++index)
    {
        const auto value = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0
            * static_cast<double>(index) / sampleRate));
        cleanPitch.left[index] = cleanPitch.right[index] = value;
        drivenPitch.left[index] = drivenPitch.right[index] = std::tanh(value * 12.0f) * 0.45f;
    }
    RegionQuality pitchQuality; pitchQuality.startSeconds = 0.0; pitchQuality.endSeconds = 2.0;
    pitchQuality.duration = 2.0f; pitchQuality.confidence = 0.9f;
    const std::array pitchQualities { pitchQuality };
    const auto cleanParts = analyzePlayableRegions(cleanPitch, pitchQualities, TargetInstrument::guitar);
    const auto drivenParts = analyzePlayableRegions(drivenPitch, pitchQualities, TargetInstrument::guitar);
    tests.expect(! cleanParts.empty() && cleanParts.front().dominantPitch == "A3"
                 && cleanParts.front().pitchConfidence > 0.4f,
                 "playable-region analysis detects dominant pitch with confidence");
    tests.expect(! drivenParts.empty() && cleanParts.front().gainCharacter == GainCharacter::clean
                 && drivenParts.front().gainCharacter != GainCharacter::clean,
                 "playable-region analysis distinguishes clean and driven material");

    {
        // Gain classification must not depend on how long the region is. The
        // analyser used to sub-sample regions longer than five seconds, which
        // aliased away the high-frequency content that identifies distortion and
        // made long high-gain parts read as crunch or clean. The two-second case
        // above never crossed that threshold, so it could not catch it.
        const auto drivenRegionOfLength = [&](double seconds)
        {
            StereoAudio audio;
            audio.sampleRate = sampleRate;
            audio.left.resize(static_cast<std::size_t>(sampleRate * seconds));
            audio.right.resize(audio.left.size());
            for (std::size_t index = 0; index < audio.samples(); ++index)
            {
                const auto value = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0
                    * static_cast<double>(index) / sampleRate));
                audio.left[index] = audio.right[index] = std::tanh(value * 12.0f) * 0.45f;
            }
            RegionQuality quality; quality.startSeconds = 0.0; quality.endSeconds = seconds;
            quality.duration = static_cast<float>(seconds); quality.confidence = 0.9f;
            const std::array qualities { quality };
            return analyzePlayableRegions(audio, qualities, TargetInstrument::guitar);
        };

        // 30 seconds at this fixture's 16 kHz rate is 480000 samples, comfortably
        // past the 240000-sample budget where the sub-sampling used to begin.
        const auto shortRegion = drivenRegionOfLength(2.0);
        const auto longRegion = drivenRegionOfLength(30.0);
        tests.expect(! shortRegion.empty() && ! longRegion.empty(),
                     "gain classification produces a region at both lengths");
        if (! shortRegion.empty() && ! longRegion.empty())
        {
            tests.expect(longRegion.front().gainCharacter != GainCharacter::clean,
                         "a long driven region is not classified as clean");
            tests.expectNear(longRegion.front().gainScore, shortRegion.front().gainScore, 0.1,
                             "gain score is stable across region length");
        }
    }

    const auto monoSong = makeSong(sampleRate, 2.0, false);
    const auto monoStems = separator.separate(monoSong, options);
    tests.expect(monoStems.success && monoStems.bass.left.size() == monoStems.bass.right.size(),
                 "mono recordings are accepted and promoted consistently");
    auto clipped = song; for (auto& sample : clipped.left) sample = std::clamp(sample * 8.0f, -1.0f, 1.0f);
    const auto clippedStems = separator.separate(clipped, options);
    const auto clippedQuality = recommendRegion(clippedStems, TargetInstrument::guitar, 3.0);
    tests.expect(clippedQuality.clipping > 0.0f && ! clippedQuality.warnings.empty(),
                 "clipping and ambiguous references lower certainty instead of looking exact");

    auto region = extractRegion(guitar, recommended.startSeconds, recommended.endSeconds);
    auto normalized = normalizeReference(region, true);
    tests.expect(! normalized.audio.left.empty() && normalized.roomTailReduced
                 && std::isfinite(normalized.originalLoudnessDb),
                 "reference normalization removes DC/silence, estimates production EQ, and optionally reduces room tail");
    nts::tone::ToneAnalyzer analyzer;
    const auto tone = analyzer.analyze({ normalized.audio.left, normalized.audio.right, sampleRate,
                                         nts::tone::SourceType::isolatedStem, recommended.confidence });
    tests.expect(tone.success, "selected normalized region feeds the Phase 7 tone analyzer");

    std::vector<float> di(static_cast<std::size_t>(sampleRate * 1.25));
    for (std::size_t index = 0; index < di.size(); ++index)
        di[index] = 0.18f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 165.0
                                                       * static_cast<double>(index) / sampleRate));
    ReconstructionReference reference;
    reference.sourceHash = stems.cacheKey;
    reference.regionStartSeconds = recommended.startSeconds;
    reference.regionEndSeconds = recommended.endSeconds;
    reference.separationModelVersion = stems.modelVersion;
    reference.analysisConfidence = tone.report.confidence.aggregate;
    reference.target = TargetInstrument::guitar;
    reference.tone = tone;
    reference.quality = recommended;
    RigReconstructor reconstructor;
    const auto reconstruction = reconstructor.reconstruct(reference, di, sampleRate, 3);
    tests.expect(reconstruction.success && reconstruction.candidates.size() == 3,
                 "staged rig search returns several playable candidates: " + reconstruction.error);
    tests.expect(reconstruction.candidates.front().toneSimilarity >= 0.0f
                 && reconstruction.candidates.front().recordingSimilarity >= 0.0f,
                 "candidate ranking separates tone and full-recording similarity");
    tests.expect(reconstruction.candidates.front().rigPreset.parameters.stageCount >= 2,
                 "candidate contains an editable traditional-amp preset");
    const auto serialized = serializeResult(reconstruction);
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(serialized.c_str()));
    tests.expect(serialized.find("rigPreset") != std::string::npos
                 && serialized.find("sourceHash") != std::string::npos
                 && serialized.find("audioSamples") == std::string::npos && parsed.isObject(),
                 "shared reconstruction package includes parameters and provenance but no source audio");
    tests.expect(! reconstructor.reconstruct(reference, {}, sampleRate).success,
                 "missing user DI is rejected instead of fabricating a playable result");
    return tests.result();
}
