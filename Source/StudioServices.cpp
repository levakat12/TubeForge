#include "StudioServices.h"

#include <nts/reconstruction/StemRefinement.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace
{
bool decodeStemFile(const juce::File& file, double targetSampleRate, std::size_t targetSamples,
                    nts::reconstruction::StereoAudio& result, std::string& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0
        || reader->lengthInSamples > std::numeric_limits<int>::max()
        || targetSamples > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        error = "Could not decode neural stem: " + file.getFileName().toStdString();
        return false;
    }
    const auto sourceSamples = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> decoded(static_cast<int>(std::min<unsigned int>(2, reader->numChannels)), sourceSamples);
    if (! reader->read(&decoded, 0, sourceSamples, 0, true, reader->numChannels > 1))
    {
        error = "Could not read neural stem: " + file.getFileName().toStdString();
        return false;
    }
    result.sampleRate = targetSampleRate;
    result.left.assign(targetSamples, 0.0f);
    result.right.assign(targetSamples, 0.0f);
    const auto outputSamples = static_cast<int>(targetSamples);
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto sourceChannel = std::min(channel, decoded.getNumChannels() - 1);
        auto& destination = channel == 0 ? result.left : result.right;
        if (std::abs(reader->sampleRate - targetSampleRate) < 0.01)
        {
            const auto copyCount = std::min(sourceSamples, outputSamples);
            std::copy_n(decoded.getReadPointer(sourceChannel), copyCount, destination.begin());
        }
        else
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.process(reader->sampleRate / targetSampleRate,
                                 decoded.getReadPointer(sourceChannel), destination.data(),
                                 outputSamples, sourceSamples, 0);
        }
    }
    return true;
}
} // namespace

StudioServices::StudioServices(ApplyRig applyRigToUse) : applyRig(std::move(applyRigToUse)) {}

StudioServices::~StudioServices()
{
    // Both workers are cancellable and can be mid-file; ask them to stop before the members
    // they reference start being destroyed.
    if (toneAnalysisWorker.joinable()) toneAnalysisWorker.request_stop();
    if (reconstructionWorker.joinable()) reconstructionWorker.request_stop();
}

void StudioServices::requestToneAnalysis(const juce::File& audioFile)
{
    if (toneAnalysisWorker.joinable()) toneAnalysisWorker.request_stop();
    {
        const std::scoped_lock lock(toneAnalysisMutex);
        toneAnalysisStatus = "Analyzing " + audioFile.getFileName().toStdString() + " off the audio thread...";
    }
    toneAnalysisWorker = std::jthread([this, audioFile](std::stop_token stopToken)
    {
        analyzeToneFile(stopToken, audioFile);
    });
}

void StudioServices::analyzeToneFile(std::stop_token stopToken, juce::File audioFile)
{
    const auto fail = [this](std::string message)
    {
        const std::scoped_lock lock(toneAnalysisMutex);
        toneAnalysisStatus = std::move(message);
    };
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(audioFile));
    if (reader == nullptr) { fail("Could not decode the selected audio file"); return; }
    const auto maximumSamples = static_cast<juce::int64>(reader->sampleRate * 60.0);
    const auto samples = static_cast<int>(std::min(reader->lengthInSamples, maximumSamples));
    if (samples <= 0 || reader->numChannels == 0) { fail("The selected audio file is empty"); return; }
    juce::AudioBuffer<float> audio(static_cast<int>(std::min<unsigned int>(2, reader->numChannels)), samples);
    if (!reader->read(&audio, 0, samples, 0, true, reader->numChannels > 1))
    { fail("Could not read samples from the selected audio file"); return; }
    if (stopToken.stop_requested()) return;
    const auto left = std::span<const float>(audio.getReadPointer(0), static_cast<std::size_t>(samples));
    const auto right = audio.getNumChannels() > 1
        ? std::span<const float>(audio.getReadPointer(1), static_cast<std::size_t>(samples))
        : std::span<const float> {};
    auto result = toneAnalyzer.analyze({ left, right, reader->sampleRate,
                                         nts::tone::SourceType::userRecording, 1.0f });
    if (stopToken.stop_requested()) return;
    if (!result.success) { fail(result.error); return; }

    const auto fingerprint = static_cast<juce::int64>(audioFile.getFullPathName().hashCode64()
        ^ audioFile.getSize() ^ audioFile.getLastModificationTime().toMilliseconds());
    nts::tone::ToneProfile profile;
    profile.profileId = "user-" + juce::String::toHexString(fingerprint).toStdString();
    profile.name = audioFile.getFileNameWithoutExtension().toStdString();
    profile.embedding = result.embedding; profile.features = result.features; profile.report = result.report;
    profile.instrument = result.report.context.instrument; profile.sourceType = nts::tone::SourceType::userRecording;
    profile.captureQuality = result.report.confidence.aggregate; profile.modelVersion = result.analysisVersion;
    profile.userTags = { "user-analysis" };
    profile.licensingMetadata = "user-supplied audio; rights and redistribution permission not verified";
    const std::scoped_lock lock(toneAnalysisMutex);
    const auto nearest = toneProfiles.search(profile, 1);
    toneNearestProfile = nearest.empty() ? "First profile in this session"
        : "Nearest: " + nearest.front().name + "  (" + std::to_string(
            static_cast<int>(std::lround(nearest.front().similarity.score * 100.0f))) + "% match)";
    std::string databaseError;
    if (!toneProfiles.addOrReplace(std::move(profile), databaseError))
    { toneAnalysisStatus = databaseError; return; }
    latestToneAnalysis = std::move(result);
    toneAnalysisStatus = "Analysis complete: " + audioFile.getFileName().toStdString();
}

