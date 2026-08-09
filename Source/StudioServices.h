#pragma once

#include <nts/amp/TraditionalAmp.h>
#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/tone/ToneProfileDatabase.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/** The offline half of the studio: tone analysis and song reconstruction.

    Neither of these has anything to do with the audio callback. They read files, run for
    seconds to minutes on worker threads, and publish results the editor polls. Keeping them
    in the AudioProcessor meant its lifetime, its mutexes and its threads were entangled with
    the real-time engine, and meant neither could be exercised without standing up a whole
    plug-in.

    The only thing this needs from the processor is a way to push a recovered rig back onto
    the host parameters, which arrives as a callback rather than a back-reference so the
    dependency runs one way.
*/
class StudioServices
{
public:
    /** Applies a recovered amplifier setting to the host's parameters.

        Called on the message thread. Supplied by the owner because parameter objects belong
        to the processor, and reaching back for them is what this separation exists to avoid.

        `isolateChain` asks for the parts of the live rig the offline render never contained --
        the pedals in front and the time effects behind -- to be taken out of the way, so what
        is heard is the amplifier that was actually scored rather than that amplifier through
        whatever else happened to be set up.

        Reporting what is still in the chain afterwards belongs to the owner as well, for the
        same reason: it means reading the live parameters, which this class deliberately
        cannot do.
    */
    using ApplyRig = std::function<void(const nts::amp::AmpParameters&, bool isolateChain)>;

    explicit StudioServices(ApplyRig applyRig);
    ~StudioServices();

    StudioServices(const StudioServices&) = delete;
    StudioServices& operator=(const StudioServices&) = delete;

    // Tone analysis.
    void requestToneAnalysis(const juce::File& audioFile);
    [[nodiscard]] juce::String toneAnalysisStatusText() const;
    [[nodiscard]] juce::String toneNearestProfileText() const;
    [[nodiscard]] std::optional<nts::tone::ToneAnalysisResult> toneAnalysisSnapshot() const;
    [[nodiscard]] std::size_t toneProfileCount() const;

    // Song reconstruction.
    void requestSongReconstruction(const juce::File& songFile,
                                   nts::reconstruction::TargetInstrument target,
                                   nts::reconstruction::StereoMode stereoMode);
    void cancelSongReconstruction();
    [[nodiscard]] juce::String reconstructionStatusText() const;
    [[nodiscard]] float reconstructionProgress() const noexcept
    { return reconstructionProgressValue.load(std::memory_order_relaxed); }
    /** True from the moment the worker starts until it leaves, however it leaves.

        Distinct from `reconstructionProgress`, which cannot answer this: progress is 0 both before
        a match begins and after one fails, so a caller watching it could not tell idle from
        running, and anything gated on it would latch on the first failure.
    */
    [[nodiscard]] bool songMatchInProgress() const noexcept
    { return reconstructionRunning.load(std::memory_order_relaxed); }
    /** Counts published reconstruction results, so a caller can tell a new one from a re-read.

        Auto Match is the caller: it applies the winning candidate the moment a match completes,
        and the only way to distinguish "a result exists" from "a result I have not applied yet"
        is a number that changes. Starts at 0, which no completed run ever has.
    */
    [[nodiscard]] std::uint64_t reconstructionGeneration() const noexcept
    { return reconstructionGenerationValue.load(std::memory_order_acquire); }
    [[nodiscard]] std::optional<nts::reconstruction::ReconstructionResult> reconstructionSnapshot() const;
    [[nodiscard]] std::vector<nts::reconstruction::PlayableRegion> reconstructionRegionsSnapshot() const;
    [[nodiscard]] bool requestReconstructionRegion(std::size_t index);
    /** Re-runs the last match against the other instrument.

        Not a re-apply of the rig that was found: `searchableTopologies` offers a different set
        of voicings per instrument -- guitar gets seven, bass gets those plus six designed for
        it -- so a guitar match carried onto a bass rig holds values the search would never have
        chosen. The stems are keyed by the song and the target, so switching target re-separates
        or hits the cache; either way it is worker-thread work and returns immediately.

        False when there is no song to re-run against, or when the target has not changed.
    */
    [[nodiscard]] bool requestReconstructionInstrument(nts::reconstruction::TargetInstrument target);
    [[nodiscard]] bool applyReconstructionCandidate(std::size_t index, bool isolateChain);
    [[nodiscard]] juce::Result exportReconstruction(const juce::File& file) const;

