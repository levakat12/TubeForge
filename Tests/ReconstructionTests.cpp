#include "TestHarness.h"

#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/reconstruction/StemRefinement.h>
#include <nts/ml/PackedFeatureRegressor.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <cstdio>
#include <numbers>
#include <numeric>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace
{
/** A packed feature regressor whose answer is known in advance.

    Built here rather than loaded from an artifact so the test has no fixture to keep in step and
    no dependency on a trained model existing. Every hidden weight is zero, so each unit sits at
    `tanh(bias)`; with the biases below that is a constant, and the output is a sigmoid of a value
    this test picks. The point is not to check arithmetic -- `production_parity_driver.py` does
    that against the Python model -- but to check the *wiring*: that a loaded model reaches the
    four amounts, that it does not touch anything else, and that the flag says so.
*/
std::vector<std::byte> constantRegressor(std::array<float, 4> wanted)
{
    constexpr std::uint32_t inputs = nts::reconstruction::productionFeatureCount;
    constexpr std::uint32_t hidden = 2, outputs = 4;
    std::vector<std::byte> bytes { std::byte{'N'}, std::byte{'T'}, std::byte{'S'}, std::byte{'M'} };
    const auto appendU32 = [&bytes](std::uint32_t value)
    {
        const auto offset = bytes.size(); bytes.resize(offset + sizeof(value));
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    const auto appendFloat = [&bytes](float value)
    {
        const auto offset = bytes.size(); bytes.resize(offset + sizeof(value));
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    appendU32(4); appendU32(6); appendU32(inputs); appendU32(hidden); appendU32(outputs);
    for (std::uint32_t index = 0; index < inputs; ++index) appendFloat(0.0f);   // mean
    for (std::uint32_t index = 0; index < inputs; ++index) appendFloat(1.0f);   // scale
    for (std::uint32_t index = 0; index < inputs * hidden; ++index) appendFloat(0.0f);
    for (std::uint32_t index = 0; index < hidden; ++index) appendFloat(0.0f);   // hidden bias
    for (std::uint32_t index = 0; index < hidden * outputs; ++index) appendFloat(0.0f);
    // tanh(0) is 0, so every hidden activation vanishes and the output is sigmoid(bias). Inverting
    // the sigmoid gives the bias that produces the value this test wants back.
    for (const auto value : wanted)
    {
        const auto clamped = std::clamp(value, 0.02f, 0.98f);
        appendFloat(std::log(clamped / (1.0f - clamped)));
    }
    return bytes;
}

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

/** A driven guitar, as opposed to a clipped sine.

    **The distinction is not pedantry; it is why the tests below changed.** Gain character is now
    measured from harmonic structure, and on that measure a softly clipped single sine is a very
    mild signal: `tanh(3.6*sin)` keeps about 4% of its energy above the second partial, because a
    near-square wave is still dominated by its fundamental. A real clean guitar recording measures
    *richer* than that -- around 15% -- once room, bleed and the other strings are in it. So no
    threshold exists that calls a clipped sine driven and a clean recording clean, and the previous
    measure only appeared to manage it by keying on brightness, which is what `tanh` adds and what
    a cabinet takes away.

    What actually makes a distorted guitar spectrally dense is polyphony: a chord through a clipper
    intermodulates, and the products land across the harmonic series. So the driven fixture is a
    power chord, plucked and hard-clipped, which is what the analyser meets in the wild.
*/
nts::reconstruction::StereoAudio drivenGuitar(double sampleRate, double seconds)
{
    nts::reconstruction::StereoAudio audio;
    audio.sampleRate = sampleRate;
    audio.left.resize(static_cast<std::size_t>(sampleRate * seconds));
    audio.right.resize(audio.left.size());
    // Root and fifth, the interval a rhythm guitar is nearly always playing.
    constexpr double root = 110.0;
    constexpr double fifth = 164.81;
    for (std::size_t index = 0; index < audio.samples(); ++index)
    {
        const auto time = static_cast<double>(index) / sampleRate;
        const auto envelope = std::exp(-std::fmod(time, 0.55) * 3.0);
        double value = 0.0;
        for (int partial = 1; partial <= 6; ++partial)
        {
            const auto weight = 1.0 / (partial * partial);
            value += weight * std::sin(2.0 * std::numbers::pi * root * partial * time);
            value += weight * 0.8 * std::sin(2.0 * std::numbers::pi * fifth * partial * time);
        }
        // Hard clipping, well into it: this is a rhythm tone, not an edge-of-breakup one.
        audio.left[index] = audio.right[index] =
            static_cast<float>(std::clamp(value * envelope * 6.0, -1.0, 1.0) * 0.5);
    }
    return audio;
}

double energy(const std::vector<float>& samples)
{
    return std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
}
}


/** The whole search, end to end, on references that differ only in how hard they are driven.

    **This is the one test that would have caught "it always falls to British Crunch".** Every
    other assertion here is about a table or a list, and all of them passed while the search was
    returning the same amplifier for every song -- because the defect was not in the table. It was
    that the search was fed `tone.report.context.gainCategory`, whose gain axis puts a pure
    undistorted sine at 0.475 against a `clean` threshold of 0.16, so it answered "crunch" for
    nearly everything; and that the coarse pass then spent its entire refinement budget on the one
    voicing that wins the crunch column.

    Asserted as a *family* rather than as a named voicing, deliberately. Which of Studio Direct and
    American Clean wins a clean reference depends on the reference, and pinning one of them would
    make this a change-detector for the synthetic signal below. What must never happen again is a
    clean part coming back as a high-gain amplifier.
*/
void testGainDrivesTopologyFamily(TestHarness& tests)
{
    using namespace nts::reconstruction;
    constexpr auto rate = 48000.0;
    const auto samples = static_cast<std::size_t>(rate * 6.0);

    // Harmonically rich, because that is what the character estimator measures: it reads
    // adjacent-sample slew, and a clipped pure sine stays fundamental-dominant and reads clean
    // however hard it is driven. A real instrument is not a sine.
    const auto render = [&](double drive)
    {
        StereoAudio audio; audio.sampleRate = rate;
        audio.left.resize(samples); audio.right.resize(samples);
        for (std::size_t index = 0; index < samples; ++index)
        {
            const auto time = static_cast<double>(index) / rate;
            const auto envelope = std::exp(-std::fmod(time, 0.5) * 3.0);
            /* Partials at 1/k^2 -- a pickup, not a sawtooth.

               This generator used 1/k over sixteen partials, which is very nearly a saw and is
               therefore already as harmonically rich as clipping could make it. Driving it changed
               the measured richness barely at all, so the "driven scores above clean" assertion
               compared two rich signals and failed. A fixture for a distortion measure has to start
               clean, and this is the third place in this file that had to learn it. */
            double note = 0.0;
            for (int partial = 1; partial <= 8; ++partial)
                note += (1.0 / (partial * partial))
                      * std::sin(2.0 * std::numbers::pi * 196.0 * partial * time);
            note *= envelope * 0.9;
            audio.left[index] = static_cast<float>(std::clamp(note * drive, -1.0, 1.0) * 0.5);
            audio.right[index] = audio.left[index];
        }
        return audio;
    };

    std::vector<float> di(samples);
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto time = static_cast<double>(index) / rate;
        di[index] = static_cast<float>(std::exp(-std::fmod(time, 0.5) * 3.0) * 0.4
                                       * std::sin(2.0 * std::numbers::pi * 196.0 * time));
    }

    const auto winningVoicing = [&](double drive, GainCharacter& measured, float& score) -> std::optional<nts::amp::Topology>
    {
        const auto audio = render(drive);
        RegionQuality whole; whole.startSeconds = 0.0; whole.endSeconds = 6.0;
        whole.confidence = 0.9f; whole.targetEnergy = 0.5f;
        const auto regions = analyzePlayableRegions(audio, std::vector<RegionQuality> { whole },
                                                    TargetInstrument::guitar);
        if (regions.empty()) return std::nullopt;
        nts::tone::ToneAnalyzer analyzer;
        ReconstructionReference reference;
        reference.target = TargetInstrument::guitar;
        reference.analysisConfidence = 0.8f;
        reference.tone = analyzer.analyze({ audio.left, audio.right, rate,
                                            nts::tone::SourceType::pluginRender, 1.0f });
        reference.gainCharacter = regions.front().gainCharacter;
        measured = reference.gainCharacter;
        score = regions.front().gainScore;
        RigReconstructor reconstructor;
        const auto result = reconstructor.reconstruct(reference, di, rate, 3);
        if (! result.success || result.candidates.empty()) return std::nullopt;
        return result.candidates.front().rigPreset.parameters.topology;
    };

    auto cleanCharacter = GainCharacter::clean;
    auto cleanScore = 0.0f;
    const auto cleanWinner = winningVoicing(0.6, cleanCharacter, cleanScore);
    tests.expect(cleanWinner.has_value(), "an undriven reference produces a candidate");
    tests.expectEqual(static_cast<int>(cleanCharacter), static_cast<int>(GainCharacter::clean),
                      "an undriven reference measures as clean");
    if (cleanWinner)
    {
        // The complaint, stated as an assertion: a clean part must not come back high-gain.
        tests.expect(*cleanWinner != nts::amp::Topology::tightModern
                     && *cleanWinner != nts::amp::Topology::saggingRectifier,
                     "a clean reference does not return a high-gain voicing");
        tests.expect(topologyGainAffinity(*cleanWinner, GainCharacter::clean) > 0.0f,
                     "a clean reference returns a voicing that suits clean material");
    }

    auto drivenCharacter = GainCharacter::clean;
    auto drivenScore = 0.0f;
    const auto drivenWinner = winningVoicing(40.0, drivenCharacter, drivenScore);
    tests.expect(drivenWinner.has_value(), "a driven reference produces a candidate");
    tests.expect(drivenScore > cleanScore + 0.05f,
                 "a driven reference scores clearly above an undriven one");
}

int main()
{
    TestHarness tests;
    testGainDrivesTopologyFamily(tests);

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

    {
        // Two known, near-orthogonal sources let the leakage in a stem be read off
        // directly as its projection onto the source it should not contain.
        constexpr double refineRate = 16000.0;
        const auto refineSamples = static_cast<std::size_t>(refineRate * 4.0);
        std::vector<float> trueGuitar(refineSamples), trueDrums(refineSamples);
        for (std::size_t index = 0; index < refineSamples; ++index)
        {
            const auto time = static_cast<double>(index) / refineRate;
            trueGuitar[index] = static_cast<float>(0.30 * std::sin(2.0 * std::numbers::pi * 220.0 * time));
            trueDrums[index] = static_cast<float>(0.45 * std::exp(-std::fmod(time, 0.5) * 90.0)
                                                  * std::sin(2.0 * std::numbers::pi * 3137.0 * time));
        }
        StereoAudio mixture; mixture.sampleRate = refineRate;
        mixture.left.resize(refineSamples); mixture.right.resize(refineSamples);
        StemSet leaky; leaky.success = true;
        leaky.guitar.sampleRate = leaky.drums.sampleRate = refineRate;
        leaky.guitar.left.resize(refineSamples); leaky.guitar.right.resize(refineSamples);
        leaky.drums.left.resize(refineSamples); leaky.drums.right.resize(refineSamples);
        for (std::size_t index = 0; index < refineSamples; ++index)
        {
            mixture.left[index] = mixture.right[index] = trueGuitar[index] + trueDrums[index];
            leaky.guitar.left[index] = leaky.guitar.right[index] = trueGuitar[index] + 0.5f * trueDrums[index];
            leaky.drums.left[index] = leaky.drums.right[index] = trueDrums[index] + 0.5f * trueGuitar[index];
        }
        const auto coefficient = [](const std::vector<float>& stem, const std::vector<float>& source)
        {
            return std::inner_product(stem.begin(), stem.end(), source.begin(), 0.0)
                / std::max(1.0e-12, energy(source));
        };
        const auto leakageOf = [&coefficient, &trueGuitar, &trueDrums](const std::vector<float>& stem)
        {
            return std::abs(coefficient(stem, trueDrums))
                / std::max(1.0e-9, std::abs(coefficient(stem, trueGuitar)));
        };
        const auto leakageBefore = leakageOf(leaky.guitar.left);
        auto refined = leaky;
        StemRefinementOptions refineOptions;
        refineOptions.fftSize = 1024; refineOptions.hopSize = 256;
        const auto refineReport = refineStems(refined, mixture, refineOptions);
        tests.expect(refineReport.applied && refineReport.stemsRefined == 2,
                     "Wiener refinement runs over every aligned stem");
        tests.expect(leakageOf(refined.guitar.left) < leakageBefore * 0.6,
                     "re-partitioning the mixture strips drum leakage out of the guitar stem");
        tests.expect(refined.guitar.left.size() == refineSamples
                     && refined.drums.right.size() == refineSamples,
                     "refinement rewrites stems in place without changing their length");
        tests.expect(refined.bass.left.empty() && refined.other.left.empty(),
                     "stems the backend never produced are not fabricated");

        StemSet singleStem; singleStem.success = true; singleStem.guitar = leaky.guitar;
        tests.expect(! refineStems(singleStem, mixture, refineOptions).applied,
                     "a lone stem has nothing to be re-partitioned against");
        std::stop_source refineStopper; refineStopper.request_stop();
        auto cancelledRefinement = leaky;
        tests.expect(! refineStems(cancelledRefinement, mixture, refineOptions, {},
                                   refineStopper.get_token()).applied,
                     "refinement observes cancellation");
    }
    const auto mid = applyStereoMode(guitar, StereoMode::mid);
    const auto side = applyStereoMode(guitar, StereoMode::side);
    const auto panned = applyStereoMode(guitar, StereoMode::pannedEstimate);
    tests.expect(mid.left == mid.right && side.left == side.right && panned.left == panned.right,
                 "mid, side, and panned-source stereo modes produce stable mono analysis views");

    {
        // A double-tracked reference has to be analysed one side at a time: the sum of two takes
        // has a crest factor and an attack time that belong to the arrangement rather than to the
        // amplifier, and the dynamics axis is fitted from exactly those two numbers.
        StereoAudio layered;
        layered.sampleRate = sampleRate;
        layered.left.resize(static_cast<std::size_t>(sampleRate * 0.5));
        layered.right.resize(layered.left.size());
        // Same note both sides, but the right channel's attacks are deliberately blunter: a
        // one-pole smoother on the transients and nothing else, so total energy stays comparable
        // and only the transient content separates the two. The recommendation must follow the
        // sharper side rather than the louder one.
        float smoothed {};
        for (std::size_t index = 0; index < layered.samples(); ++index)
        {
            const auto phase = 2.0 * std::numbers::pi * 110.0 * static_cast<double>(index) / sampleRate;
            const auto pluck = static_cast<float>(std::sin(phase))
                             * (index % 6000 < 400 ? 0.9f : 0.25f);
            layered.left[index] = pluck;
            smoothed += 0.02f * (pluck - smoothed);
            layered.right[index] = smoothed * 3.0f;
        }

        tests.expectEqual(recommendStereoMode(layered, 0.8f), StereoMode::left,
                          "a likely double-track is analysed through its sharper-picked side");
        tests.expectEqual(recommendStereoMode(layered, 0.1f), StereoMode::fullStereo,
                          "a reference that reads as one source keeps the full-stereo view");
        StereoAudio mono;
        mono.sampleRate = sampleRate;
        mono.left = layered.left;
        tests.expectEqual(recommendStereoMode(mono, 0.9f), StereoMode::fullStereo,
                          "mono input has no side to choose");
        tests.expectEqual(recommendStereoMode({}, 0.9f), StereoMode::fullStereo,
                          "empty input is safe to recommend for");
    }

    StereoAudio cleanPitch;
    cleanPitch.sampleRate = sampleRate;
    cleanPitch.left.resize(static_cast<std::size_t>(sampleRate * 2.0));
    cleanPitch.right.resize(cleanPitch.left.size());
    for (std::size_t index = 0; index < cleanPitch.samples(); ++index)
    {
        const auto value = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0
            * static_cast<double>(index) / sampleRate));
        cleanPitch.left[index] = cleanPitch.right[index] = value;
        (void) index;
    }
    RegionQuality pitchQuality; pitchQuality.startSeconds = 0.0; pitchQuality.endSeconds = 2.0;
    pitchQuality.duration = 2.0f; pitchQuality.confidence = 0.9f;
    const std::array pitchQualities { pitchQuality };
    const auto cleanParts = analyzePlayableRegions(cleanPitch, pitchQualities, TargetInstrument::guitar);
    const auto drivenPitch = drivenGuitar(sampleRate, 2.0);
    const auto drivenParts = analyzePlayableRegions(drivenPitch, pitchQualities, TargetInstrument::guitar);
    tests.expect(! cleanParts.empty() && cleanParts.front().dominantPitch == "A3"
                 && cleanParts.front().pitchConfidence > 0.4f,
                 "playable-region analysis detects dominant pitch with confidence");
    /* Ordering rather than category, and the change is a finding rather than a weakening.

       Gain character is measured from harmonic structure now, and synthetic fixtures do not reach
       the richness of real recordings: a hard-clipped power chord scores about 0.09 where a mixed
       metal rhythm track scores 0.6 to 0.9. A real *clean* guitar intro scores about 0.15 -- above
       the clipped chord -- so no absolute threshold can call this fixture driven and Enter
       Sandman's intro clean at the same time. Asserting a category here would either be asserting
       a threshold tuned to a fixture, or forcing the measure to fail on the material it exists to
       classify. What is genuinely true, and is what this guards, is that driven material scores
       clearly above clean material. */
    tests.expect(! drivenParts.empty() && cleanParts.front().gainCharacter == GainCharacter::clean,
                 "an undistorted tone is classified clean");
    tests.expect(! drivenParts.empty()
                 && drivenParts.front().gainScore > cleanParts.front().gainScore + 0.05f,
                 "playable-region analysis scores driven material clearly above clean material");

    {
        // Gain classification must not depend on how long the region is. The
        // analyser used to sub-sample regions longer than five seconds, which
        // aliased away the high-frequency content that identifies distortion and
        // made long high-gain parts read as crunch or clean. The two-second case
        // above never crossed that threshold, so it could not catch it.
        const auto drivenRegionOfLength = [&](double seconds)
        {
            const auto audio = drivenGuitar(sampleRate, seconds);
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
            /* The property this test was written for is *length independence*: the analyser used
               to sub-sample regions past a budget, which aliased away the very content it was
               measuring, so a long region scored lower than the same material short. Comparing the
               two scores states that directly, where "not clean" only detected it when the drop
               happened to cross a threshold. */
            tests.expect(std::abs(longRegion.front().gainScore - shortRegion.front().gainScore) < 0.12f,
                         "gain score does not depend on how long the region is");
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
                 "reference normalization removes DC/silence, records the level it removed, and optionally reduces room tail");
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

    /* The search offers a guitarist only voicings a guitarist would use.

       Every voicing answers for guitar, so a bass-native one appearing here would not crash or
       sound broken -- it would quietly take a shortlist slot from an amplifier the player might
       actually have used, and cost an extra offline render to do it. That is the kind of wrong
       that looks like a mediocre match rather than a bug.
    */
    auto guitarCandidatesAreGuitarVoicings = true;
    for (const auto& candidate : reconstruction.candidates)
        guitarCandidatesAreGuitarVoicings = guitarCandidatesAreGuitarVoicings
            && nts::amp::topologyAffinity(candidate.rigPreset.parameters.topology)
                   == nts::amp::TopologyAffinity::either;
    tests.expect(guitarCandidatesAreGuitarVoicings,
                 "a guitar reconstruction never proposes a bass-native voicing");

    auto bassReference = reference;
    bassReference.target = TargetInstrument::bass;
    const auto bassRun = reconstructor.reconstruct(bassReference, di, sampleRate, 10);
    tests.expect(bassRun.success, "a bass reconstruction completes: " + bassRun.error);

    /* The searchable set, tested where it is decidable.

       This file first tried to assert that a fitted bi-amp candidate keeps its 500 Hz split and
       that a hard-clipping candidate keeps its 8x. Both were **vacuous**: a shortlist is filled by
       refining the *winning* point, and topology is deliberately excluded from the "is this a
       different rig" test, so a synthetic reference that favours one voicing returns ten
       refinements of it and never reaches another. Asking for ten candidates instead of three
       changed nothing. Assertions over candidates that never appear are worse than no assertion --
       they read as coverage and are silence.

       What *is* decidable is the set the search draws from, which is why it is published. The
       length of that list is also the cost: the topology re-check renders the winning point under
       every voicing the coarse pass did not reach.
    */
    const auto guitarSearch = searchableTopologies(nts::amp::Instrument::guitar);
    const auto bassSearch = searchableTopologies(nts::amp::Instrument::bass);
    /* Both pools are non-empty and neither offers the other's exclusive voicings -- the part that
       holds whichever way `bassSearchesBassVoicingsOnly` is set. The *sizes* depend on that switch,
       so they are asserted under it rather than here: with the experiment on, bass searches six
       against guitar's seven, and the old "bass searches more" relation is inverted by design. */
    tests.expect(! guitarSearch.empty() && ! bassSearch.empty(),
                 "both instruments have something to search");
    if (bassSearchesBassVoicingsOnly)
        tests.expect(bassSearch.size() < nts::amp::topologyCount,
                     "the bass-only experiment narrows the bass pool");
    else
    {
        tests.expectEqual(bassSearch.size(), nts::amp::topologyCount,
                          "a bass reconstruction searches every voicing");
        tests.expect(guitarSearch.size() < bassSearch.size(),
                     "a guitar reconstruction searches fewer voicings than a bass one");
    }
    auto guitarSearchIsGuitarOnly = true;
    for (const auto topology : guitarSearch)
        guitarSearchIsGuitarOnly = guitarSearchIsGuitarOnly
            && nts::amp::topologyAffinity(topology) == nts::amp::TopologyAffinity::either;
    tests.expect(guitarSearchIsGuitarOnly, "no bass-native voicing is searched for a guitar reference");

    /* The gain prior: what a measured category argues for, and what it must not do.

       Asserted on the table directly rather than through a reconstruction. A full run renders
       every voicing and picks a winner, and that winner depends on the reference audio as much as
       on the prior -- so an end-to-end assertion here would be testing the synthetic test tone,
       and would go red for reasons that have nothing to do with this weighting. The property
       being claimed is a property of the table, so that is what is checked.
    */
    using nts::amp::Topology;
    using Character = nts::reconstruction::GainCharacter;

    // The claim, in the terms it was asked for: distortion aims at the high-gain pair, clean aims
    // at the studio and American voicings.
    const auto bestFor = [](Character category, std::span<const nts::amp::Topology> pool)
    {
        auto winner = pool.front();
        for (const auto topology : pool)
            if (topologyGainAffinity(topology, category) > topologyGainAffinity(winner, category))
                winner = topology;
        return winner;
    };
    const auto highGainWinner = bestFor(Character::distorted, guitarSearch);
    tests.expect(highGainWinner == Topology::tightModern || highGainWinner == Topology::saggingRectifier,
                 "a distorted reference argues hardest for a high-gain voicing");
    const auto cleanWinner = bestFor(Character::clean, guitarSearch);
    tests.expect(cleanWinner == Topology::studioDirect || cleanWinner == Topology::americanClean,
                 "a clean reference argues hardest for Studio Direct or American Clean");

    // And the two must actually disagree, or the prior is decoration.
    tests.expect(topologyGainAffinity(Topology::studioDirect, Character::clean)
                     > topologyGainAffinity(Topology::tightModern, Character::clean)
                 && topologyGainAffinity(Topology::tightModern, Character::distorted)
                     > topologyGainAffinity(Topology::studioDirect, Character::distorted),
                 "the clean and distorted voicings swap places between the two characters");

    /* It is a prior, not a filter, and that is the part worth guarding.

       The spread has to stay small enough that a clearly better match still wins. Pinned against
       the complexity penalty a stage costs (0.025) and against the similarity scale: one full
       swing of the prior must beat a complexity step -- otherwise it never decides anything --
       and must lose to a 15% similarity difference, or the search stops being a measurement.
    */
    auto widestSwing = 0.0f;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
        for (const auto category : { Character::clean, Character::crunch, Character::distorted })
            widestSwing = std::max(widestSwing, std::abs(topologyGainAffinity(
                static_cast<Topology>(index), category)));
    tests.expect(widestSwing <= 1.0f, "no affinity leaves the documented -1..+1 range");
    const auto fullSwing = nts::reconstruction::topologyGainPriorWeight * 2.0f * widestSwing;
    tests.expect(fullSwing > 0.025f, "the prior outweighs a complexity step, so it can decide a tie");
    tests.expect(fullSwing < 0.15f * 0.7f,
                 "the prior loses to a 15% tone-similarity difference, so it cannot overrule the measurement");

    // Every voicing is scored for every category: a missing row would silently read as neutral
    // and quietly opt one amplifier out of the prior altogether.
    auto everyRowScored = true;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
    {
        auto rowIsAllZero = true;
        for (const auto category : { Character::clean, Character::crunch, Character::distorted })
            rowIsAllZero = rowIsAllZero
                && std::abs(topologyGainAffinity(static_cast<Topology>(index), category)) < 1.0e-6f;
        everyRowScored = everyRowScored && ! rowIsAllZero;
    }
    tests.expect(everyRowScored, "every voicing carries a gain affinity, including the bass-native block");

    /* Where the search *starts*, which matters more than it looks.

       `coarsePoint` walks topology as `(variant / 45) % size` and the pool is twelve, so the
       coarse pass never advances past index 0 and the whole refinement is spent on that one
       voicing. Whatever sits first is therefore the voicing the winning point is fitted for, and
       every other voicing is later asked to compete at coordinates chosen for it.

       So the first entry is the assertion worth making: a distorted reference must start on a
       high-gain amplifier and a clean one on a clean amplifier, or the search spends its entire
       budget in the wrong neighbourhood and the rank prior is left to rescue it afterwards.
    */
    const auto highGainOrder = searchOrderForGain(nts::amp::Instrument::guitar, Character::distorted);
    const auto cleanOrder = searchOrderForGain(nts::amp::Instrument::guitar, Character::clean);
    tests.expect(! highGainOrder.empty() && (highGainOrder.front() == Topology::tightModern
                                             || highGainOrder.front() == Topology::saggingRectifier),
                 "a distorted reference refines around a high-gain voicing");
    tests.expect(! cleanOrder.empty() && (cleanOrder.front() == Topology::studioDirect
                                          || cleanOrder.front() == Topology::americanClean),
                 "a clean reference refines around Studio Direct or American Clean");
    tests.expect(highGainOrder.front() != cleanOrder.front(),
                 "the two references do not start the search in the same place");

    /* Ordering must not change the *set*: every voicing is still rendered by the topology
       re-check, so reordering may not quietly drop one. This is the property that keeps the
       seeding a hint rather than a filter, and it is the one that would break silently -- a
       dropped voicing simply never appears in anyone's shortlist again. */
    for (const auto category : { Character::clean, Character::crunch, Character::distorted })
        for (const auto instrument : { nts::amp::Instrument::guitar, nts::amp::Instrument::bass })
        {
            auto ordered = searchOrderForGain(instrument, category);
            auto plain = searchableTopologies(instrument);
            std::sort(ordered.begin(), ordered.end());
            std::sort(plain.begin(), plain.end());
            tests.expect(ordered == plain,
                         "ordering the search by gain keeps exactly the searchable set");
        }

    // Stable on ties, so the same reference always searches the same way. A reconstruction that
    // reordered itself between runs would be untraceable.
    tests.expect(searchOrderForGain(nts::amp::Instrument::guitar, Character::crunch)
                     == searchOrderForGain(nts::amp::Instrument::guitar, Character::crunch),
                 "the search order is deterministic");

    /* The crossover is scaled from the voicing rather than replaced by an absolute range.

       Checked as a ratio against each candidate's own factory value, which is the property the fit
       now has and the previous absolute `90 + tightness * 180` did not. Weaker than "the bi-amp
       keeps its 500 Hz split" and the strongest thing this API can observe; the voicing-specific
       consequence is covered where it is reachable, in the amp tests, by the factory preset.
    */
    auto crossoverScalesFromTheVoicing = true;
    auto crossoverStillFitted = false;
    for (const auto& candidate : bassRun.candidates)
    {
        const auto factory = nts::amp::makeOriginalPreset(candidate.rigPreset.parameters.topology,
                                                          nts::amp::Instrument::bass);
        const auto ratio = candidate.rigPreset.parameters.bass.crossoverHz
                         / std::max(1.0f, factory.parameters.bass.crossoverHz);
        crossoverScalesFromTheVoicing = crossoverScalesFromTheVoicing && ratio >= 0.5f && ratio <= 1.5f;
        crossoverStillFitted = crossoverStillFitted || std::abs(ratio - 1.0f) > 0.01f;
    }
    tests.expect(crossoverScalesFromTheVoicing,
                 "a fitted crossover stays within half to one-and-a-half of its voicing's own");
    tests.expect(crossoverStillFitted, "tightness still moves the fitted crossover");

    // The candidate label used to name only two voicings, so eleven of thirteen read "bloom".
    auto candidatesAreNamedForTheirVoicing = true;
    for (const auto& candidate : reconstruction.candidates)
        candidatesAreNamedForTheirVoicing = candidatesAreNamedForTheirVoicing
            && candidate.rigPreset.name.find(std::string(
                   nts::amp::topologyName(candidate.rigPreset.parameters.topology))) != std::string::npos;
    tests.expect(candidatesAreNamedForTheirVoicing,
                 "a candidate is named for the voicing it actually uses");

    {
        // Offering the user a shortlist is only useful if the entries differ.
        // The variant offset used to come from `variant % 5`, so a pool of twelve
        // contained five unique rigs and seven exact copies, and the shortlist
        // could hand back the same settings more than once.
        const auto shortlist = reconstructor.reconstruct(reference, di, sampleRate, 4);
        tests.expect(shortlist.success && shortlist.candidates.size() == 4,
                     "rig search returns the requested candidate count: " + shortlist.error);
        auto allDistinct = true;
        for (std::size_t a = 0; a < shortlist.candidates.size(); ++a)
            for (auto b = a + 1; b < shortlist.candidates.size(); ++b)
            {
                const auto& candidateA = shortlist.candidates[a].rigPreset.parameters;
                const auto& candidateB = shortlist.candidates[b].rigPreset.parameters;
                const auto sameDrive = std::abs(candidateA.stages[0].driveDb
                                                - candidateB.stages[0].driveDb) < 1.0e-4f;
                const auto sameTone = std::abs(candidateA.toneStack.treble
                                               - candidateB.toneStack.treble) < 1.0e-4f
                                   && std::abs(candidateA.toneStack.mid
                                               - candidateB.toneStack.mid) < 1.0e-4f;
                const auto samePreEq = std::abs(candidateA.preEq.highCutHz
                                                - candidateB.preEq.highCutHz) < 1.0e-4f
                                    && std::abs(candidateA.preEq.tightness
                                                - candidateB.preEq.tightness) < 1.0e-4f;
                /* Topology is part of a candidate's identity, not incidental to it.

                   The topology re-check renders the *same* refined point under every voicing, so
                   those candidates carry identical drive, tone and pre-EQ by construction and
                   differ only in which amplifier they are. That is not a duplicate: measured, the
                   same fitted values on a different voicing render at 0.71 similarity to the
                   original, not 1.0 -- the phase inverter, tone-stack type and the rest of the
                   block `makeOriginalPreset` chose are still different. Judging duplication on the
                   overwritten fields alone declared seven different amplifiers to be one. */
                const auto sameVoicing = candidateA.topology == candidateB.topology;
                if (sameVoicing && sameDrive && sameTone && samePreEq) allDistinct = false;
            }
        tests.expect(allDistinct, "shortlisted candidates are not duplicates of one another");

        auto oversampled = true;
        for (const auto& candidate : shortlist.candidates)
            for (std::size_t stage = 0; stage < candidate.rigPreset.parameters.stageCount; ++stage)
                oversampled = oversampled
                    && candidate.rigPreset.parameters.stages[stage].oversamplingFactor > 1;
        tests.expect(oversampled,
                     "candidates are scored and returned with anti-aliased preamp stages");

        // Refinement searches around the coarse winner, so it must not come back
        // worse than the coarse grid. Comparing a larger shortlist against a
        // smaller one from the same reference checks the ranking stays coherent.
        const auto single = reconstructor.reconstruct(reference, di, sampleRate, 1);
        tests.expect(single.success && ! single.candidates.empty(),
                     "rig search returns a best candidate: " + single.error);
        if (single.success && ! single.candidates.empty() && ! shortlist.candidates.empty())
        {
            const auto rank = [](const RigCandidate& value)
            {
                return value.toneSimilarity * 0.7f + value.recordingSimilarity * 0.3f
                     - value.complexityPenalty;
            };
            tests.expect(rank(single.candidates.front()) >= rank(shortlist.candidates.front()) - 1.0e-4f,
                         "the top candidate is the best the search found");
        }

        {
            // The dynamics axis has to actuate, not merely be measured. Two references with the
            // same spectrum and different transient behaviour must produce rigs that differ in
            // the three parameters it drives -- attack reduction and pick emphasis, which no
            // candidate ever set at all before, and supply sag.
            //
            // The reference is copied and its dynamic features are edited directly rather than
            // synthesising two clips that happen to analyse differently: what is under test is
            // the measurement-to-parameter mapping, and driving it from the measurement makes the
            // test about that mapping instead of about the analyser.
            auto openReference = reference;
            openReference.tone.features.dynamic.crestFactorDb = 17.0f;
            openReference.tone.features.dynamic.attackMilliseconds = 3.0f;
            auto squashedReference = reference;
            squashedReference.tone.features.dynamic.crestFactorDb = 4.0f;
            squashedReference.tone.features.dynamic.attackMilliseconds = 32.0f;

            const auto open = reconstructor.reconstruct(openReference, di, sampleRate, 1);
            const auto squashed = reconstructor.reconstruct(squashedReference, di, sampleRate, 1);
            tests.expect(open.success && squashed.success && ! open.candidates.empty()
                         && ! squashed.candidates.empty(),
                         "both dynamics references reconstruct: " + open.error + squashed.error);
            if (open.success && squashed.success && ! open.candidates.empty()
                && ! squashed.candidates.empty())
            {
                const auto& openRig = open.candidates.front().rigPreset.parameters;
                const auto& squashedRig = squashed.candidates.front().rigPreset.parameters;
                tests.expect(openRig.stages[0].attackReduction < squashedRig.stages[0].attackReduction,
                             "an open reference softens the pick less than a squashed one");
                tests.expect(openRig.preEq.pickEmphasisDb > squashedRig.preEq.pickEmphasisDb,
                             "an open reference gets more pick emphasis than a squashed one");
                tests.expect(openRig.powerAmp.sag < squashedRig.powerAmp.sag,
                             "an open reference asks for less supply sag than a squashed one");
            }

            // Double-tracking corrupts both of those measurements in the same direction: two takes
            // sum with their peaks apart, so crest factor reads low, and two pick attacks tens of
            // milliseconds apart read as one slow attack. Neither is a property of the amplifier,
            // so the seed corrects for it -- otherwise a hard-panned rhythm part fits a rig softer
            // and more compressed than the one in the recording.
            auto layeredReference = squashedReference;
            layeredReference.tone.features.spatial.doubleTrackingLikelihood = 0.9f;
            auto singleReference = squashedReference;
            singleReference.tone.features.spatial.doubleTrackingLikelihood = 0.0f;

            const auto layered = reconstructor.reconstruct(layeredReference, di, sampleRate, 1);
            const auto unlayered = reconstructor.reconstruct(singleReference, di, sampleRate, 1);
            tests.expect(layered.success && unlayered.success && ! layered.candidates.empty()
                         && ! unlayered.candidates.empty(),
                         "both layering references reconstruct: " + layered.error + unlayered.error);
            if (layered.success && unlayered.success && ! layered.candidates.empty()
                && ! unlayered.candidates.empty())
            {
                const auto& layeredRig = layered.candidates.front().rigPreset.parameters;
                const auto& singleRig = unlayered.candidates.front().rigPreset.parameters;
                tests.expect(layeredRig.stages[0].attackReduction < singleRig.stages[0].attackReduction,
                             "the same measurements read as double-tracked soften the pick less");
                tests.expect(layeredRig.preEq.pickEmphasisDb > singleRig.preEq.pickEmphasisDb,
                             "the same measurements read as double-tracked keep more pick emphasis");
            }
        }

        // Progress has to finish at 1 even though refinement can stop early and
        // leave the render count short of its upper bound.
        float lastFraction = -1.0f;
        auto monotonic = true;
        const auto tracked = reconstructor.reconstruct(reference, di, sampleRate, 2,
            [&](const SeparationProgress& update)
            {
                monotonic = monotonic && update.fraction >= lastFraction - 1.0e-4f;
                lastFraction = update.fraction;
                return true;
            });
        tests.expect(tracked.success, "tracked reconstruction succeeds: " + tracked.error);
        tests.expect(monotonic, "reconstruction progress never moves backwards");
        tests.expectNear(lastFraction, 1.0, 1.0e-4, "reconstruction progress finishes at one");

        std::stop_source stopper;
        stopper.request_stop();
        const auto cancelledRun = reconstructor.reconstruct(reference, di, sampleRate, 2, {},
                                                            stopper.get_token());
        tests.expect(! cancelledRun.success && ! cancelledRun.error.empty(),
                     "an already-cancelled search stops instead of rendering");
    }
    /* P1 -- the level normalisation removes is carried, not lost.

       Asserted on the profile rather than on audio: the whole point of the field is that the
       measurement survives the search, and a render-level assertion would pass just as well if
       the offset had been folded into the amplifier's own output gain, which is the mistake this
       exists to avoid. Two regions differing only by a known gain must differ by that gain. */
    {
        auto quieter = region;
        for (auto& sample : quieter.left) sample *= 0.25f;
        for (auto& sample : quieter.right) sample *= 0.25f;
        const auto loudProfile = estimateProductionProfile(normalizeReference(region, false), tone,
                                                            {}, {}, sampleRate);
        const auto quietProfile = estimateProductionProfile(normalizeReference(quieter, false), tone,
                                                             {}, {}, sampleRate);
        tests.expectNear(static_cast<double>(loudProfile.levelOffsetDb - quietProfile.levelOffsetDb),
                         20.0 * std::log10(4.0), 1.0,
                         "the level normalization removes is recovered into the production profile");
        tests.expect(std::isfinite(loudProfile.levelOffsetDb),
                     "a measured level offset is finite");
    }

    /* P2 -- the spectral residual measures spectrum, not envelope.

       The field this replaces, `broadProductionEqDb`, split the region into its first quarter,
       middle half and last quarter *by time* and called the result a frequency balance. These two
       cases tell a real measurement apart from that one: a render identical to its reference has
       to read flat, and a known shelf has to read back as that shelf. */
    {
        const auto flat = estimateProductionProfile(normalized, tone, normalized.audio.left,
                                                     normalized.audio.left, sampleRate);
        tests.expect(! flat.spectralResidualDb.empty()
                     && flat.spectralResidualDb.size() == flat.residualFrequencies.size(),
                     "a candidate render produces a banded spectral residual");
        auto worstDb = 0.0f;
        for (const auto value : flat.spectralResidualDb) worstDb = std::max(worstDb, std::abs(value));
        tests.expect(worstDb < 0.5f,
                     "a render identical to its reference reads a flat residual, not an envelope");

        // A one-pole high shelf: +3 dB well above 1 kHz, unity well below it.
        auto shelved = normalized.audio.left;
        const auto pole = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 1000.0 / sampleRate));
        const auto shelfGain = std::pow(10.0f, 3.0f / 20.0f) - 1.0f;
        auto lowpass = 0.0f;
        for (auto& sample : shelved)
        {
            lowpass = lowpass * pole + sample * (1.0f - pole);
            sample += (sample - lowpass) * shelfGain;
        }
        const auto tilted = estimateProductionProfile(normalized, tone, shelved,
                                                       normalized.audio.left, sampleRate);
        /* Read as the difference between the shelf's two ends rather than as an absolute level in
           the high bands. `measureCabinetResidual` levels each spectrum to its own mean before
           differencing, so a +3 dB shelf over half the range reads as roughly +1.5 and -1.5 rather
           than +3 and 0. That levelling is wanted -- it is what makes the residual a shape that
           composes with `levelOffsetDb` instead of double-counting the loudness -- so the shelf
           height is the span between the ends, which is invariant to it. */
        auto lowSum = 0.0f; auto lowCount = 0;
        auto highSum = 0.0f; auto highCount = 0;
        for (std::size_t index = 0; index < tilted.spectralResidualDb.size(); ++index)
        {
            const auto frequency = tilted.residualFrequencies[index];
            if (frequency < 200.0f) { lowSum += tilted.spectralResidualDb[index]; ++lowCount; }
            else if (frequency > 4000.0f) { highSum += tilted.spectralResidualDb[index]; ++highCount; }
        }
        tests.expect(lowCount > 0 && highCount > 0, "the residual spans the shelf's two ends");
        if (lowCount > 0 && highCount > 0)
            tests.expectNear(static_cast<double>(highSum / static_cast<float>(highCount)
                                                 - lowSum / static_cast<float>(lowCount)),
                             3.0, 1.0, "a known 3 dB shelf reads back as a 3 dB shelf");
    }

    /* P11 -- both estimators work, and the profile says which one ran.

       **Two paths that can drift, and the deterministic one drifts silently.** With a model
       installed the built-in mapping stops being exercised by anything a user does, so it rots
       exactly when it is needed -- on the machines with no model, which is most of them. Both are
       asserted here for that reason, and the assertion on the deterministic path is that a model
       argument of `nullptr` changes nothing at all.

       The learned path is checked for *wiring* rather than for accuracy: that it reaches the four
       amounts, that it leaves the level and the residual alone, and that `learned` distinguishes
       the two. Accuracy is `production_parity_driver.py`'s job against the Python model, and
       `nts_production_recovery`'s against the deterministic baseline. */
    {
        const auto deterministic = estimateProductionProfile(normalized, tone, normalized.audio.left,
                                                              normalized.audio.left, sampleRate);
        const auto fallback = estimateProductionProfile(normalized, tone, normalized.audio.left,
                                                         normalized.audio.left, sampleRate,
                                                         TargetInstrument::guitar, nullptr);
        tests.expect(! deterministic.learned && ! fallback.learned
                     && std::abs(deterministic.roomAmount - fallback.roomAmount) < 1.0e-6f,
                     "with no model installed the built-in mapping runs and says so");

        const std::array<float, 4> wanted { 0.21f, 0.34f, 0.55f, 0.72f };
        const auto packed = constantRegressor(wanted);
        nts::ml::PackedFeatureRegressor model;
        std::string error;
        tests.expect(model.load(packed, error), "a packed production regressor loads: " + error);

        const auto learned = estimateProductionProfile(normalized, tone, normalized.audio.left,
                                                        normalized.audio.left, sampleRate,
                                                        TargetInstrument::guitar, &model);
        tests.expect(learned.learned, "an installed model produces the amounts and is reported");
        tests.expectNear(learned.roomAmount, wanted[0], 1.0e-3, "the model supplies room");
        tests.expectNear(learned.delayAmount, wanted[1], 1.0e-3, "the model supplies delay");
        tests.expectNear(learned.doubling, wanted[2], 1.0e-3, "the model supplies doubling");
        tests.expectNear(learned.compression, wanted[3], 1.0e-3, "the model supplies compression");
        /* Level and the residual stay measured. They are recovered exactly -- 0.000 dB and
           0.031 dB RMS in the harness -- so putting an approximation in front of them could only
           lose, and a model that reached them would be a regression no accuracy number would
           show. */
        tests.expectNear(learned.levelOffsetDb, deterministic.levelOffsetDb, 1.0e-4,
                         "a model never touches the measured level");
        tests.expect(learned.spectralResidualDb == deterministic.spectralResidualDb,
                     "a model never touches the measured spectral residual");

        // A model of the wrong shape is refused rather than fed a misaligned vector, which would
        // produce four plausible numbers from columns meaning something else.
        const auto mismatched = constantRegressor(wanted);
        nts::ml::PackedFeatureRegressor wrongShape;
        std::vector<std::byte> truncated(mismatched.begin(), mismatched.end());
        std::uint32_t narrower = nts::reconstruction::productionFeatureCount - 1;
        std::memcpy(truncated.data() + 12, &narrower, sizeof(narrower));
        tests.expect(! wrongShape.load(truncated, error),
                     "a regressor whose header disagrees with its payload is refused");

        /* Bass is measured over its own band range, and this is the assertion that matters most
           for it. The residual's default floor is 70 Hz -- a guitar decision, taken because in a
           guitar stem everything below that is somebody else's instrument. A four-string's low E
           is 41 Hz and a five-string's low B is 31, so on a bass that floor discards the entire
           fundamental octave of the thing being measured and reports only what the mix did to the
           harmonics. */
        tests.expect(productionResidualOptions(TargetInstrument::bass).lowLimitHz
                     < productionResidualOptions(TargetInstrument::guitar).lowLimitHz,
                     "the bass residual reaches below the guitar residual's floor");
        tests.expect(productionResidualOptions(TargetInstrument::bass).lowLimitHz <= 31.0f,
                     "the bass residual reaches the low B of a five-string");
        const auto bassProfile = estimateProductionProfile(normalized, tone, normalized.audio.left,
                                                            normalized.audio.left, sampleRate,
                                                            TargetInstrument::bass);
        tests.expect(! bassProfile.residualFrequencies.empty()
                     && bassProfile.residualFrequencies.front() < 41.2f,
                     "a bass production estimate measures the octave its fundamentals live in");
        tests.expect(deterministic.residualFrequencies.size() == bassProfile.residualFrequencies.size(),
                     "both targets report the same number of bands, so the vector length is one contract");
        tests.expect(deterministic.residualFrequencies.front() > bassProfile.residualFrequencies.front(),
                     "the same feature column stands for a different frequency on each target");

        const auto features = productionFeatures(tone, deterministic);
        tests.expectEqual(features.size(), productionFeatureCount,
                          "the feature vector is the length the artifact contract fixes");
        tests.expect(productionFeatures(tone, {}).empty(),
                     "a profile with no residual yields no feature vector rather than zeros");
    }

    /* A bass reference searches bass amplifiers first.

       **This was a real defect and the symptom was visible in the product**: matching a bass part
       returned guitar voicings. `searchableTopologies` filtered the pool by instrument and then
       `searchOrderForGain` sorted it by gain character alone, so instrument affinity never reached
       the ordering. Ties broke on enum order, guitar occupies indices 0-6 and bass-native 7-12, so
       a bass-native voicing could not win a tie -- and the first entry is the one the whole
       coordinate descent is spent on.

       Asserted on the *first* entry specifically, not on mere presence. Every bass-native voicing
       was already present in the list before the fix; being present and never being fitted is
       exactly the failure that shipped.
    */
    {
        const auto isBassNative = [](nts::amp::Topology topology)
        {
            return nts::amp::topologyAffinity(topology) == nts::amp::TopologyAffinity::bass;
        };
        for (const auto character : { GainCharacter::clean, GainCharacter::crunch,
                                      GainCharacter::distorted })
        {
            const auto bassOrder = searchOrderForGain(nts::amp::Instrument::bass, character);
            tests.expect(! bassOrder.empty() && isBassNative(bassOrder.front()),
                         std::string("a bass search starts from a bass-native voicing: ")
                             + std::string(toString(character)));
            /* What the pool contains depends on the `bassSearchesBassVoicingsOnly` experiment, so
               this asserts whichever behaviour that switch currently selects rather than pinning
               one of them. Written as a branch instead of deleted: the moment the switch goes back
               to `false`, the reachability guarantee has to be covered again, and a test that had
               quietly dropped it would not notice. */
            auto guitarReachable = false;
            for (const auto topology : bassOrder) guitarReachable = guitarReachable || ! isBassNative(topology);
            if (bassSearchesBassVoicingsOnly)
            {
                tests.expect(! guitarReachable,
                             std::string("the bass-only experiment offers no guitar voicing: ")
                                 + std::string(toString(character)));
                auto allBassNative = true;
                for (const auto topology : bassOrder) allBassNative = allBassNative && isBassNative(topology);
                tests.expect(allBassNative && ! bassOrder.empty(),
                             "the bass-only experiment searches bass-native voicings exclusively");
                tests.expect(bassOrder.size() < nts::amp::topologyCount,
                             "the bass-only experiment searches fewer voicings than exist");
            }
            else
            {
                // Still a search, not a redirect: guitar voicings remain reachable for a bass part
                // that genuinely sounds like one.
                tests.expect(guitarReachable,
                             std::string("a bass search can still reach guitar voicings: ")
                                 + std::string(toString(character)));
                tests.expectEqual(bassOrder.size(), std::size_t { nts::amp::topologyCount },
                                  "a bass search considers every voicing");
            }

            /* Guitar is untouched. `searchableTopologies` removes every bass-native voicing from a
               guitar search, so the bonus has nothing to reorder -- and a guitar reconstruction
               must search exactly the order it did before this changed, or the rig-recovery
               baseline stops meaning anything. */
            const auto guitarOrder = searchOrderForGain(nts::amp::Instrument::guitar, character);
            auto guitarClean = true;
            for (const auto topology : guitarOrder) guitarClean = guitarClean && ! isBassNative(topology);
            tests.expect(guitarClean, "a guitar search offers no bass-native voicing");
            auto sortedByGain = true;
            for (std::size_t index = 1; index < guitarOrder.size(); ++index)
                sortedByGain = sortedByGain
                    && topologyGainAffinity(guitarOrder[index - 1], character)
                       >= topologyGainAffinity(guitarOrder[index], character);
            tests.expect(sortedByGain, "a guitar search is still ordered by gain affinity alone");
        }
    }

    /* P7 -- production must never enter the search.

       The single way this feature can make the product worse is by leaking into ranking: scored
       with production applied, the search would fit the *amplifier* to compensate for reverb and
       mix EQ, producing a rig that is right inside one mix and wrong everywhere else. Nothing
       else in this file would notice, because the candidates it produced would still look
       plausible. So the boundary is asserted rather than intended -- two references differing
       only in their `ProductionProfile` must rank identically.

       Verified to fail: subtracting `reference.production.roomAmount * 0.1f` from
       `recordingSimilarity` in `reconstruct()` breaks this and no other assertion here. */
    {
        auto plainReference = reference;
        plainReference.production = {};
        auto producedReference = reference;
        producedReference.production.valid = true;
        producedReference.production.levelOffsetDb = 9.0f;
        producedReference.production.roomAmount = 0.9f;
        producedReference.production.delayAmount = 0.8f;
        producedReference.production.doubling = 0.7f;
        producedReference.production.stereoWidth = 0.95f;
        producedReference.production.compression = 0.85f;
        producedReference.production.confidence = 1.0f;
        producedReference.production.spectralResidualDb.assign(41, 6.0f);
        producedReference.production.residualFrequencies.assign(41, 1000.0f);
        const auto plain = reconstructor.reconstruct(plainReference, di, sampleRate, 3);
        const auto produced = reconstructor.reconstruct(producedReference, di, sampleRate, 3);
        auto identical = plain.success && produced.success
                      && plain.candidates.size() == produced.candidates.size();
        for (std::size_t index = 0; identical && index < plain.candidates.size(); ++index)
        {
            const auto& withoutProduction = plain.candidates[index];
            const auto& withProduction = produced.candidates[index];
            identical = withoutProduction.rigPreset.name == withProduction.rigPreset.name
                     && std::abs(withoutProduction.toneSimilarity - withProduction.toneSimilarity) < 1.0e-6f
                     && std::abs(withoutProduction.recordingSimilarity - withProduction.recordingSimilarity) < 1.0e-6f
                     && std::abs(withoutProduction.confidence - withProduction.confidence) < 1.0e-6f;
        }
        tests.expect(identical, "a production estimate on the reference changes no candidate");
    }

    const auto serialized = serializeResult(reconstruction);
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(serialized.c_str()));
    tests.expect(serialized.find("rigPreset") != std::string::npos
                 && serialized.find("sourceHash") != std::string::npos
                 && serialized.find("audioSamples") == std::string::npos && parsed.isObject(),
                 "shared reconstruction package includes parameters and provenance but no source audio");
    tests.expect(serialized.find("\"production\":null") != std::string::npos,
                 "a match with no production estimate exports a null block, not a block of zeros");
    {
        auto measured = reconstruction;
        measured.reference.production = estimateProductionProfile(normalized, tone,
                                                                   normalized.audio.left,
                                                                   normalized.audio.left, sampleRate);
        const auto withProduction = serializeResult(measured);
        const auto reparsed = juce::JSON::parse(juce::String::fromUTF8(withProduction.c_str()));
        tests.expect(reparsed.isObject() && withProduction.find("levelOffsetDb") != std::string::npos
                     && withProduction.find("spectralResidualDb") != std::string::npos,
                     "a measured production estimate survives export as a readable block");
        tests.expect(measured.reconstructionVersion.find("v2") != std::string::npos,
                     "the export format is versioned past the schema that had no production block");
    }
    tests.expect(! reconstructor.reconstruct(reference, {}, sampleRate).success,
                 "missing user DI is rejected instead of fabricating a playable result");
    return tests.result();
}
