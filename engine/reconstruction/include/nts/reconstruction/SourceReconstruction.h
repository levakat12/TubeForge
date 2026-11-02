#pragma once

#include <nts/amp/TraditionalAmp.h>
#include <nts/ir/CabinetMatch.h>
#include <nts/ml/PackedFeatureRegressor.h>
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
/** Bumped to v2 when the export gained its `production` block.

    A reader of a v1 file must treat the absent block as "no production estimate" rather than as
    zeros: zeros are a measurement saying the record did nothing, and that is a different claim
    from having not measured. `ProductionProfile::valid` carries the distinction.
*/
inline constexpr std::string_view currentReconstructionVersion = "tubeforge-reconstruction-v2";

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

/** Which stereo mode to analyse a target stem through.

    `fullStereo` for anything that reads as one source, which is the safe general answer and what
    this used to be pinned to. A hard-panned double-tracked rhythm part is the case that needs
    something else: analysing both takes at once measures their sum, and the sum's crest factor and
    attack time belong to the arrangement rather than to the amplifier. One side alone is a real
    performance through a real rig, so that is what gets analysed.

    The side is chosen by transient peak energy -- the sharper-picked channel -- rather than by
    total energy, because the axis this protects is the transient one. `mid` is never recommended:
    summing two hard-panned takes with micro-delays between them comb-filters, which is the
    failure it would be picked to avoid.

    Returns `fullStereo` for mono or near-silent input, so a caller can use it unconditionally.
*/
[[nodiscard]] StereoMode recommendStereoMode(const StereoAudio& audio,
                                             float doubleTrackingLikelihood);
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

/** What the record did to the guitar after the amplifier.

    Deliberately separate from `DiAdaptation`, which describes the *input* side and is folded into
    the amp preset. This describes the output side and is never folded into anything: the search
    must not see it (see `productionPenalty` in the ranking, and the boundary test that pins this),
    and the user must be able to refuse it.

    Every field is an estimate that can be wrong in a way that is audible -- a residual measured
    against a badly matched cabinet is cabinet error wearing a mix EQ's name, and a room estimate
    from an artifact-heavy stem is measuring the separator. `confidence` is what the interface has
    to say so with, and `valid` distinguishes "measured as nothing" from "not measured".
*/
struct ProductionProfile
{
    /// Where the guitar sat relative to the analysis level, in dB.
    float levelOffsetDb {};
    /** Sixth-octave residual of reference minus matched render, in dB.

        Levelled to its own mean by `measureCabinetResidual`, so it is a *shape* rather than a
        loudness and composes with `levelOffsetDb` instead of double-counting it.
    */
    std::vector<float> spectralResidualDb;
    std::vector<float> residualFrequencies;
    /// 0..1 estimates taken from `ToneFeatures::spatial` and `::dynamic`.
    float roomAmount {};
    float delayAmount {};
    float doubling {};
    float stereoWidth {};
    float compression {};
    /// How much of this to believe, 0..1. Low means the interface should say so.
    float confidence {};
    /** True when a validated model produced the four amounts above, false when the built-in
        mapping did.

        Carried so the interface can say which one ran, which is the same promise the neural
        separator makes when it falls back to the CPU path. A user comparing two matches on two
        machines has to be able to find out why they differ, and "one of them had a model
        installed" is the answer more often than anything about the audio.

        Only the four 0..1 amounts are ever learned. `levelOffsetDb` and the spectral residual are
        recovered exactly by measurement -- 0.000 dB and 0.031 dB RMS in `nts_production_recovery`
        -- and putting an approximation in front of an exact answer would be a straight loss.
    */
    bool learned {};
    bool valid {};
};

/// Length of the vector `productionFeatures` builds. Seven spatial, nine dynamic, 41 residual.
inline constexpr std::size_t productionFeatureCount = 7 + 9 + 41;

/** The model's input vector, in the order `nts_ml.models.FEATURE_NAMES` declares.

    **One definition, because this order is a contract with a trained artifact.** The weights were
    fitted against columns in this sequence; a model outlives the code that trained it, so a field
    inserted into `SpatialFeatures` must not be able to silently shift every later column of an
    artifact already on somebody's disk. Written out field by field rather than copied from the
    structs for exactly that reason -- a struct layout is not a contract and this is.

    Returns an empty vector when the profile carries no residual, because a feature vector padded
    with zeros where a spectrum should be is a valid-looking input describing a measurement that
    did not happen.
*/
[[nodiscard]] std::vector<float> productionFeatures(const nts::tone::ToneAnalysisResult& tone,
                                                     const ProductionProfile& profile);

