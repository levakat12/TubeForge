/** Round-trip recovery: render a DI through a known rig, then ask the search to name it back.

    **The point is that this needs no labels and no copyrighted audio.** The engine renders the
    ground truth itself, so "did the match find the right amplifier" becomes a number instead of an
    opinion. Everything about the rig search was previously tuned by reasoning about score
    magnitudes -- the prior weight, how many voicings the coarse pass samples, the rank mix -- and
    none of it had a measurement behind it. This is that measurement.

        nts_rig_recovery

    Two things are deliberately made *easy* here, because the question is about the search and not
    about everything upstream of it. There is no separation: the reference is the render, so no
    separator artifacts. And the DI handed to the search is the true DI, not the reference standing
    in for one -- which is the structural limitation `StudioServices` carries and warns about. A
    search that cannot recover a rig under these conditions will certainly not recover one from a
    song, so the numbers here are a ceiling rather than a prediction.
*/
#include "RecoveryFixtures.h"

#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/tone/ToneAnalysis.h>
#include <nts/tone/ToneProfileDatabase.h>
#include <nts/amp/TraditionalAmp.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <numbers>
#include <string>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr double seconds = 4.0;
using nts::recovery::phrases;
using nts::recovery::renderPhrase;
} // namespace

int main(int argc, char** argv)
{
    using namespace nts::reconstruction;
    /* `--bass` measures the other instrument. Added because bass recovery had never been measured
       at all: the harness was guitar-only, so the defect where a bass reference searched guitar
       voicings first was invisible to every number this file printed. */
    auto bass = false;
    for (int index = 1; index < argc; ++index) bass = bass || std::string(argv[index]) == "--bass";
    const auto instrument = bass ? nts::amp::Instrument::bass : nts::amp::Instrument::guitar;
    const auto target = bass ? TargetInstrument::bass : TargetInstrument::guitar;
    const auto& fixtures = phrases(bass ? nts::recovery::Fixture::bass
                                        : nts::recovery::Fixture::guitar);
    std::cout << "target: " << toString(target) << std::endl;
    const auto searchable = searchableTopologies(instrument);
    // The voicing the focus dump examines; Vintage Bloom is the one never recovered.
    constexpr auto focusVoicing = nts::amp::Topology::vintageBloom;

    // Five points across each voicing's own drive range, so recovery is measured over the span a
    // player uses rather than at one arbitrary setting.
    struct Trim { const char* label; float driveOffsetDb; };
    constexpr Trim trims[] = { { "-9", -9.0f }, { "-4", -4.0f }, { "0", 0.0f },
                               { "+5", +5.0f }, { "+10", +10.0f } };

    int attempts {}, topOne {}, inShortlist {};
    std::map<std::string, int> characterTally;
    std::map<std::string, std::pair<int, int>> byPhrase;   // name -> {hits, attempts}
    std::map<std::string, std::pair<int, int>> byDrive;
    std::map<std::string, std::pair<int, int>> bySource;
    std::map<std::string, int> recoveredTally;

    for (const auto& phrase : fixtures)
    {
        const auto di = renderPhrase(phrase, sampleRate, seconds);
        for (const auto topology : searchable)
        {
            for (const auto& trim : trims)
            {
                auto preset = nts::amp::makeOriginalPreset(topology, instrument);
                for (std::size_t stage = 0; stage < preset.parameters.stageCount; ++stage)
                    preset.parameters.stages[stage].driveDb += trim.driveOffsetDb;

                nts::amp::TraditionalAmpProcessor processor;
                processor.prepare({ sampleRate, 256, 1 });
                processor.loadPreset(preset, 0);
                const auto rendered = nts::amp::renderOffline(processor, di, 256);
                if (rendered.empty()) continue;

                StereoAudio reference;
                reference.sampleRate = sampleRate;
                reference.left = rendered;
                reference.right = rendered;

                RegionQuality whole;
                whole.startSeconds = 0.0; whole.endSeconds = seconds;
                whole.confidence = 0.9f; whole.targetEnergy = 0.5f;
                const auto regions = analyzePlayableRegions(reference,
                    std::vector<RegionQuality> { whole }, target);
                if (regions.empty()) continue;

                nts::tone::ToneAnalyzer analyzer;
                auto tone = analyzer.analyze({ reference.left, reference.right, sampleRate,
                                               nts::tone::SourceType::pluginRender, 0.9f });
                if (! tone.success) continue;

                ReconstructionReference input;
                input.regionStartSeconds = 0.0;
                input.regionEndSeconds = seconds;
                input.analysisConfidence = tone.report.confidence.aggregate;
                input.target = target;
                input.gainCharacter = regions.front().gainCharacter;
                input.quality = whole;
                input.tone = std::move(tone);

                RigReconstructor reconstructor;
                const auto result = reconstructor.reconstruct(input, di, sampleRate, 5);
                ++attempts;
                ++characterTally[std::string(toString(input.gainCharacter))];

                const auto sourceName = std::string(nts::amp::topologyName(topology));
                ++byPhrase[phrase.name].second;
                ++byDrive[trim.label].second;
                ++bySource[sourceName].second;
                if (! result.success || result.candidates.empty()) continue;

                const auto recovered = result.candidates.front().rigPreset.parameters.topology;
                ++recoveredTally[std::string(nts::amp::topologyName(recovered))];
                auto found = false;
                for (const auto& candidate : result.candidates)
                    found = found || candidate.rigPreset.parameters.topology == topology;
                if (recovered == topology)
                {
                    ++topOne;
                    ++byPhrase[phrase.name].first;
                    ++byDrive[trim.label].first;
                    ++bySource[sourceName].first;
                }
                if (found) ++inShortlist;
            }
        }
        std::cout << "  " << phrase.name << ": " << byPhrase[phrase.name].first << "/"
                  << byPhrase[phrase.name].second << std::endl;
    }

    if (attempts == 0) { std::cout << "no attempts" << std::endl; return 0; }
    const auto percent = [&](int value) { return 100.0 * value / attempts; };
    std::cout << std::endl << std::fixed << std::setprecision(1)
              << "attempts        " << attempts << std::endl
              << "top-1 recovery  " << topOne << "/" << attempts << "  (" << percent(topOne) << "%)" << std::endl
              << "in shortlist    " << inShortlist << "/" << attempts << "  (" << percent(inShortlist) << "%)" << std::endl
              << "chance baseline " << (100.0 / static_cast<double>(std::max<std::size_t>(1, searchable.size())))
              << "%  (top-1, " << searchable.size() << " voicings)" << std::endl;


    /* One voicing, examined candidate by candidate.

       `Vintage Bloom` is recovered 0 times out of 20 with the prior on and 0 with it off, while
       being *returned* 12 times for other voicings' renders. A voicing that can win when it is
       wrong and never when it is right is not a tuning problem, so this prints what actually beat
       it and by how much: the rank is `tone*0.7 + recording*0.3 - complexity`, and any of the three
       could be the one doing the damage. */
    std::cout << std::endl << "focus: " << std::string(nts::amp::topologyName(focusVoicing)) << std::endl;
    {
        const auto& phrase = phrases()[1];
        const auto focusDi = renderPhrase(phrase, sampleRate, seconds);
        for (const auto& trim : trims)
        {
            auto preset = nts::amp::makeOriginalPreset(focusVoicing, instrument);
            for (std::size_t stage = 0; stage < preset.parameters.stageCount; ++stage)
                preset.parameters.stages[stage].driveDb += trim.driveOffsetDb;
            nts::amp::TraditionalAmpProcessor processor;
            processor.prepare({ sampleRate, 256, 1 });
            processor.loadPreset(preset, 0);
            const auto rendered = nts::amp::renderOffline(processor, focusDi, 256);
            if (rendered.empty()) continue;

            StereoAudio reference;
            reference.sampleRate = sampleRate;
            reference.left = rendered; reference.right = rendered;
            RegionQuality whole;
            whole.startSeconds = 0.0; whole.endSeconds = seconds;
            whole.confidence = 0.9f; whole.targetEnergy = 0.5f;
            const auto regions = analyzePlayableRegions(reference,
                std::vector<RegionQuality> { whole }, target);
            if (regions.empty()) continue;
            nts::tone::ToneAnalyzer analyzer;
            auto tone = analyzer.analyze({ reference.left, reference.right, sampleRate,
                                           nts::tone::SourceType::pluginRender, 0.9f });
            if (! tone.success) continue;
            ReconstructionReference input;
            input.regionStartSeconds = 0.0; input.regionEndSeconds = seconds;
            input.analysisConfidence = tone.report.confidence.aggregate;
            input.target = target;
            input.gainCharacter = regions.front().gainCharacter;
            input.quality = whole;
            input.tone = std::move(tone);

            RigReconstructor reconstructor;
            // Ask for as many candidates as there are voicings, so the true one is only absent if
            // the shortlist's diversity filter dropped it or it genuinely ranked last.
            const auto result = reconstructor.reconstruct(input, focusDi, sampleRate,
                                                          searchable.size());
            std::cout << "  drive " << std::setw(4) << trim.label << "  "
                      << std::string(toString(input.gainCharacter)) << std::endl;
            if (! result.success) { std::cout << "    failed: " << result.error << std::endl; continue; }
            auto sawFocus = false;
            for (const auto& candidate : result.candidates)
            {
                const auto& parameters = candidate.rigPreset.parameters;
                const auto isFocus = parameters.topology == focusVoicing;
                sawFocus = sawFocus || isFocus;
                const auto rankValue = candidate.toneSimilarity * 0.7f
                                     + candidate.recordingSimilarity * 0.3f
                                     - candidate.complexityPenalty;
                std::cout << "    " << (isFocus ? "*" : " ") << std::left << std::setw(19)
                          << std::string(nts::amp::topologyName(parameters.topology)) << std::right
                          << " rank " << std::setprecision(4) << rankValue
                          << "  tone " << candidate.toneSimilarity
                          << "  rec " << candidate.recordingSimilarity
                          << "  cplx " << candidate.complexityPenalty
                          << "  stages " << parameters.stageCount << std::endl;
            }
            if (! sawFocus)
                std::cout << "    (the true voicing is not in the shortlist at all)" << std::endl;
        }
    }

    // One representative phrase for the two diagnostics below: they compare voicings against each
    // other rather than measuring recovery, so they want a fixed signal, not the sweep.
    const auto diagnosticDi = renderPhrase(fixtures[1], sampleRate, seconds);

    /* How separable the voicings are at all, before any search strategy is blamed.

       Recovery sitting at chance could mean the search looks in the wrong place, or it could mean
       the thing it is looking for is not visible in what it measures. Two hypotheses were already
       eliminated by measurement -- the DI limitation, because this harness hands over the true DI,
       and topology being frozen during refinement, because refining the winning voicing a second
       time on its own terms changed nothing. This is the third: render every voicing through the
       same DI at its own factory preset and score every pair against each other.

       Self-similarity is 1.0 by construction, so the number that matters is how close the
       *off-diagonal* comes to it. If two different amplifiers score 0.98 against each other, no
       ranking over these features can reliably tell them apart, and 14% is a ceiling set by the
       feature set rather than by the search. */
    std::cout << std::endl << "voicing separability (tone similarity between factory presets)" << std::endl;
    std::vector<nts::tone::ToneProfile> profiles;
    std::vector<std::string> names;
    for (const auto topology : searchable)
    {
        const auto preset = nts::amp::makeOriginalPreset(topology, instrument);
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, 256, 1 });
        processor.loadPreset(preset, 0);
        const auto rendered = nts::amp::renderOffline(processor, diagnosticDi, 256);
        nts::tone::ToneAnalyzer analyzer;
        auto tone = analyzer.analyze({ rendered, {}, sampleRate,
                                       nts::tone::SourceType::pluginRender, 1.0f });
        if (! tone.success) continue;
        nts::tone::ToneProfile entry;
        entry.profileId = std::string(nts::amp::topologyName(topology));
        entry.name = entry.profileId;
        entry.embedding = tone.embedding;
        entry.features = tone.features;
        entry.report = tone.report;
        entry.instrument = tone.report.context.instrument;
        entry.captureQuality = tone.report.confidence.aggregate;
        profiles.push_back(std::move(entry));
        names.push_back(std::string(nts::amp::topologyName(topology)));
    }

    double worstOff = 0.0, sumOff = 0.0;
    int countOff = 0;
    for (std::size_t row = 0; row < profiles.size(); ++row)
    {
        std::cout << "  " << std::left << std::setw(19) << names[row] << std::right;
        for (std::size_t column = 0; column < profiles.size(); ++column)
        {
            const auto score = nts::tone::ToneSimilarity::compare(profiles[row], profiles[column]).score;
            std::cout << std::setw(7) << std::setprecision(3) << score;
            if (row != column) { worstOff = std::max(worstOff, static_cast<double>(score));
                                 sumOff += score; ++countOff; }
        }
        std::cout << std::endl;
    }
    if (countOff > 0)
        std::cout << "  highest score between two different voicings: " << worstOff
                  << "   mean: " << (sumOff / countOff) << std::endl;


    /* How much of a voicing survives being fitted.

       `Topology` selects a block of values in `makeOriginalPreset` and has no independent effect on
       the DSP graph -- the developer guide says so plainly. `setCandidateParameters` then
       overwrites stage drives and count, power-amp master and saturation, and the tone stack: most
       of what that block set. So two candidates carrying different `topology` flags but the same
       fitted numbers may render as very nearly the same amplifier.

       This measures exactly that. Each voicing is rendered twice: once at its own factory preset,
       and once with the first voicing's fitted-over fields pasted onto it. If the second render
       scores near 1.0 against the first voicing's render, the flag is all that is left of the
       voicing by the time the search compares candidates -- and no ranking can recover something
       the audio no longer carries. */
    std::cout << std::endl << "how much of a voicing survives fitting" << std::endl;
    const auto reference = nts::amp::makeOriginalPreset(searchable.front(), instrument);
    nts::tone::ToneProfile referenceProfile;
    {
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, 256, 1 });
        processor.loadPreset(reference, 0);
        const auto rendered = nts::amp::renderOffline(processor, diagnosticDi, 256);
        nts::tone::ToneAnalyzer analyzer;
        const auto tone = analyzer.analyze({ rendered, {}, sampleRate,
                                             nts::tone::SourceType::pluginRender, 1.0f });
        referenceProfile.profileId = "reference";
        referenceProfile.embedding = tone.embedding;
        referenceProfile.features = tone.features;
        referenceProfile.report = tone.report;
    }
    double sumFitted = 0.0; int countFitted = 0;
    for (std::size_t index = 1; index < searchable.size(); ++index)
    {
        auto fitted = nts::amp::makeOriginalPreset(searchable[index], instrument);
        // The fields the candidate fitter writes, taken from the reference voicing.
        fitted.parameters.stageCount = reference.parameters.stageCount;
        fitted.parameters.stages = reference.parameters.stages;
        fitted.parameters.powerAmp = reference.parameters.powerAmp;
        fitted.parameters.toneStack = reference.parameters.toneStack;
        fitted.parameters.phaseInverter = reference.parameters.phaseInverter;
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, 256, 1 });
        processor.loadPreset(fitted, 0);
        const auto rendered = nts::amp::renderOffline(processor, diagnosticDi, 256);
        nts::tone::ToneAnalyzer analyzer;
        const auto tone = analyzer.analyze({ rendered, {}, sampleRate,
                                             nts::tone::SourceType::pluginRender, 1.0f });
        if (! tone.success) continue;
        nts::tone::ToneProfile entry;
        entry.profileId = "fitted";
        entry.embedding = tone.embedding;
        entry.features = tone.features;
        entry.report = tone.report;
        const auto score = nts::tone::ToneSimilarity::compare(referenceProfile, entry).score;
        std::cout << "  " << std::left << std::setw(19) << std::string(nts::amp::topologyName(searchable[index]))
                  << std::right << " wearing " << std::string(nts::amp::topologyName(searchable.front()))
                  << "'s fitted values scores " << std::setprecision(3) << score << std::endl;
        sumFitted += score; ++countFitted;
    }
    if (countFitted > 0)
        std::cout << "  mean: " << (sumFitted / countFitted)
                  << "   (compare with " << (sumOff / countOff) << " at factory presets)" << std::endl;

    std::cout << "measured character of the renders: ";
    for (const auto& entry : characterTally) std::cout << entry.first << "=" << entry.second << " ";
    std::cout << std::endl;
    const auto share = [](const std::pair<int, int>& entry)
    {
        return entry.second == 0 ? 0.0 : 100.0 * entry.first / entry.second;
    };
    std::cout << std::endl << "  by drive offset (dB from the voicing's factory value)" << std::endl;
    for (const auto& entry : byDrive)
        std::cout << "    " << std::setw(5) << entry.first << "  " << entry.second.first << "/"
                  << entry.second.second << "  " << std::setprecision(1) << share(entry.second) << "%" << std::endl;
    std::cout << "  by source voicing" << std::endl;
    for (const auto& entry : bySource)
        std::cout << "    " << std::left << std::setw(20) << entry.first << std::right
                  << entry.second.first << "/" << entry.second.second << "  "
                  << share(entry.second) << "%" << std::endl;
    /* What the search returns regardless of what it was given.

       A flat distribution here means the search is guessing; a spiked one means it has a standing
       preference, which is a different and more fixable fault. With every source voicing equally
       represented in the sweep, an unbiased search would return each about as often. */
    std::cout << "  what was returned, over all sources" << std::endl;
    for (const auto& entry : recoveredTally)
        std::cout << "    " << std::left << std::setw(20) << entry.first << std::right
                  << entry.second << std::endl;
    return 0;
}