juce::String StudioServices::toneAnalysisStatusText() const
{
    const std::scoped_lock lock(toneAnalysisMutex);
    return juce::String::fromUTF8(toneAnalysisStatus.c_str());
}

juce::String StudioServices::toneNearestProfileText() const
{
    const std::scoped_lock lock(toneAnalysisMutex);
    return juce::String::fromUTF8(toneNearestProfile.c_str());
}

std::optional<nts::tone::ToneAnalysisResult> StudioServices::toneAnalysisSnapshot() const
{
    const std::scoped_lock lock(toneAnalysisMutex);
    return latestToneAnalysis;
}

std::size_t StudioServices::toneProfileCount() const
{
    const std::scoped_lock lock(toneAnalysisMutex);
    return toneProfiles.size();
}

void StudioServices::requestSongReconstruction(
    const juce::File& songFile, nts::reconstruction::TargetInstrument target,
    nts::reconstruction::StereoMode stereoMode)
{
    if (reconstructionWorker.joinable()) reconstructionWorker.request_stop();
    reconstructionProgressValue.store(0.0f, std::memory_order_relaxed);
    {
        const std::scoped_lock lock(reconstructionMutex);
        latestReconstruction.reset();
        reconstructionRegions.clear();
        lastReconstructionSong = songFile;
        lastReconstructionTarget = target;
        lastReconstructionStereoMode = stereoMode;
        activeReconstructionRegion = 0;
        reconstructionStatus = "Decoding " + songFile.getFileName().toStdString() + " off the audio thread...";
    }
    reconstructionWorker = std::jthread([this, songFile, target, stereoMode](std::stop_token stopToken)
    {
        reconstructSongFile(stopToken, songFile, target, stereoMode, std::nullopt);
    });
}

void StudioServices::cancelSongReconstruction()
{
    if (reconstructionWorker.joinable()) reconstructionWorker.request_stop();
    const std::scoped_lock lock(reconstructionMutex);
    reconstructionStatus = "Reconstruction cancellation requested";
}