/** The band range the spectral residual is measured over, which is not the same for both targets.

    **The default 70 Hz floor is a guitar decision and is wrong for a bass.** `CabinetMatchOptions`
    holds the residual flat below its low limit because, in a *guitar* stem, everything down there
    is bass and kick leaking in. Point the same measurement at a bass and that reasoning inverts:
    41 Hz is a low E and 31 Hz is the low B of a five-string, so a 70 Hz floor discards the whole
    fundamental octave of the instrument being measured and reports the mix's effect on the
    harmonics alone.

    The bass floor is 30 Hz for that reason. Kick drum still leaks there and there is no way around
    it -- a separator that could keep them apart perfectly would not need this plug-in -- so a bass
    residual carries more of the drum kit than a guitar one does, and `confidence` is the only
    honest place that shows up.

    The band *count* is unchanged, so the feature vector is the same length for both. What changes
    is what each column means: 41 bands over 30 Hz to 8 kHz is about a fifth of an octave each
    against the guitar range's sixth. **A model trained on one target therefore cannot read the
    other's residual**, which is why the weights are per-instrument rather than one artifact with
    an instrument flag.
*/
[[nodiscard]] nts::ir::CabinetMatchOptions productionResidualOptions(TargetInstrument target) noexcept;

/** Measures what the record did to the guitar that the matched amplifier does not account for.

    Called *after* a candidate has been chosen, never during the search -- see the boundary test in
    `nts_reconstruction_tests`. `candidateRender` is the user's DI through the chosen rig, and
    `referenceMono` is the region that rig was matched from. Either may be normalised or not:
    `measureCabinetResidual` levels both spectra to their own means, so only the shape survives.

    `candidateRender` may be empty, in which case the spectral residual is left empty and the rest
    of the profile is still populated: level, room and dynamics do not need a render to measure.
*/
[[nodiscard]] ProductionProfile estimateProductionProfile(const ReferenceNormalization& normalized,
                                                           const nts::tone::ToneAnalysisResult& tone,
                                                           std::span<const float> referenceMono,
                                                           std::span<const float> candidateRender,
                                                           double sampleRate,
                                                           TargetInstrument target = TargetInstrument::guitar,
                                                           const nts::ml::PackedFeatureRegressor* model = nullptr);

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
    /** What the chosen region measured as, as the enum rather than as its label.

        It was a `std::string` built by `toString` at the call site, which meant the only
        machine-readable form of this fact was a display label -- so the rig search could not use
        it without parsing English back into a decision, and reached for
        `tone.report.context.gainCategory` instead. That reading is miscalibrated: a pure
        undistorted sine measures 0.475 on its 0..1 gain axis where `clean` requires below 0.16,
        so clean and edge-of-breakup are unreachable and nearly everything lands in crunch. This
        one is measured from adjacent-sample slew, zero-crossing rate and crest factor on the
        region audio, and it does discriminate. It is also the value the interface already shows
        beside the region, so the search and the label now agree by construction.
    */
    GainCharacter gainCharacter { GainCharacter::clean };
    float pitchConfidence {};
    float tuningOffsetCents {};
    nts::tone::ToneAnalysisResult tone;
    RegionQuality quality;
    /** What the mix did after the amplifier. Ignored by `reconstruct()` by construction.

        Carried on the reference rather than on a candidate because it is a property of the record,
        not of any one rig fitted to it, and because the export writes it once beside the region it
        was measured from.
    */
    ProductionProfile production;
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

/** The amplifier voicings a reconstruction will consider for an instrument, in enum order.

    Guitar gets the seven that suit both instruments; bass gets those plus the six that were
    designed for it. `nts::amp::topologyAffinity` is the source of that split.

    Published rather than kept private because the count is a *cost*: the search renders the
    refined winning point under every voicing it did not already reach, so this list's length sets
    how long a reconstruction takes and is worth being able to ask about -- and worth being able
    to test, since a voicing wrongly included does not fail, it quietly takes a shortlist slot
    from an amplifier the player might actually have used.
*/
/** TEMPORARY EXPERIMENT -- restrict a bass search to bass-native voicings only.

    **Set this back to `false` to restore the shipped behaviour.** That is the only step; nothing
    else in the search depends on it, and the assertions in `nts_reconstruction_tests` that cover
    this switch say which side of it they are testing.

    With it `true`, choosing Bass in Song Match searches the six bass-native voicings and cannot
    return a guitar amplifier at all. With it `false`, bass searches all thirteen -- a bass-native
    voicing first, thanks to `instrumentNativeBonus`, but a guitar voicing that fits the measured
    gain character markedly better can still win.

    **This is a preference, not a correctness fix, and the difference matters.** The ordering bug
    that `instrumentNativeBonus` addresses was a defect: instrument affinity never reached the sort
    at all, so a bass-native voicing could not win a tie under any input. *This* switch is a
    judgement about what a bass player should be offered, and it is not backed by a measurement --
    `nts_rig_recovery --bass` scores recovery against the pool it is given, so narrowing the pool
    raises the chance baseline from 7.7% to 16.7% and the two numbers are not comparable. Do not
    read a recovery figure taken with this on as evidence that it helped.

    Two consequences worth knowing before this is left on:

    - A bass part genuinely recorded through a guitar amplifier -- which happens, and is a sound
      people go looking for -- becomes unreachable through Song Match on the Bass target.
    - `nts_production_recovery --bass` builds its render pool from this function, so the bass
      training corpus narrows from 156 renders to 72. The bass estimator currently in
      `build/production-estimator/bass` was trained across all thirteen; retraining with this on
      fits a narrower amplifier distribution.
*/
inline constexpr bool bassSearchesBassVoicingsOnly = true;