    /** The tone of the song a reconstruction was matched against, if there is one.

        A narrow accessor rather than reconstructionSnapshot() because the assistant asks for
        this several times a minute and does not need the candidates, regions and stems that
        come with the whole result.
    */
    [[nodiscard]] std::optional<nts::tone::ToneAnalysisResult> reconstructionReferenceTone() const;

    /** What the last match was made from, named the way the user would name it.

        The song's file name without its extension, or empty when nothing has been matched.
        Exists so that Auto Match can say *which* analysis is holding a control -- "the match"
        is not an answer when the user has run three of them this afternoon.
    */
    [[nodiscard]] juce::String reconstructionSourceName() const;
    /** The reference audio the last match was fitted from, and its rate. Empty when none.

        For the cabinet matcher, which needs the reference itself rather than a description of it.
        Copied out under the lock; a few seconds of mono at the working rate is a few hundred
        kilobytes, and the alternative is handing a caller a pointer into state a worker rewrites.
    */
    [[nodiscard]] std::vector<float> reconstructionReferenceSamples(double& sampleRate) const;

private:
    void analyzeToneFile(std::stop_token stopToken, juce::File audioFile);
    void reconstructSongFile(std::stop_token stopToken, juce::File songFile,
                             nts::reconstruction::TargetInstrument target,
                             nts::reconstruction::StereoMode stereoMode,
                             std::optional<std::size_t> regionOverride = std::nullopt);
    [[nodiscard]] std::optional<nts::reconstruction::StemSet> runMlStemSeparation(
        const juce::File& songFile, const nts::reconstruction::StereoAudio& mixture,
        nts::reconstruction::TargetInstrument target, std::stop_token stopToken,
        std::string& failureReason);

    ApplyRig applyRig;

    nts::tone::ToneAnalyzer toneAnalyzer;
    nts::tone::ToneProfileDatabase toneProfiles;
    mutable std::mutex toneAnalysisMutex;
    std::optional<nts::tone::ToneAnalysisResult> latestToneAnalysis;
    std::string toneAnalysisStatus { "Choose a guitar or bass recording to analyze" };
    std::string toneNearestProfile { "Profile library is empty" };
    std::jthread toneAnalysisWorker;

    nts::reconstruction::StemSeparator stemSeparator;
    nts::reconstruction::RigReconstructor rigReconstructor;
    mutable std::mutex reconstructionMutex;
    std::optional<nts::reconstruction::ReconstructionResult> latestReconstruction;
    /** The normalised reference audio the last reconstruction was fitted from, mono.

        Retained so the cabinet matcher has something to measure against. Kept here rather than in
        `ReconstructionResult` deliberately: that structure is serialised into an exported profile,
        and a few seconds of somebody's copyrighted recording is precisely the thing that must not
        travel with a shared rig. This copy never leaves the process.
    */
    std::vector<float> reconstructionReferenceAudio;
    double reconstructionReferenceRate { 48000.0 };
    std::vector<nts::reconstruction::PlayableRegion> reconstructionRegions;
    juce::File lastReconstructionSong;
    nts::reconstruction::TargetInstrument lastReconstructionTarget { nts::reconstruction::TargetInstrument::guitar };
    nts::reconstruction::StereoMode lastReconstructionStereoMode { nts::reconstruction::StereoMode::fullStereo };
    std::size_t activeReconstructionRegion {};
    std::string reconstructionStatus { "Import a song to begin source reconstruction" };
    std::atomic<float> reconstructionProgressValue {};
    /// Set for the lifetime of a reconstructSongFile call; see songMatchInProgress.
    std::atomic<bool> reconstructionRunning {};
    std::atomic<std::uint64_t> reconstructionGenerationValue {};
    std::jthread reconstructionWorker;
};