void StudioServices::reconstructSongFile(
    std::stop_token stopToken, juce::File songFile,
    nts::reconstruction::TargetInstrument target,
    nts::reconstruction::StereoMode stereoMode,
    std::optional<std::size_t> regionOverride)
{
    /* Cleared on every exit path, which is why it is a guard rather than two stores.

       This function leaves from about two dozen places -- every `fail(...)` and every cancellation
       check returns early -- and a flag the UI veils itself on must not be able to stay set. Set
       by hand at the top and cleared by hand at the bottom, the first failure would have left the
       amplifier, tone and pedal pages greyed out for the rest of the session. */
    struct RunningGuard
    {
        std::atomic<bool>& flag;
        explicit RunningGuard(std::atomic<bool>& target) : flag(target)
        { flag.store(true, std::memory_order_relaxed); }
        ~RunningGuard() { flag.store(false, std::memory_order_relaxed); }
    } runningGuard { reconstructionRunning };

    const auto fail = [this](std::string message)
    {
        reconstructionProgressValue.store(0.0f, std::memory_order_relaxed);
        const std::scoped_lock lock(reconstructionMutex);
        reconstructionStatus = std::move(message);
    };
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(songFile));
    if (reader == nullptr) { fail("Could not decode the selected song; decoder support may be unavailable"); return; }
    const auto maximumSamples = static_cast<juce::int64>(reader->sampleRate * 60.0 * 20.0);
    const auto sourceSamples64 = std::min(reader->lengthInSamples, maximumSamples);
    if (sourceSamples64 <= 0 || sourceSamples64 > std::numeric_limits<int>::max() || reader->numChannels == 0)
    { fail("The selected song is empty or too long for the offline cache"); return; }
    const auto sourceSamples = static_cast<int>(sourceSamples64);
    juce::AudioBuffer<float> decoded(static_cast<int>(std::min<unsigned int>(2, reader->numChannels)), sourceSamples);
    if (! reader->read(&decoded, 0, sourceSamples, 0, true, reader->numChannels > 1))
    { fail("Could not read samples from the selected song"); return; }
    if (stopToken.stop_requested()) { fail("Reconstruction cancelled"); return; }
    reconstructionProgressValue.store(0.08f, std::memory_order_relaxed);

    constexpr double workingSampleRate = 48000.0;
    const auto outputSamples64 = static_cast<juce::int64>(std::ceil(
        static_cast<double>(sourceSamples) * workingSampleRate / reader->sampleRate));
    if (outputSamples64 <= 0 || outputSamples64 > std::numeric_limits<int>::max())
    { fail("Resampled song would exceed the offline cache limit"); return; }
    const auto outputSamples = static_cast<int>(outputSamples64);
    nts::reconstruction::StereoAudio song;
    song.sampleRate = workingSampleRate;
    song.left.resize(static_cast<std::size_t>(outputSamples));
    song.right.resize(static_cast<std::size_t>(outputSamples));
    const auto speedRatio = reader->sampleRate / workingSampleRate;
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto sourceChannel = std::min(channel, decoded.getNumChannels() - 1);
        juce::LagrangeInterpolator interpolator;
        auto& destination = channel == 0 ? song.left : song.right;
        interpolator.process(speedRatio, decoded.getReadPointer(sourceChannel), destination.data(),
                             outputSamples, sourceSamples, 0);
    }
    reconstructionProgressValue.store(0.12f, std::memory_order_relaxed);
    std::string mlFailure;
    auto mlStems = runMlStemSeparation(songFile, song, target, stopToken, mlFailure);
    if (stopToken.stop_requested()) { fail("Reconstruction cancelled"); return; }
    nts::reconstruction::StemSet stems;
    if (mlStems)
    {
        stems = std::move(*mlStems);
    }
    else
    {
        {
            const std::scoped_lock lock(reconstructionMutex);
            reconstructionStatus = "ML separator unavailable; running artifact-prone CPU fallback";
        }
        nts::reconstruction::SeparationOptions options;
        stems = stemSeparator.separate(song, options,
            [this](const nts::reconstruction::SeparationProgress& update)
            {
                reconstructionProgressValue.store(0.12f + update.fraction * 0.48f, std::memory_order_relaxed);
                const std::scoped_lock lock(reconstructionMutex);
                reconstructionStatus = update.stage;
                return true;
            }, stopToken);
        if (! mlFailure.empty()) stems.warnings.emplace_back("ML fallback: " + mlFailure);
    }
    if (! stems.success) { fail(stems.error); return; }

    // Separation backends decode each source independently, so the same snare sits
    // in the drum stem and in the guitar stem at once and nothing downstream can
    // tell them apart. Re-partitioning the mixture across the stems awards each
    // time-frequency bin to whichever source dominates it before any of the tone
    // features are measured.
    {
        const std::scoped_lock lock(reconstructionMutex);
        reconstructionStatus = "Refining stem masks against the mixture";
    }
    const auto refinement = nts::reconstruction::refineStems(stems, song, {},
        [this](const nts::reconstruction::SeparationProgress& update)
        {
            reconstructionProgressValue.store(0.60f + update.fraction * 0.04f, std::memory_order_relaxed);
            return true;
        }, stopToken);
    if (stopToken.stop_requested()) { fail("Reconstruction cancelled"); return; }
    for (const auto& warning : refinement.warnings) stems.warnings.push_back("refinement: " + warning);
    if (refinement.applied)
        stems.warnings.emplace_back("stems re-partitioned with " + refinement.version + " ("
            + std::to_string(refinement.stemsRefined) + " stems, "
            + std::to_string(static_cast<int>(std::lround(refinement.residualShare * 100.0f)))
            + "% of the mixture unexplained)");

    const auto cacheDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("cache").getChildFile("source-reconstruction");
    if (cacheDirectory.createDirectory().wasOk())
    {
        std::unique_ptr<juce::OutputStream> stream = cacheDirectory
            .getChildFile(juce::String(stems.cacheKey) + ".wav").createOutputStream();
        juce::WavAudioFormat format;
        auto writer = format.createWriterFor(stream,
            juce::AudioFormatWriter::Options{}.withSampleRate(workingSampleRate)
                                               .withNumChannels(2).withBitsPerSample(24));
        if (writer != nullptr)
        {
            juce::AudioBuffer<float> cacheAudio(2, outputSamples);
            cacheAudio.copyFrom(0, 0, song.left.data(), outputSamples);
            cacheAudio.copyFrom(1, 0, song.right.data(), outputSamples);
            writer->writeFromAudioSampleBuffer(cacheAudio, 0, outputSamples);
        }
    }
    if (stopToken.stop_requested()) { fail("Reconstruction cancelled"); return; }
    reconstructionProgressValue.store(0.64f, std::memory_order_relaxed);
    auto selected = nts::reconstruction::selectStem(stems, target);
    selected = nts::reconstruction::applyStereoMode(selected, stereoMode);
    const auto qualityRegions = nts::reconstruction::scoreRegions(stems, target, 7.5, 3.75);
    auto playableRegions = nts::reconstruction::analyzePlayableRegions(selected, qualityRegions, target);
    if (playableRegions.empty()) { fail("No playable regions were detected in the selected stem"); return; }
    auto selectedRegion = std::size_t {};
    if (regionOverride && *regionOverride < playableRegions.size()) selectedRegion = *regionOverride;
    else
        selectedRegion = static_cast<std::size_t>(std::distance(playableRegions.begin(),
            std::max_element(playableRegions.begin(), playableRegions.end(), [](const auto& first, const auto& second)
            { return first.quality.confidence < second.quality.confidence; })));
    const auto regionAnalysis = playableRegions[selectedRegion];
    const auto regionQuality = regionAnalysis.quality;
    {
        const std::scoped_lock lock(reconstructionMutex);
        reconstructionRegions = playableRegions;
        activeReconstructionRegion = selectedRegion;
        reconstructionStatus = "Analyzing selected " + std::string(nts::reconstruction::toString(regionAnalysis.gainCharacter))
            + " part: " + regionAnalysis.dominantPitch + ", " + regionAnalysis.estimatedTuning;
    }
    auto region = nts::reconstruction::extractRegion(selected, regionQuality.startSeconds, regionQuality.endSeconds);
    auto normalized = nts::reconstruction::normalizeReference(region, regionQuality.reverbAmount > 0.45f);
    nts::tone::ToneAnalyzer analyzer;
    auto tone = analyzer.analyze({ normalized.audio.left, normalized.audio.right, workingSampleRate,
                                   nts::tone::SourceType::isolatedStem, regionQuality.confidence });
    if (! tone.success) { fail("Tone extraction failed: " + tone.error); return; }

    /* Re-analyse one side alone when the part turns out to be double-tracked.

       Only when the user left the choice at Full stereo, which is the "you decide" setting -- an
       explicit Left, Right, Mid or Side is a decision and is not second-guessed here.

       Two takes summed have a crest factor and an attack time that belong to the arrangement
       rather than to the amplifier, and those two numbers are exactly what the rig search fits its
       dynamics axis from. `dynamicsSeed` corrects for the effect when it has to, but one real
       performance through one real rig is a better reference than a corrected estimate of one.

       Done as a second pass rather than a cheaper up-front guess because the likelihood is
       something the analyser already computes: estimating it separately would mean a second
       definition of it that could drift from the first. The cost is one more analysis of a region
       a few seconds long, against a separation pass that took orders of magnitude longer. */
    if (stereoMode == nts::reconstruction::StereoMode::fullStereo)
    {
        const auto recommended = nts::reconstruction::recommendStereoMode(
            region, tone.features.spatial.doubleTrackingLikelihood);
        if (recommended != nts::reconstruction::StereoMode::fullStereo)
        {
            auto side = nts::reconstruction::applyStereoMode(selected, recommended);
            auto sideRegion = nts::reconstruction::extractRegion(side, regionQuality.startSeconds,
                                                                 regionQuality.endSeconds);
            auto sideNormalized = nts::reconstruction::normalizeReference(
                sideRegion, regionQuality.reverbAmount > 0.45f);
            auto sideTone = analyzer.analyze({ sideNormalized.audio.left, sideNormalized.audio.right,
                                               workingSampleRate, nts::tone::SourceType::isolatedStem,
                                               regionQuality.confidence });
            // Kept only if it actually analysed. A failure here is not worth failing the whole
            // reconstruction over when a usable full-stereo analysis is already in hand.
            if (sideTone.success)
            {
                selected = std::move(side);
                region = std::move(sideRegion);
                normalized = std::move(sideNormalized);
                tone = std::move(sideTone);
                stereoMode = recommended;
                const std::scoped_lock lock(reconstructionMutex);
                reconstructionStatus = std::string("Double-tracked part: analyzing the ")
                    + std::string(nts::reconstruction::toString(recommended)) + " side alone";
            }
        }
    }
    reconstructionProgressValue.store(0.72f, std::memory_order_relaxed);
    nts::reconstruction::ReconstructionReference reference;
    reference.sourceHash = stems.cacheKey;
    reference.regionStartSeconds = regionQuality.startSeconds;
    reference.regionEndSeconds = regionQuality.endSeconds;
    reference.separationModelVersion = stems.modelVersion;
    reference.analysisConfidence = tone.report.confidence.aggregate;
    reference.target = target;
    reference.dominantPitch = regionAnalysis.dominantPitch;
    reference.estimatedTuning = regionAnalysis.estimatedTuning;
    reference.gainCharacter = std::string(nts::reconstruction::toString(regionAnalysis.gainCharacter));
    reference.pitchConfidence = regionAnalysis.pitchConfidence;
    reference.tuningOffsetCents = regionAnalysis.tuningOffsetCents;
    reference.tone = std::move(tone);
    reference.quality = regionQuality;
    auto result = rigReconstructor.reconstruct(reference, normalized.audio.left, workingSampleRate, 4,
        [this](const nts::reconstruction::SeparationProgress& update)
        {
            reconstructionProgressValue.store(0.72f + update.fraction * 0.27f, std::memory_order_relaxed);
            const std::scoped_lock lock(reconstructionMutex);
            reconstructionStatus = update.stage;
            return true;
        }, stopToken);
    if (! result.success) { fail(result.error); return; }
    for (const auto& warning : stems.warnings) result.warnings.push_back(warning);
    result.warnings.emplace_back("user DI adaptation uses the selected reference until a dedicated DI is supplied");
    {
        const std::scoped_lock lock(reconstructionMutex);
        latestReconstruction = std::move(result);
        reconstructionStatus = "Ready: " + reference.gainCharacter + " part, " + reference.dominantPitch
            + ", " + reference.estimatedTuning + "; " + std::to_string(
            latestReconstruction->candidates.size()) + " playable candidates";
    }
    reconstructionProgressValue.store(1.0f, std::memory_order_relaxed);
}