[[nodiscard]] std::vector<nts::amp::Topology> searchableTopologies(nts::amp::Instrument instrument);

/** How well a voicing suits a measured gain category, in [-1, +1].

    **A prior on the search, not a filter on it.** The rig search renders the winning point under
    every searchable voicing and ranks the results, and before this every voicing entered that
    comparison as an equal. That is wrong in a specific and audible way: a distorted reference and
    a clean one are not equally likely to have come from any of the thirteen, and a comparison
    that pretends otherwise lets a small similarity wobble hand a wall-of-guitars part to Studio
    Direct, or a clean part to Sagging Rectifier. The measured gain category is real evidence
    about which amplifier was in the room, and it was being thrown away.

    Deliberately a weight rather than an eligibility rule. Restricting the pool by gain category
    would make some voicings unreachable for some material, which is the failure `searchableTopologies`
    already refuses for the instrument axis and refuses here for the same reason -- players
    genuinely do record clean parts through high-gain amps at low drive, and a search that cannot
    return that is a search that is wrong with confidence. Scaled by `topologyGainPriorWeight`,
    a voicing the reference argues against still wins when it matches clearly better.

    The values are reasoned from what each voicing *is* rather than fitted: `studioDirect` and
    `americanClean` carry the most headroom and so answer for clean; `tightModern` and
    `saggingRectifier` are the two high-gain guitar voicings; `classAChime` is cathode-biased with
    almost no feedback and so peaks at edge-of-breakup rather than at either extreme. The
    bass-native block is scored on the same reading of its own entries.
*/
[[nodiscard]] float topologyGainAffinity(nts::amp::Topology topology,
                                         GainCharacter character) noexcept;

/** Score weight applied to `topologyGainAffinity` when ranking candidates.

    The rank it joins is `tone * 0.7 + recording * 0.3 - complexityPenalty`, where the similarities
    are 0..1 and a complexity step is 0.025. At 0.05 the prior spans 0.1 between the voicing a
    reference most argues for and the one it most argues against -- larger than a complexity step,
    so it decides ties and near-ties, and smaller than a clear similarity win, so it cannot
    overrule the measurement. Raise it to bias harder toward the expected family; set it to zero
    to restore the previous behaviour exactly.
*/
inline constexpr float topologyGainPriorWeight = 0.05f;

/** `searchableTopologies`, ordered so the voicing the measurement most argues for comes first.

    **The order is not cosmetic: the first entry is the one the search actually refines around.**
    `coarsePoint` walks topology as its slowest digit -- `(variant / 45) % size` -- and the pool is
    twelve by default, so the coarse pass never advances past index 0. Everything after it is
    coordinate descent on that one voicing's parameters, and only then does the topology re-check
    render the refined point under the others. Whichever voicing sits first therefore gets a real
    home advantage: the point was fitted for it.

    That advantage used to go to `tightModern` for every reconstruction, because it is first in the
    enum. A clean part was refined in high-gain territory and Studio Direct was then asked to
    compete at coordinates chosen for a Rectifier, which is a strange way to look for a clean amp.
    Ordering by affinity does not add a bias so much as point the existing one somewhere defensible.

    Ranking is untouched by this and every voicing is still rendered and still reachable -- this
    decides where the search *starts looking*, `topologyGainAffinity` decides how the results are
    weighed, and the two are deliberately separate knobs.

    Ties keep enum order (the sort is stable), so a reconstruction stays reproducible.
*/
[[nodiscard]] std::vector<nts::amp::Topology> searchOrderForGain(nts::amp::Instrument instrument,
                                                                 GainCharacter character);

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
