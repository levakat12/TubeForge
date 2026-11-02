/** Runs the real reconstruction pipeline over a folder of songs and prints what it decided.

    **Not a ctest suite, and deliberately so.** It needs a folder of real recordings that cannot be
    committed -- commercial releases, and 70 MB of them -- so a test depending on them would fail on
    every machine but one. It is a tool you point at your own material:

        nts_song_probe [folder]        (default: test_songs under the working directory)

    It exists because the synthetic signals in `nts_reconstruction_tests` cannot answer the question
    that actually matters: whether the gain character measured from real music varies, and whether
    the voicing that wins follows it. A clipped sine stays fundamental-dominant and reads clean
    however hard it is driven, which is exactly the blind spot that let a miscalibrated classifier
    ship. Real guitar is not a sine.

    Uses the deterministic CPU separator rather than the ML one so it needs no Python, and caps how
    much of each file it reads so a folder of albums does not become an afternoon.
*/
#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/tone/ToneAnalysis.h>
#include <nts/amp/TraditionalAmp.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{
constexpr double workingSampleRate = 48000.0;
/// Enough to cover several different passages without decoding a whole track.
constexpr double maximumSeconds = 150.0;

std::optional<nts::reconstruction::StereoAudio> decodeSong(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->numChannels == 0) return std::nullopt;
    const auto wanted = static_cast<juce::int64>(reader->sampleRate * maximumSeconds);
    const auto sourceSamples = static_cast<int>(std::min(reader->lengthInSamples, wanted));
    if (sourceSamples <= 0) return std::nullopt;
    juce::AudioBuffer<float> decoded(static_cast<int>(std::min<unsigned int>(2, reader->numChannels)),
                                     sourceSamples);
    if (! reader->read(&decoded, 0, sourceSamples, 0, true, reader->numChannels > 1)) return std::nullopt;

    const auto outputSamples = static_cast<int>(std::ceil(
        static_cast<double>(sourceSamples) * workingSampleRate / reader->sampleRate));
    nts::reconstruction::StereoAudio song;
    song.sampleRate = workingSampleRate;
    song.left.resize(static_cast<std::size_t>(outputSamples));
    song.right.resize(static_cast<std::size_t>(outputSamples));
    const auto ratio = reader->sampleRate / workingSampleRate;
    for (int channel = 0; channel < 2; ++channel)
    {
        juce::LagrangeInterpolator interpolator;
        auto& destination = channel == 0 ? song.left : song.right;
        interpolator.process(ratio, decoded.getReadPointer(std::min(channel, decoded.getNumChannels() - 1)),
                             destination.data(), outputSamples, sourceSamples, 0);
    }
    return song;
}
} // namespace