std::optional<nts::reconstruction::StemSet> StudioServices::runMlStemSeparation(
    const juce::File& songFile, const nts::reconstruction::StereoAudio& mixture,
    nts::reconstruction::TargetInstrument target, std::stop_token stopToken,
    std::string& failureReason)
{
    const auto runtimeRoot = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("ml-separation");
    const auto runtimeManifest = runtimeRoot.getChildFile("runtime.json");
    if (! runtimeManifest.existsAsFile())
    {
        failureReason = "runtime is not installed; run scripts/setup-ml-separator.ps1";
        return std::nullopt;
    }
    juce::var runtime;
    if (juce::JSON::parse(runtimeManifest.loadFileAsString(), runtime).failed() || ! runtime.isObject())
    {
        failureReason = "runtime.json is invalid";
        return std::nullopt;
    }
    const juce::File python(runtime.getProperty("python", "").toString());
    const juce::File worker(runtime.getProperty("worker", "").toString());
    const auto model = runtime.getProperty("model", "htdemucs_6s").toString();
    const auto device = runtime.getProperty("device", "auto").toString();
    if (! python.existsAsFile() || ! worker.existsAsFile())
    {
        failureReason = "configured Python or Demucs worker is missing";
        return std::nullopt;
    }

    const auto sourceHash = juce::MD5(songFile).toHexString();
    const auto cacheKey = sourceHash + "-" + model.replaceCharacter(':', '_');
    const auto outputDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("cache").getChildFile("source-reconstruction")
        .getChildFile("ml").getChildFile(cacheKey);
    if (outputDirectory.createDirectory().failed())
    {
        failureReason = "could not create the local neural-stem cache";
        return std::nullopt;
    }
    const auto outputManifest = outputDirectory.getChildFile("manifest.json");
    const auto requiredTarget = target == nts::reconstruction::TargetInstrument::guitar ? "guitar.wav" : "bass.wav";
    const auto cacheReady = outputManifest.existsAsFile()
        && outputDirectory.getChildFile("vocals.wav").existsAsFile()
        && outputDirectory.getChildFile("drums.wav").existsAsFile()
        && outputDirectory.getChildFile(requiredTarget).existsAsFile();
    if (! cacheReady)
    {
        {
            const std::scoped_lock lock(reconstructionMutex);
            reconstructionStatus = "Starting local Demucs neural separator (first run downloads the model)";
        }
        juce::ChildProcess process;
        juce::StringArray arguments { python.getFullPathName(), worker.getFullPathName(),
                                      "--input", songFile.getFullPathName(),
                                      "--output", outputDirectory.getFullPathName(),
                                      "--model", model, "--device", device };
        if (! process.start(arguments))
        {
            failureReason = "could not start the Demucs worker process";
            return std::nullopt;
        }
        auto lastProgressText = juce::String();
        while (process.isRunning())
        {
            if (stopToken.stop_requested())
            {
                process.kill();
                failureReason = "neural separation was cancelled";
                return std::nullopt;
            }
            const auto progressFile = outputDirectory.getChildFile("progress.json");
            if (progressFile.existsAsFile())
            {
                juce::var progress;
                if (juce::JSON::parse(progressFile.loadFileAsString(), progress).wasOk() && progress.isObject())
                {
                    const auto fraction = std::clamp(static_cast<float>(progress.getProperty("fraction", 0.0)), 0.0f, 1.0f);
                    reconstructionProgressValue.store(0.12f + fraction * 0.48f, std::memory_order_relaxed);
                    const auto stage = progress.getProperty("stage", "Running neural separator").toString();
                    if (stage != lastProgressText)
                    {
                        const std::scoped_lock lock(reconstructionMutex);
                        reconstructionStatus = stage.toStdString();
                        lastProgressText = stage;
                    }
                }
            }
            process.waitForProcessToFinish(200);
        }
        const auto output = process.readAllProcessOutput();
        if (process.getExitCode() != 0 || ! outputManifest.existsAsFile())
        {
            failureReason = "Demucs failed: " + output.substring(std::max(0, output.length() - 500)).toStdString();
            return std::nullopt;
        }
    }

    juce::var manifest;
    if (juce::JSON::parse(outputManifest.loadFileAsString(), manifest).failed() || ! manifest.isObject())
    {
        failureReason = "neural stem manifest is invalid";
        return std::nullopt;
    }
    nts::reconstruction::StemSet result;
    result.success = true;
    result.cacheKey = cacheKey.toStdString();
    result.modelVersion = manifest.getProperty("modelVersion", "demucs:unknown").toString().toStdString();
    result.usedGpu = manifest.getProperty("device", "cpu").toString().containsIgnoreCase("cuda");
    result.reconstructionError = static_cast<float>(manifest.getProperty("reconstructionError", 0.0));
    std::string decodeError;
    if (! decodeStemFile(outputDirectory.getChildFile("vocals.wav"), mixture.sampleRate, mixture.samples(),
                         result.vocals, decodeError)
        || ! decodeStemFile(outputDirectory.getChildFile("drums.wav"), mixture.sampleRate, mixture.samples(),
                            result.drums, decodeError))
    {
        failureReason = decodeError;
        return std::nullopt;
    }
    if (target == nts::reconstruction::TargetInstrument::bass)
    {
        if (! decodeStemFile(outputDirectory.getChildFile("bass.wav"), mixture.sampleRate, mixture.samples(),
                             result.bass, decodeError))
        { failureReason = decodeError; return std::nullopt; }
    }
    else
    {
        if (! decodeStemFile(outputDirectory.getChildFile("guitar.wav"), mixture.sampleRate, mixture.samples(),
                             result.guitar, decodeError))
        { failureReason = decodeError; return std::nullopt; }
    }
    if (result.reconstructionError > 0.1f)
        result.warnings.emplace_back("neural stem mixture consistency is lower than expected");
    result.warnings.emplace_back("local " + result.modelVersion + " separation; source audio remains in the private cache");
    failureReason.clear();
    return result;
}