int main(int argc, char** argv)
{
    using namespace nts::reconstruction;
    const juce::String folderName = argc > 1 ? juce::String(argv[1]) : juce::String("test_songs");
    auto folder = juce::File::isAbsolutePath(folderName)
        ? juce::File(folderName)
        : juce::File::getCurrentWorkingDirectory().getChildFile(folderName);
    if (! folder.isDirectory())
    {
        std::cout << "no song folder at " << folder.getFullPathName() << " -- nothing to probe" << std::endl;
        return 0;
    }

    auto files = folder.findChildFiles(juce::File::findFiles, false, "*.mp3;*.wav;*.flac;*.aif;*.aiff");
    files.sort();
    if (files.isEmpty()) { std::cout << "no audio in " << folder.getFullPathName() << std::endl; return 0; }

    std::map<std::string, int> characterTally;
    std::map<std::string, int> voicingTally;

    for (const auto& file : files)
    {
        std::cout << std::endl << file.getFileNameWithoutExtension() << std::endl;
        const auto song = decodeSong(file);
        if (! song) { std::cout << "  could not decode" << std::endl; continue; }

        StemSeparator separator;
        SeparationOptions options;
        const auto stems = separator.separate(*song, options);
        auto selected = applyStereoMode(selectStem(stems, TargetInstrument::guitar), StereoMode::fullStereo);
        const auto quality = scoreRegions(stems, TargetInstrument::guitar, 7.5, 3.75);
        const auto regions = analyzePlayableRegions(selected, quality, TargetInstrument::guitar);
        if (regions.empty()) { std::cout << "  no playable regions" << std::endl; continue; }

        // Every region's character, which is the distribution the search depends on.
        std::map<std::string, int> perSong;
        for (const auto& region : regions)
        {
            ++perSong[std::string(toString(region.gainCharacter))];
            ++characterTally[std::string(toString(region.gainCharacter))];
        }
        std::cout << "  regions: " << regions.size() << "   ";
        for (const auto& entry : perSong) std::cout << entry.first << "=" << entry.second << " ";
        std::cout << std::endl;

        /* The same opening seconds read straight off the mix, with no separation at all.

           This column exists to tell two very different failures apart. If the separated stem reads
           distorted where the raw mix reads clean, the deterministic CPU separator's artifacts are
           what the character estimator is measuring -- it splatters broadband energy, and broadband
           energy is exactly what adjacent-sample slew responds to. If both read the same, the
           estimator itself is running hot on real material. Only one of those is a bug in the
           estimator, and guessing which would have been easy and wrong. */
        const auto openingQuality = quality.empty() ? RegionQuality {} : quality.front();
        auto rawOpening = extractRegion(*song, openingQuality.startSeconds, openingQuality.endSeconds);
        const auto rawRegions = analyzePlayableRegions(rawOpening,
            std::vector<RegionQuality> { openingQuality }, TargetInstrument::guitar);
        const auto stemRegions = analyzePlayableRegions(
            extractRegion(selected, openingQuality.startSeconds, openingQuality.endSeconds),
            std::vector<RegionQuality> { openingQuality }, TargetInstrument::guitar);
        std::cout << "  opening " << std::fixed << std::setprecision(1)
                  << openingQuality.startSeconds << "-" << openingQuality.endSeconds << "s   stem="
                  << (stemRegions.empty() ? std::string("n/a") : std::string(toString(stemRegions.front().gainCharacter)))
                  << " (" << std::setprecision(2) << (stemRegions.empty() ? 0.0f : stemRegions.front().gainScore) << ")"
                  << "   raw mix="
                  << (rawRegions.empty() ? std::string("n/a") : std::string(toString(rawRegions.front().gainCharacter)))
                  << " (" << (rawRegions.empty() ? 0.0f : rawRegions.front().gainScore) << ")"
                  << std::setprecision(1) << std::endl;

        // And the rig the best region actually produces.
        const auto best = static_cast<std::size_t>(std::distance(regions.begin(),
            std::max_element(regions.begin(), regions.end(), [](const auto& a, const auto& b)
            { return a.quality.confidence < b.quality.confidence; })));
        const auto chosen = regions[best];
        auto region = extractRegion(selected, chosen.quality.startSeconds, chosen.quality.endSeconds);
        auto normalized = normalizeReference(region, chosen.quality.reverbAmount > 0.45f);
        nts::tone::ToneAnalyzer analyzer;
        auto tone = analyzer.analyze({ normalized.audio.left, normalized.audio.right, workingSampleRate,
                                       nts::tone::SourceType::isolatedStem, chosen.quality.confidence });
        if (! tone.success) { std::cout << "  tone analysis failed: " << tone.error << std::endl; continue; }

        ReconstructionReference reference;
        reference.regionStartSeconds = chosen.quality.startSeconds;
        reference.regionEndSeconds = chosen.quality.endSeconds;
        reference.analysisConfidence = tone.report.confidence.aggregate;
        reference.target = TargetInstrument::guitar;
        reference.gainCharacter = chosen.gainCharacter;
        reference.quality = chosen.quality;
        reference.tone = tone;

        // The region stands in for the user's DI, which is what the plug-in does when it has no
        // separate clean track -- and what its own warnings say it is doing.
        const std::vector<float> di = normalized.audio.left;
        RigReconstructor reconstructor;
        const auto result = reconstructor.reconstruct(reference, di, workingSampleRate, 3);
        std::cout << "  best " << std::fixed << std::setprecision(1)
                  << chosen.quality.startSeconds << "-" << chosen.quality.endSeconds << "s  "
                  << std::string(toString(chosen.gainCharacter)) << "  -> ";
        if (! result.success || result.candidates.empty())
        { std::cout << "no candidate: " << result.error << std::endl; continue; }
        const auto voicing = std::string(nts::amp::topologyName(
            result.candidates.front().rigPreset.parameters.topology));
        ++voicingTally[voicing];
        std::cout << voicing << std::endl;
    }

    std::cout << std::endl << "region characters, all songs: ";
    for (const auto& entry : characterTally) std::cout << entry.first << "=" << entry.second << " ";
    std::cout << std::endl << "winning voicings:             ";
    for (const auto& entry : voicingTally) std::cout << entry.first << "=" << entry.second << " ";
    std::cout << std::endl;
    return 0;
}