juce::String StudioServices::reconstructionStatusText() const
{
    const std::scoped_lock lock(reconstructionMutex);
    return juce::String::fromUTF8(reconstructionStatus.c_str());
}

std::optional<nts::reconstruction::ReconstructionResult> StudioServices::reconstructionSnapshot() const
{
    const std::scoped_lock lock(reconstructionMutex);
    return latestReconstruction;
}

std::vector<nts::reconstruction::PlayableRegion> StudioServices::reconstructionRegionsSnapshot() const
{
    const std::scoped_lock lock(reconstructionMutex);
    return reconstructionRegions;
}

bool StudioServices::requestReconstructionRegion(std::size_t index)
{
    juce::File song;
    nts::reconstruction::TargetInstrument target;
    nts::reconstruction::StereoMode stereoMode;
    {
        const std::scoped_lock lock(reconstructionMutex);
        if (index >= reconstructionRegions.size() || ! lastReconstructionSong.existsAsFile()) return false;
        song = lastReconstructionSong;
        target = lastReconstructionTarget;
        stereoMode = lastReconstructionStereoMode;
        latestReconstruction.reset();
        activeReconstructionRegion = index;
        reconstructionStatus = "Rebuilding from selected part (cached neural stems)...";
    }
    if (reconstructionWorker.joinable()) reconstructionWorker.request_stop();
    reconstructionProgressValue.store(0.60f, std::memory_order_relaxed);
    reconstructionWorker = std::jthread([this, song, target, stereoMode, index](std::stop_token stopToken)
    { reconstructSongFile(stopToken, song, target, stereoMode, index); });
    return true;
}

bool StudioServices::applyReconstructionCandidate(std::size_t index, bool isolateChain)
{
    nts::reconstruction::RigCandidate candidate;
    {
        const std::scoped_lock lock(reconstructionMutex);
        if (! latestReconstruction || index >= latestReconstruction->candidates.size()) return false;
        candidate = latestReconstruction->candidates[index];
        reconstructionStatus = "Applied " + candidate.rigPreset.name + " for live DI playing";
    }
    const auto& p = candidate.rigPreset.parameters;
    // Handed back to the owner rather than written directly: the parameter objects belong
    // to the processor, and reaching for them from here is what this split removes. Called
    // outside the lock, because it writes host parameters and those can call back into
    // anything.
    if (applyRig) applyRig(p, isolateChain);

    return true;
}

std::optional<nts::tone::ToneAnalysisResult> StudioServices::reconstructionReferenceTone() const
{
    const std::scoped_lock lock(reconstructionMutex);
    if (! latestReconstruction) return std::nullopt;
    return latestReconstruction->reference.tone;
}

juce::Result StudioServices::exportReconstruction(const juce::File& file) const
{
    const std::scoped_lock lock(reconstructionMutex);
    if (! latestReconstruction) return juce::Result::fail("No reconstruction result is available");
    if (! file.replaceWithText(juce::String::fromUTF8(
            nts::reconstruction::serializeResult(*latestReconstruction).c_str())))
        return juce::Result::fail("Could not write the reconstruction package");
    return juce::Result::ok();
}
