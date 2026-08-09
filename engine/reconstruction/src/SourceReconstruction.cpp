#include <nts/reconstruction/SourceReconstruction.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <numbers>
#include <numeric>
#include <span>
#include <sstream>
#include <vector>

namespace nts::reconstruction
{
namespace
{
float clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }
float db(float value) noexcept { return 20.0f * std::log10(std::max(value, 1.0e-9f)); }
float linear(float decibels) noexcept { return std::pow(10.0f, decibels / 20.0f); }

void hashBytes(std::uint64_t& hash, const void* data, std::size_t bytes) noexcept
{
    const auto* source = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < bytes; ++index)
    {
        hash ^= source[index];
        hash *= 1099511628211ull;
    }
}

std::string hexHash(std::uint64_t hash)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

StereoAudio like(const StereoAudio& source)
{
    StereoAudio result;
    result.sampleRate = source.sampleRate;
    result.left.assign(source.samples(), 0.0f);
    result.right.assign(source.samples(), 0.0f);
    return result;
}

float rmsRange(const std::vector<float>& values, std::size_t begin, std::size_t end) noexcept
{
    if (end <= begin || begin >= values.size()) return 0.0f;
    end = std::min(end, values.size());
    double energy = 0.0;
    for (auto index = begin; index < end; ++index) energy += values[index] * values[index];
    return std::sqrt(static_cast<float>(energy / static_cast<double>(end - begin)));
}

float sampleAt(const std::vector<float>& values, std::size_t index) noexcept
{
    return index < values.size() ? values[index] : 0.0f;
}

/** Leakage severity from the target-to-interferer ratio, in dB.

    This used to be `clamp01(interfererRms / targetRms)`, which saturates the moment
    the interferer reaches the target's level. Every region from "slightly leaky" to
    "hopeless" therefore scored exactly 1.0, and region ranking lost its ordering in
    precisely the range where the choice between regions matters. Mapping a 30 dB
    window keeps the whole span ordered: +12 dB of separation or better reads as
    clean, -18 dB or worse as unusable, and the existing 0.55 warning threshold
    still lands at a sensible -4.5 dB.
*/
float leakageSeverity(float targetRms, float interfererRms) noexcept
{
    constexpr float cleanSeparationDb = 12.0f;
    constexpr float severityRangeDb = 30.0f;
    return clamp01((cleanSeparationDb - (db(targetRms) - db(interfererRms))) / severityRangeDb);
}

float correlationRange(const StereoAudio& audio, std::size_t begin, std::size_t end) noexcept
{
    if (audio.right.empty() || end <= begin) return 1.0f;
    double cross = 0.0, leftEnergy = 0.0, rightEnergy = 0.0;
    end = std::min({ end, audio.left.size(), audio.right.size() });
    for (auto index = begin; index < end; ++index)
    {
        cross += audio.left[index] * audio.right[index];
        leftEnergy += audio.left[index] * audio.left[index];
        rightEnergy += audio.right[index] * audio.right[index];
    }
    return static_cast<float>(cross / std::sqrt(std::max(1.0e-18, leftEnergy * rightEnergy)));
}

struct PitchSummary
{
    int dominantMidi { -1 };
    int lowestMidi { -1 };
    float frequency {};
    float confidence {};
    float cents {};
};

std::string pitchName(int midi)
{
    static constexpr std::array names { "C", "C#", "D", "D#", "E", "F",
                                        "F#", "G", "G#", "A", "A#", "B" };
    if (midi < 0) return "Unknown pitch";
    return std::string(names[static_cast<std::size_t>((midi % 12 + 12) % 12)])
        + std::to_string(midi / 12 - 1);
}

PitchSummary estimatePitch(const StereoAudio& audio, std::size_t begin, std::size_t end,
                           TargetInstrument target)
{
    PitchSummary result;
    if (end <= begin || begin >= audio.samples() || audio.sampleRate < 8000.0) return result;
    end = std::min(end, audio.samples());
    const auto downsample = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(audio.sampleRate / 8000.0)));
    const auto analysisRate = audio.sampleRate / static_cast<double>(downsample);
    const auto frameSourceSamples = std::max<std::size_t>(downsample * 256,
        static_cast<std::size_t>(audio.sampleRate * 0.16));
    const auto hop = std::max<std::size_t>(1, static_cast<std::size_t>(audio.sampleRate * 0.10));
    const auto minimumFrequency = target == TargetInstrument::bass ? 30.0 : 55.0;
    const auto minimumLag = std::max(2, static_cast<int>(analysisRate / 900.0));
    const auto maximumLag = std::max(minimumLag + 1, static_cast<int>(analysisRate / minimumFrequency));
    std::array<float, 128> midiWeights {};
    std::vector<float> centsValues;
    float confidenceSum {};
    std::size_t accepted {};
    const auto availableFrames = std::max<std::size_t>(1, (end - begin) / hop);
    const auto frameStride = std::max<std::size_t>(1, availableFrames / 32);
    std::vector<float> frame;
    frame.reserve(frameSourceSamples / downsample + 1);

    std::size_t frameIndex {};
    for (auto frameBegin = begin; frameBegin + frameSourceSamples <= end; frameBegin += hop, ++frameIndex)
    {
        if (frameIndex % frameStride != 0) continue;
        frame.clear();
        for (auto index = frameBegin; index < frameBegin + frameSourceSamples; index += downsample)
        {
            const auto right = audio.right.empty() ? audio.left[index] : audio.right[index];
            frame.push_back(0.5f * (audio.left[index] + right));
        }
        const auto mean = std::accumulate(frame.begin(), frame.end(), 0.0) / std::max<std::size_t>(1, frame.size());
        double energy {};
        for (auto& sample : frame) { sample -= static_cast<float>(mean); energy += sample * sample; }
        if (energy / std::max<std::size_t>(1, frame.size()) < 1.0e-7) continue;

        auto correlationAt = [&frame](int lag)
        {
            double cross {}, first {}, second {};
            for (std::size_t index = static_cast<std::size_t>(lag); index < frame.size(); ++index)
            {
                const auto a = frame[index], b = frame[index - static_cast<std::size_t>(lag)];
                cross += a * b; first += a * a; second += b * b;
            }
            return static_cast<float>(cross / std::sqrt(std::max(1.0e-18, first * second)));
        };
        auto bestLag = minimumLag;
        auto bestCorrelation = -1.0f;
        std::vector<float> correlations(static_cast<std::size_t>(maximumLag + 1), -1.0f);
        for (auto lag = minimumLag; lag <= maximumLag && lag < static_cast<int>(frame.size() / 2); ++lag)
        {
            const auto correlation = correlationAt(lag);
            correlations[static_cast<std::size_t>(lag)] = correlation;
            if (correlation > bestCorrelation) { bestCorrelation = correlation; bestLag = lag; }
        }
        for (auto lag = minimumLag + 1; lag < maximumLag; ++lag)
            if (correlations[static_cast<std::size_t>(lag)] >= bestCorrelation * 0.96f
                && correlations[static_cast<std::size_t>(lag)] >= correlations[static_cast<std::size_t>(lag - 1)]
                && correlations[static_cast<std::size_t>(lag)] >= correlations[static_cast<std::size_t>(lag + 1)])
            { bestLag = lag; bestCorrelation = correlations[static_cast<std::size_t>(lag)]; break; }
        if (bestCorrelation < 0.28f) continue;
        const auto frequency = static_cast<float>(analysisRate / static_cast<double>(bestLag));
        const auto midiFloat = 69.0f + 12.0f * std::log2(frequency / 440.0f);
        const auto midi = std::clamp(static_cast<int>(std::lround(midiFloat)), 0, 127);
        midiWeights[static_cast<std::size_t>(midi)] += bestCorrelation;
        centsValues.push_back((midiFloat - static_cast<float>(midi)) * 100.0f);
        confidenceSum += bestCorrelation;
        ++accepted;
    }
    if (accepted == 0) return result;
    result.dominantMidi = static_cast<int>(std::distance(midiWeights.begin(),
        std::max_element(midiWeights.begin(), midiWeights.end())));
    result.frequency = 440.0f * std::pow(2.0f, (static_cast<float>(result.dominantMidi) - 69.0f) / 12.0f);
    result.confidence = clamp01((confidenceSum / static_cast<float>(accepted))
        * std::min(1.0f, static_cast<float>(accepted) / 6.0f));
    std::sort(centsValues.begin(), centsValues.end());
    result.cents = centsValues[centsValues.size() / 2];
    float cumulative {}, threshold = confidenceSum * 0.12f;
    for (std::size_t midi = 0; midi < midiWeights.size(); ++midi)
    {
        cumulative += midiWeights[midi];
        if (cumulative >= threshold) { result.lowestMidi = static_cast<int>(midi); break; }
    }
    return result;
}

std::string tuningFamily(int lowestMidi)
{
    if (lowestMidi < 0) return "Tuning uncertain";
    switch ((lowestMidi % 12 + 12) % 12)
    {
        case 4: return "Likely E-standard family";
        case 3: return "Likely E-flat family";
        case 2: return "Likely D / Drop-D family";
        case 1: return "Likely C# / Drop-C# family";
        case 0: return "Likely C / Drop-C family";
        case 11: return "Likely B / Drop-B family";
        default: return "Tuning uncertain (open string not observed)";
    }
}

std::pair<GainCharacter, float> estimateGainCharacter(const StereoAudio& audio,
                                                       std::size_t begin, std::size_t end)
{
    end = std::min(end, audio.samples());
    if (end <= begin) return { GainCharacter::clean, 0.0f };

    // Distortion is identified by how fast the waveform moves between *adjacent*
    // samples, so the analysis has to stay on adjacent samples. Reading every
    // stride'th sample instead, as this previously did once a region exceeded
    // five seconds, decimates with no low-pass and aliases away precisely the
    // high-frequency content that separates a distorted tone from a clean one.
    // Long regions therefore scored lower than they should and high-gain parts
    // were classified as crunch or clean. Cost is bounded by covering a long
    // region with contiguous windows spread across it instead.
    constexpr std::size_t analysisBudget = 240000;
    const auto span = end - begin;
    const auto windowCount = span <= analysisBudget ? std::size_t { 1 } : std::size_t { 64 };
    const auto windowLength = std::min(span / windowCount, analysisBudget / windowCount);

    const auto monoAt = [&audio](std::size_t index)
    {
        const auto right = audio.right.empty() ? audio.left[index] : audio.right[index];
        return 0.5f * (audio.left[index] + right);
    };

    double energy {}, derivativeEnergy {};
    float peak {};
    std::size_t samples {}, crossings {};
    for (std::size_t window = 0; window < windowCount; ++window)
    {
        const auto start = windowCount == 1
            ? begin : begin + (span - windowLength) * window / (windowCount - 1);
        const auto stop = std::min(end, start + windowLength);
        auto previous = monoAt(start);
        energy += static_cast<double>(previous) * previous;
        peak = std::max(peak, std::abs(previous));
        ++samples;
        for (auto index = start + 1; index < stop; ++index)
        {
            const auto value = monoAt(index);
            energy += static_cast<double>(value) * value;
            const auto difference = value - previous;
            derivativeEnergy += static_cast<double>(difference) * difference;
            if (std::signbit(value) != std::signbit(previous)) ++crossings;
            peak = std::max(peak, std::abs(value)); previous = value; ++samples;
        }
    }

    const auto rms = std::sqrt(static_cast<float>(energy / std::max<std::size_t>(1, samples)));
    const auto crest = peak / std::max(rms, 1.0e-7f);
    const auto derivativeRatio = std::sqrt(static_cast<float>(derivativeEnergy / std::max(energy, 1.0e-12)));
    // Crossings and duration now come from the same adjacent-sample material, so
    // the rate needs no stride extrapolation to undo the decimation.
    const auto seconds = static_cast<float>(samples) / static_cast<float>(audio.sampleRate);
    const auto crossingRate = static_cast<float>(crossings) / std::max(seconds, 1.0e-6f);
    const auto score = clamp01(clamp01((derivativeRatio - 0.045f) / 0.22f) * 0.60f
        + clamp01((crossingRate - 450.0f) / 2600.0f) * 0.25f
        + clamp01((4.5f - crest) / 2.8f) * 0.15f);
    return { score < 0.34f ? GainCharacter::clean : score < 0.62f ? GainCharacter::crunch
                                                                       : GainCharacter::distorted,
             score };
}

nts::tone::ToneProfile profile(std::string id, const nts::tone::ToneAnalysisResult& analysis)
{
    return { std::move(id), "Reconstruction", analysis.embedding, analysis.features, analysis.report,
             analysis.report.context.instrument, nts::tone::SourceType::userRecording,
             analysis.report.confidence.aggregate, analysis.analysisVersion, { "phase-8" },
             "user-supplied reference; source audio excluded" };
}

/** One point in the rig search space.

    Topology is part of the point rather than derived from tightness, so both
    voicings can be rendered and compared instead of one being unreachable for a
    given reference. The three scalars are absolute 0..1 positions, which lets the
    refinement pass step around a coarse winner without re-deriving it.
*/
struct CandidatePoint
{
    nts::amp::Topology topology { nts::amp::Topology::tightModern };
    float gain {};
    float brightness {};
    float tightness {};
    /** How open the rig's response to a pick should be. 1 is fast and uncompressed.

        Unlike the axes above it starts from a direct measurement -- crest factor and attack
        time -- rather than from a descriptor whose mapping onto amplifier controls is a guess,
        so the coarse grid does not perturb it and the refinement pass does. Seeded by
        `dynamicsSeed`, consumed by `setCandidateParameters` for attack reduction, pick
        emphasis and part of the supply sag.
    */
    float dynamics { 0.5f };
    float driveOffset {};
};

/** Where the dynamics axis starts, from the two features the transient score is built on.

    Normalised against the same references `ToneSimilarity::compare` uses -- 18 dB of crest
    factor and 40 ms of attack -- so a point that scores well on the transient term and a point
    that sits at the measured seed mean the same thing rather than two different things.

    Both halves point the same way: high crest factor and a short attack are the open, dynamic
    end, and the amplifier parameters this drives all read 1 as "get out of the way".

    `doubleTrackingLikelihood` corrects for the reference being two performances rather than one,
    which is the normal case for a hard-panned rhythm guitar. Both measurements are corrupted in
    the same direction by that, and neither is a property of the amplifier:

      - two takes sum with their peaks landing at different moments, so the crest factor of the
        sum is lower than either take's;
      - two pick attacks tens of milliseconds apart read as one attack lasting the gap between
        them, so the measured attack time is longer than either take's.

    Uncorrected, a double-tracked reference therefore fits a rig with more attack softening, less
    pick emphasis and more supply sag than the amplifier in the recording had -- the analyser
    would be measuring the arrangement. The corrections are the size of the effect in typical
    double-tracked material, not derived quantities: about 3 dB of crest factor and about 12 ms of
    apparent attack at full likelihood. Better still is to analyse one side alone, which
    `recommendStereoMode` now steers towards; this is the fallback for when the reference is
    genuinely a summed pair.
*/
float dynamicsSeed(const nts::tone::DynamicFeatures& dynamic,
                   float doubleTrackingLikelihood) noexcept
{
    const auto layered = clamp01(doubleTrackingLikelihood);
    const auto crestDb = dynamic.crestFactorDb + 3.0f * layered;
    const auto attackMs = std::max(0.0f, dynamic.attackMilliseconds - 12.0f * layered);
    const auto crest = clamp01(crestDb / 18.0f);
    const auto attack = clamp01(1.0f - attackMs / 40.0f);
    return clamp01(0.5f * (crest + attack));
}

/** Centres a search axis so a whole grid of steps stays inside 0..1.

    `clamp01(base + step)` folds every step that runs past a rail onto the rail
    itself. A reference measured at full brightness therefore collapsed the entire
    brightness axis to a single value, and the shortlist handed the user several
    identical rigs — the exact failure the mixed-radix indexing below was meant to
    end. Pulling the centre `span` away from each rail keeps every step distinct
    while moving the search centre by at most that same span, which the refinement
    pass can walk back if the rail really was the right answer.
*/
float gridCentre(float base, float span) noexcept
{
    return std::clamp(base, span, 1.0f - span);
}

/** Steps ordered centre-first, so a pool smaller than the whole grid stays unbiased.

    The arithmetic form these replace -- `(variant / radix) % 3 - 1` -- put -1 at index 0.
    Callers ask for `poolSize` *consecutive* variants and `poolSize` is 12 by default, so the
    two slowest digits never advanced: every candidate rendered one step below the measured
    tightness, and ten of twelve below the measured brightness. Brightness drives the pre-EQ
    high cut, the treble control, presence and the post high shelf, so the whole pool came out
    darker than the reference in four places at once and none of it brighter. Centre-first
    ordering means index 0 is the measured value whatever the pool size is.
*/
constexpr std::array<int, 3> centredSteps { 0, -1, 1 };
constexpr std::array<int, 5> centredGainSteps { 0, -1, 1, -2, 2 };

/** Maps a coarse variant index onto a point.

    Digits are ordered by how much a small pool gains from sampling them, because a caller asks
    for `poolSize` *consecutive* variants and the slow digits then never advance. Gain is
    fastest: it has five steps, it is the axis a listener reads as "is this the right amount of
    amplifier", and at the default pool of twelve `variant % 5` covers all five. Brightness
    follows and gets all three. Tightness and topology sit behind them and stay at their centres
    for a default pool -- which is the measured value, so the pool is unbiased even where it is
    unexplored, and the refinement pass steps tightness while the post-refinement re-check
    covers topology.

    An earlier ordering here put brightness fastest and gain on `variant / 9`, which fixed the
    dullness bias but cut gain from five sampled steps to two. Both mattered; this ordering gets
    both. The mixed radix gives 5 x 3 x 3 x however many voicings the instrument can use.

    Dynamics is deliberately absent: it is seeded from a direct measurement rather than a
    guessed mapping, so its centre is already the best coarse answer and perturbing it here
    would cost pool slots that gain and brightness use better. Refinement explores it.
*/
CandidatePoint coarsePoint(const nts::tone::ToneReport& report, float dynamics, std::size_t variant,
                           std::span<const nts::amp::Topology> eligible)
{
    const auto gainStep = centredGainSteps[variant % 5];
    const auto brightnessStep = centredSteps[(variant / 5) % 3];
    const auto tightnessStep = centredSteps[(variant / 15) % 3];
    const auto topologyStep = eligible.empty() ? std::size_t {} : (variant / 45) % eligible.size();
    const auto offset = static_cast<float>(gainStep) * 0.06f;

    CandidatePoint point;
    point.dynamics = dynamics;
    point.topology = eligible.empty() ? nts::amp::Topology::tightModern : eligible[topologyStep];
    point.gain = clamp01(gridCentre(report.gain.value, 0.12f) + offset);
    point.brightness = clamp01(gridCentre(report.brightness.value, 0.12f) - offset * 0.5f
                               + static_cast<float>(brightnessStep) * 0.06f);
    point.tightness = clamp01(gridCentre(report.tightness.value, 0.096f) + offset * 0.3f
                              + static_cast<float>(tightnessStep) * 0.06f);
    point.driveOffset = offset;
    return point;
}

void setCandidateParameters(nts::amp::AmpPreset& preset, const nts::tone::ToneReport& report,
                            const CandidatePoint& point)
{
    auto& p = preset.parameters;
    /* What the voicing itself asked for, captured before anything below overwrites it.

       `p` aliases `preset.parameters`, so a fit that wants to scale a voicing's own value rather
       than replace it has to read it first or it reads its own output. Taken here rather than at
       each use so that reordering the assignments below cannot silently turn one of them into a
       self-reference.
    */
    const auto voicingCrossoverHz = p.bass.crossoverHz;
    std::array<int, 4> voicingOversampling {};
    for (std::size_t stage = 0; stage < voicingOversampling.size(); ++stage)
        voicingOversampling[stage] = p.stages[stage].oversamplingFactor;

    const auto gain = point.gain;
    const auto brightness = point.brightness;
    const auto tightness = point.tightness;
    const auto dynamics = point.dynamics;
    const auto offset = point.driveOffset;
    p.topology = point.topology;
    // Off for the same reason the stages below are oversampled: the analyser has to see the
    // amplifier and not the output stage. The matcher normalises output RMS back to the dry
    // input's with a ~104 ms time constant, which is a slow compressor sitting on the render --
    // and `ToneSimilarity::compare` scores that render on crest factor, loudness range,
    // compression amount and attack time. Left on, the dynamics and transient terms measure the
    // matcher rather than the rig, so a candidate could not be ranked on how it responds to a
    // pick.
    //
    // This preset is also what the user auditions, so the setting ships with the match rather
    // than applying to the render alone. That is deliberate: a rig chosen for its dynamics
    // should not have them flattened on playback by the stage that was excluded from choosing
    // it. Hand-built presets keep the matcher's `true` default.
    p.loudnessMatch = false;
    p.stageCount = gain < 0.2f ? 2 : gain < 0.65f ? 3 : 4;
    for (std::size_t stage = 0; stage < p.stages.size(); ++stage)
    {
        p.stages[stage].driveDb = 2.0f + gain * 28.0f + static_cast<float>(stage) * 2.0f + offset * 20.0f;
        p.stages[stage].bias = (0.5f - report.tightness.value) * 0.45f + offset;
        p.stages[stage].asymmetry = gain * 0.35f;
        /* Attack softening, from the dynamics axis.

           `transientGain = 1 / (1 + 22 * attackReduction * attack)` in the preamp stage, so this
           is literally how much the amplifier rounds off a pick. It was never fitted -- every
           candidate carried whatever `makeOriginalPreset` left -- which is why two references
           with very different attack profiles produced rigs that felt the same. High dynamics
           means a fast, uncompressed reference and therefore little softening. */
        p.stages[stage].attackReduction = clamp01(0.55f - dynamics * 0.45f);
        // Candidates were rendered at 1x, so the tone analyser measured the
        // aliasing of the render rather than the amplifier. Fold-back adds
        // inharmonic high-frequency energy that scales with drive, so high-gain
        // candidates were scored against a spectrum the amp does not actually
        // produce, and the preset handed back sounded unlike the thing that won.
        // Matching playback here is what makes the score mean something.
        //
        // Which is why it is a floor rather than a value. A hard-clipping voicing asks for 8x
        // because a clamped curve folds far harder than a valve one; pinning it to 4 would score
        // exactly the aliasing this line exists to stop measuring, and would do it worst on the
        // candidates most sensitive to it.
        p.stages[stage].oversamplingFactor = std::max(4, voicingOversampling[stage]);
    }
    p.preEq.lowCutHz = p.instrument == nts::amp::Instrument::bass
        ? 28.0f + tightness * 45.0f : 55.0f + tightness * 95.0f;
    p.preEq.highCutHz = 6500.0f + brightness * 9500.0f;
    p.preEq.tightness = tightness;
    // A peaking filter at 2800 Hz -- the amplifier's own pick-attack control, and the other
    // parameter that was never fitted, so it sat at 0 dB for every candidate ever produced.
    // +-3 dB either side of flat, following the dynamics axis: an open reference gets the peak,
    // a compressed one gets it taken away.
    p.preEq.pickEmphasisDb = (dynamics - 0.5f) * 6.0f;
    p.toneStack.bass = clamp01(0.35f + report.cleanLowBlendEstimate.value * 0.35f);
    p.toneStack.mid = clamp01(0.35f + (1.0f - brightness) * 0.35f + offset);
    p.toneStack.treble = clamp01(0.22f + brightness * 0.65f);
    p.powerAmp.masterDb = -12.0f + gain * 10.0f;
    p.powerAmp.saturation = gain;
    // Half from tightness, half from dynamics inverted: sag is supply compression, so an open
    // reference wants less of it. The candidate assembly in `reconstruct` then blends the
    // measured `compression` descriptor in on top of this, which is the sustained counterpart to
    // the transient measurement the dynamics axis carries.
    p.powerAmp.sag = clamp01((0.75f - tightness * 0.55f) * 0.5f + (1.0f - dynamics) * 0.5f);
    p.powerAmp.feedback = clamp01(0.25f + tightness * 0.55f);
    p.powerAmp.presence = brightness;
    p.powerAmp.resonance = report.cleanLowBlendEstimate.value;
    p.cabinet.highCutHz = 5500.0f + (1.0f - report.cabinetDarkness.value) * 8500.0f;
    p.cabinet.blend = clamp01(0.45f + offset);
    p.postLowDb = (report.cleanLowBlendEstimate.value - 0.5f) * 5.0f;
    p.postMidDb = (0.5f - brightness) * 3.0f;
    p.postHighDb = (brightness - 0.5f) * 5.0f;
    /* Fitted *around* the voicing's own crossover rather than replacing it.

       This used to be `90 + tightness * 180`, an absolute 90-270 Hz for every candidate. That is
       a reasonable fit while every voicing splits at 120-220 Hz to keep a low B out of the
       distortion -- and it destroys the one voicing whose split is its identity. Solid-State
       Bi-Amp divides at 500 Hz so that the whole body of a note passes clean and only the attack
       is driven; forced down to 270 it is not a bi-amp being fitted, it is a different amplifier
       wearing the name, and it would lose matches it should win while sounding like nothing in
       particular.

       Scaling by 0.5x to 1.5x of the preset's own value keeps tightness meaningful on every
       voicing and lands within a few Hz of the old range for the ones that used to define it:
       the shared 170 Hz default now sweeps 85-255 against the previous 90-270.
    */
    p.bass.crossoverHz = voicingCrossoverHz * (0.5f + tightness);
    p.bass.cleanBlend = report.cleanLowBlendEstimate.value;
    p.outputGainDb = -9.0f;
    preset.name = "Plausible " + std::string(nts::tone::toString(report.context.gainCategory))
        + " " + std::string(nts::amp::topologyName(point.topology)) + " candidate";
}

/** Neighbours of a point, one step along each axis. Topology is deliberately not
    perturbed: it is a discrete voicing already covered by the coarse pass, and
    stepping it here would restart the search rather than refine it.

    Dynamics is stepped here and nowhere else. The coarse grid seeds it at the measured value and
    leaves it alone, so this is the only thing that can move it -- which is the point: descending
    from a measurement is cheaper and better conditioned than sampling a guess.
*/
constexpr std::size_t refinementAxes = 4;

std::vector<CandidatePoint> refinementNeighbours(const CandidatePoint& centre, float step)
{
    std::vector<CandidatePoint> points;
    for (const auto direction : { -1.0f, 1.0f })
    {
        auto gainPoint = centre; gainPoint.gain = clamp01(centre.gain + direction * step);
        auto brightPoint = centre; brightPoint.brightness = clamp01(centre.brightness + direction * step);
        auto tightPoint = centre; tightPoint.tightness = clamp01(centre.tightness + direction * step);
        auto dynamicPoint = centre; dynamicPoint.dynamics = clamp01(centre.dynamics + direction * step);
        points.push_back(gainPoint); points.push_back(brightPoint);
        points.push_back(tightPoint); points.push_back(dynamicPoint);
    }
    return points;
}
} // namespace

std::vector<nts::amp::Topology> searchableTopologies(nts::amp::Instrument instrument)
{
    std::vector<nts::amp::Topology> eligible;
    eligible.reserve(nts::amp::topologyCount);
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
    {
        const auto topology = static_cast<nts::amp::Topology>(index);
        // A bass-native voicing still answers for guitar -- `makeOriginalPreset` gives every one a
        // guitar reading -- so this decides what is *searched*, not what exists.
        if (instrument == nts::amp::Instrument::guitar
            && nts::amp::topologyAffinity(topology) == nts::amp::TopologyAffinity::bass)
            continue;
        eligible.push_back(topology);
    }
    return eligible;
}

std::string StemSeparator::deterministicCacheKey(const StereoAudio& mixture,
                                                 const SeparationOptions& options)
{
    std::uint64_t hash = 1469598103934665603ull;
    hashBytes(hash, &mixture.sampleRate, sizeof(mixture.sampleRate));
    const auto count = mixture.samples();
    hashBytes(hash, &count, sizeof(count));
    hashBytes(hash, options.modelVersion.data(), options.modelVersion.size());
    hashBytes(hash, &options.chunkSamples, sizeof(options.chunkSamples));
    hashBytes(hash, &options.overlapSamples, sizeof(options.overlapSamples));
    hashBytes(hash, mixture.left.data(), mixture.left.size() * sizeof(float));
    hashBytes(hash, mixture.right.data(), mixture.right.size() * sizeof(float));
    return hexHash(hash);
}

StemSet StemSeparator::separate(const StereoAudio& mixture, const SeparationOptions& options,
                                const ProgressCallback& progress, std::stop_token stopToken) const
{
    StemSet result;
    result.modelVersion = options.modelVersion;
    if (mixture.sampleRate < 8000.0 || mixture.sampleRate > 384000.0 || mixture.left.empty()
        || (! mixture.right.empty() && mixture.right.size() != mixture.left.size()))
    {
        result.error = "invalid decoded song audio";
        return result;
    }
    if (options.chunkSamples < 1024 || options.overlapSamples >= options.chunkSamples)
    {
        result.error = "invalid separation chunk/overlap configuration";
        return result;
    }
    result.cacheKey = deterministicCacheKey(mixture, options);
    result.usedGpu = false;
    if (options.preferGpu) result.warnings.emplace_back("GPU model unavailable; deterministic CPU fallback used");
    auto source = mixture;
    if (source.right.empty()) source.right = source.left;
    auto rawVocals = like(source), rawDrums = like(source), rawBass = like(source), rawOther = like(source);
    const auto alphaBass = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * 230.0f
                                           / static_cast<float>(source.sampleRate));
    const auto alphaBand = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * 5200.0f
                                           / static_cast<float>(source.sampleRate));
    std::array<float, 2> low {}, bandLow {}, previous {}, transientEnvelope {};
    for (std::size_t index = 0; index < source.samples(); ++index)
    {
        if (stopToken.stop_requested()) { result.error = "separation cancelled"; return result; }
        const auto mid = 0.5f * (source.left[index] + source.right[index]);
        const auto side = 0.5f * (source.left[index] - source.right[index]);
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            const auto input = channel == 0 ? source.left[index] : source.right[index];
            low[channel] += alphaBass * (input - low[channel]);
            bandLow[channel] += alphaBand * (input - bandLow[channel]);
            const auto high = input - bandLow[channel];
            const auto onset = std::abs(input - previous[channel]);
            transientEnvelope[channel] = std::max(onset, transientEnvelope[channel] * 0.985f);
            const auto drumWeight = clamp01((onset - transientEnvelope[channel] * 0.35f) * 8.0f);
            const auto vocalMid = (bandLow[channel] - low[channel]) * 0.42f;
            const auto vocal = vocalMid + mid * 0.05f - side * (channel == 0 ? 0.02f : -0.02f);
            const auto drum = high * (0.08f + drumWeight * 0.38f)
                            + (input - previous[channel]) * drumWeight * 0.12f;
            auto& vocals = channel == 0 ? rawVocals.left : rawVocals.right;
            auto& drums = channel == 0 ? rawDrums.left : rawDrums.right;
            auto& bass = channel == 0 ? rawBass.left : rawBass.right;
            auto& other = channel == 0 ? rawOther.left : rawOther.right;
            vocals[index] = vocal;
            drums[index] = drum;
            bass[index] = low[channel];
            other[index] = input - vocal - drum - low[channel];
            previous[channel] = input;
        }
    }

    result.vocals = like(source); result.drums = like(source); result.bass = like(source); result.other = like(source);
    std::vector<float> weights(source.samples(), 0.0f);
    const auto hop = options.chunkSamples - options.overlapSamples;
    const auto chunks = std::max<std::size_t>(1, (source.samples() + hop - 1) / hop);
    std::size_t chunkIndex {};
    for (std::size_t begin = 0; begin < source.samples(); begin += hop, ++chunkIndex)
    {
        const auto end = std::min(source.samples(), begin + options.chunkSamples);
        for (auto index = begin; index < end; ++index)
        {
            float weight = 1.0f;
            if (options.overlapSamples > 0 && begin > 0 && index < begin + options.overlapSamples)
                weight *= static_cast<float>(index - begin + 1) / static_cast<float>(options.overlapSamples + 1);
            if (options.overlapSamples > 0 && end < source.samples() && index + options.overlapSamples >= end)
                weight *= static_cast<float>(end - index) / static_cast<float>(options.overlapSamples + 1);
            weights[index] += weight;
            for (std::size_t channel = 0; channel < 2; ++channel)
            {
                auto add = [index, weight](std::vector<float>& destination, const std::vector<float>& raw)
                    { destination[index] += raw[index] * weight; };
                add(channel == 0 ? result.vocals.left : result.vocals.right,
                    channel == 0 ? rawVocals.left : rawVocals.right);
                add(channel == 0 ? result.drums.left : result.drums.right,
                    channel == 0 ? rawDrums.left : rawDrums.right);
                add(channel == 0 ? result.bass.left : result.bass.right,
                    channel == 0 ? rawBass.left : rawBass.right);
                add(channel == 0 ? result.other.left : result.other.right,
                    channel == 0 ? rawOther.left : rawOther.right);
            }
        }
        const auto update = SeparationProgress { 0.15f + 0.8f * static_cast<float>(chunkIndex + 1)
                                                 / static_cast<float>(chunks), "separating overlapping chunks" };
        if ((progress && ! progress(update)) || stopToken.stop_requested())
        { result.error = "separation cancelled"; return result; }
        if (end == source.samples()) break;
    }
    double errorEnergy = 0.0, sourceEnergy = 0.0;
    for (std::size_t index = 0; index < source.samples(); ++index)
    {
        const auto inverse = 1.0f / std::max(weights[index], 1.0e-9f);
        for (auto* stem : { &result.vocals, &result.drums, &result.bass, &result.other })
        { stem->left[index] *= inverse; stem->right[index] *= inverse; }
        const auto reconstructed = result.vocals.left[index] + result.drums.left[index]
                                 + result.bass.left[index] + result.other.left[index];
        const auto difference = reconstructed - source.left[index];
        errorEnergy += difference * difference;
        sourceEnergy += source.left[index] * source.left[index];
    }
    result.reconstructionError = std::sqrt(static_cast<float>(errorEnergy / std::max(1.0e-18, sourceEnergy)));
    result.success = true;
    if (result.reconstructionError > 1.0e-4f)
        result.warnings.emplace_back("stem reconstruction consistency is outside tolerance");
    if (progress) progress({ 1.0f, "separation complete" });
    return result;
}

StereoAudio selectStem(const StemSet& stems, TargetInstrument target)
{
    if (target == TargetInstrument::bass) return stems.bass;
    if (! stems.guitar.left.empty()) return stems.guitar;
    auto guitar = stems.other;
    if (guitar.left.empty()) return guitar;
    const auto alphaLow = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * 75.0f
                                          / static_cast<float>(guitar.sampleRate));
    const auto alphaHigh = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * 9500.0f
                                           / static_cast<float>(guitar.sampleRate));
    std::array<float, 2> low {}, high {};
    for (std::size_t index = 0; index < guitar.samples(); ++index)
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            auto& samples = channel == 0 ? guitar.left : guitar.right;
            low[channel] += alphaLow * (samples[index] - low[channel]);
            high[channel] += alphaHigh * (samples[index] - high[channel]);
            samples[index] = high[channel] - low[channel];
        }
    return guitar;
}

StereoMode recommendStereoMode(const StereoAudio& audio, float doubleTrackingLikelihood)
{
    // Below this the reference reads as one source and the sum is what the listener hears, so
    // there is nothing to be gained by throwing half of it away. Set where the analyser's own
    // estimate becomes confident rather than merely non-zero: doubleTrackingLikelihood is a
    // product of three clamped terms and drifts off the floor for any wide mono-ish source.
    constexpr auto layeredThreshold = 0.45f;
    if (! audio.stereo() || audio.samples() == 0 || doubleTrackingLikelihood < layeredThreshold)
        return StereoMode::fullStereo;

    // Transient peak energy, not total energy: the first difference emphasises pick attacks over
    // sustain, and the transient axis is what analysing one side exists to protect. A channel
    // that is merely louder is not the one that was picked harder.
    const auto transientEnergy = [](const std::vector<float>& channel)
    {
        double energy {};
        for (std::size_t index = 1; index < channel.size(); ++index)
        {
            const auto slope = static_cast<double>(channel[index]) - channel[index - 1];
            energy += slope * slope;
        }
        return energy;
    };
    const auto left = transientEnergy(audio.left);
    const auto right = transientEnergy(audio.right);
    if (left <= 0.0 && right <= 0.0) return StereoMode::fullStereo;
    return left >= right ? StereoMode::left : StereoMode::right;
}

StereoAudio applyStereoMode(const StereoAudio& audio, StereoMode mode)
{
    auto source = audio;
    if (source.right.empty()) source.right = source.left;
    auto result = like(source);
    double leftEnergy = 0.0, rightEnergy = 0.0;
    for (std::size_t index = 0; index < source.samples(); ++index)
    { leftEnergy += source.left[index] * source.left[index]; rightEnergy += source.right[index] * source.right[index]; }
    const auto chooseLeft = leftEnergy >= rightEnergy;
    for (std::size_t index = 0; index < source.samples(); ++index)
    {
        const auto left = source.left[index], right = source.right[index];
        switch (mode)
        {
            case StereoMode::left: result.left[index] = result.right[index] = left; break;
            case StereoMode::right: result.left[index] = result.right[index] = right; break;
            case StereoMode::mid: result.left[index] = result.right[index] = 0.5f * (left + right); break;
            case StereoMode::side: result.left[index] = result.right[index] = 0.5f * (left - right); break;
            case StereoMode::pannedEstimate:
                result.left[index] = result.right[index] = chooseLeft ? left - 0.25f * right : right - 0.25f * left; break;
            case StereoMode::fullStereo: result.left[index] = left; result.right[index] = right; break;
        }
    }
    return result;
}

std::vector<RegionQuality> scoreRegions(const StemSet& stems, TargetInstrument target,
                                        double regionSeconds, double hopSeconds)
{
    std::vector<RegionQuality> result;
    if (! stems.success || regionSeconds <= 0.0 || hopSeconds <= 0.0) return result;
    const auto targetAudio = selectStem(stems, target);
    const auto regionSamples = std::max<std::size_t>(1, static_cast<std::size_t>(regionSeconds * targetAudio.sampleRate));
    const auto hopSamples = std::max<std::size_t>(1, static_cast<std::size_t>(hopSeconds * targetAudio.sampleRate));
    for (std::size_t begin = 0; begin < targetAudio.samples(); begin += hopSamples)
    {
        const auto end = std::min(targetAudio.samples(), begin + regionSamples);
        RegionQuality quality;
        quality.startSeconds = static_cast<double>(begin) / targetAudio.sampleRate;
        quality.endSeconds = static_cast<double>(end) / targetAudio.sampleRate;
        quality.duration = static_cast<float>(quality.endSeconds - quality.startSeconds);
        const auto targetRms = 0.5f * (rmsRange(targetAudio.left, begin, end) + rmsRange(targetAudio.right, begin, end));
        const auto vocalRms = 0.5f * (rmsRange(stems.vocals.left, begin, end) + rmsRange(stems.vocals.right, begin, end));
        const auto drumRms = 0.5f * (rmsRange(stems.drums.left, begin, end) + rmsRange(stems.drums.right, begin, end));
        quality.targetEnergy = clamp01((db(targetRms) + 60.0f) / 48.0f);
        quality.vocalLeakage = leakageSeverity(targetRms, vocalRms);
        quality.drumLeakage = leakageSeverity(targetRms, drumRms);
        quality.stereoStability = clamp01((correlationRange(targetAudio, begin, end) + 1.0f) * 0.5f);
        std::size_t clipped {}, crossings {};
        double tailCorrelation = 0.0, tailEnergy = 0.0;
        const auto delay = static_cast<std::size_t>(targetAudio.sampleRate * 0.08);
        for (auto index = begin; index < end; ++index)
        {
            // Different separator backends do not necessarily materialize every stem. The
            // Demucs target-only loading path, for example, keeps vocals, drums, and the
            // requested instrument while leaving the other vectors empty to reduce memory.
            const auto reconstructedMixture = sampleAt(stems.vocals.left, index)
                                            + sampleAt(stems.drums.left, index)
                                            + sampleAt(stems.bass.left, index)
                                            + sampleAt(stems.other.left, index)
                                            + sampleAt(stems.guitar.left, index)
                                            + sampleAt(stems.piano.left, index);
            if (std::abs(reconstructedMixture) > 0.985f) ++clipped;
            if (index > begin && std::signbit(targetAudio.left[index]) != std::signbit(targetAudio.left[index - 1])) ++crossings;
            if (index >= begin + delay)
            { tailCorrelation += targetAudio.left[index] * targetAudio.left[index - delay]; tailEnergy += targetAudio.left[index] * targetAudio.left[index]; }
        }
        quality.clipping = clamp01(static_cast<float>(clipped) / static_cast<float>(std::max<std::size_t>(1, end - begin)) * 40.0f);
        quality.reverbAmount = clamp01(std::abs(static_cast<float>(tailCorrelation / std::max(1.0e-12, tailEnergy))));
        quality.polyphonicDensity = clamp01(static_cast<float>(crossings) / static_cast<float>(std::max<std::size_t>(1, end - begin))
                                            * static_cast<float>(targetAudio.sampleRate) / 1800.0f);
        const auto durationScore = clamp01(quality.duration / 5.0f);
        quality.confidence = clamp01(quality.targetEnergy * 0.24f + (1.0f - quality.vocalLeakage) * 0.16f
            + (1.0f - quality.drumLeakage) * 0.17f + quality.stereoStability * 0.10f
            + (1.0f - quality.clipping) * 0.10f + (1.0f - quality.reverbAmount) * 0.08f
            + durationScore * 0.15f);
        if (quality.duration < 2.0f) quality.warnings.emplace_back("region is too short for reliable reconstruction");
        if (quality.drumLeakage > 0.55f) quality.warnings.emplace_back("heavy drum leakage");
        if (quality.vocalLeakage > 0.55f) quality.warnings.emplace_back("vocal leakage");
        if (quality.clipping > 0.1f) quality.warnings.emplace_back("clipped reference");
        if (quality.reverbAmount > 0.55f) quality.warnings.emplace_back("strong room or reverb tail");
        if (quality.polyphonicDensity > 0.8f) quality.warnings.emplace_back("dense or ambiguous source");
        result.push_back(std::move(quality));
        if (end == targetAudio.samples()) break;
    }
    return result;
}

RegionQuality recommendRegion(const StemSet& stems, TargetInstrument target, double regionSeconds)
{
    auto regions = scoreRegions(stems, target, regionSeconds, std::max(0.5, regionSeconds * 0.5));
    if (regions.empty()) return {};
    return *std::max_element(regions.begin(), regions.end(), [](const auto& first, const auto& second)
        { return first.confidence < second.confidence; });
}

std::vector<PlayableRegion> analyzePlayableRegions(
    const StereoAudio& targetAudio, std::span<const RegionQuality> qualityRegions,
    TargetInstrument target, std::size_t maximumRegions)
{
    std::vector<PlayableRegion> result;
    if (targetAudio.left.empty() || maximumRegions == 0 || qualityRegions.empty()) return result;
    std::vector<std::size_t> ranked(qualityRegions.size());
    std::iota(ranked.begin(), ranked.end(), 0);
    std::sort(ranked.begin(), ranked.end(), [qualityRegions](auto first, auto second)
    { return qualityRegions[first].confidence > qualityRegions[second].confidence; });
    for (const auto index : ranked)
    {
        const auto& quality = qualityRegions[index];
        const auto centre = (quality.startSeconds + quality.endSeconds) * 0.5;
        const auto overlaps = std::any_of(result.begin(), result.end(), [centre, &quality](const auto& selected)
        {
            const auto selectedCentre = (selected.quality.startSeconds + selected.quality.endSeconds) * 0.5;
            return std::abs(selectedCentre - centre) < std::max(1.0, quality.duration * 0.45);
        });
        if (overlaps) continue;
        PlayableRegion region;
        region.quality = quality;
        const auto begin = std::min(targetAudio.samples(), static_cast<std::size_t>(
            std::max(0.0, quality.startSeconds) * targetAudio.sampleRate));
        const auto end = std::min(targetAudio.samples(), static_cast<std::size_t>(
            std::max(quality.startSeconds, quality.endSeconds) * targetAudio.sampleRate));
        const auto gain = estimateGainCharacter(targetAudio, begin, end);
        region.gainCharacter = gain.first;
        region.gainScore = gain.second;
        const auto pitch = estimatePitch(targetAudio, begin, end, target);
        region.dominantPitch = pitchName(pitch.dominantMidi);
        region.dominantFrequencyHz = pitch.frequency;
        region.pitchConfidence = pitch.confidence;
        region.lowestPitchMidi = pitch.lowestMidi;
        region.tuningOffsetCents = pitch.cents;
        region.estimatedTuning = tuningFamily(pitch.lowestMidi);
        result.push_back(std::move(region));
        if (result.size() >= maximumRegions) break;
    }
    const auto reliableLowest = std::min_element(result.begin(), result.end(), [](const auto& first, const auto& second)
    {
        const auto firstMidi = first.pitchConfidence >= 0.35f && first.lowestPitchMidi >= 0
            ? first.lowestPitchMidi : std::numeric_limits<int>::max();
        const auto secondMidi = second.pitchConfidence >= 0.35f && second.lowestPitchMidi >= 0
            ? second.lowestPitchMidi : std::numeric_limits<int>::max();
        return firstMidi < secondMidi;
    });
    if (reliableLowest != result.end() && reliableLowest->pitchConfidence >= 0.35f)
    {
        const auto tuning = tuningFamily(reliableLowest->lowestPitchMidi);
        const auto cents = reliableLowest->tuningOffsetCents;
        for (auto& region : result)
        {
            region.estimatedTuning = tuning;
            region.tuningOffsetCents = cents;
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& first, const auto& second)
    { return first.quality.startSeconds < second.quality.startSeconds; });
    return result;
}

StereoAudio extractRegion(const StereoAudio& audio, double startSeconds, double endSeconds)
{
    StereoAudio result; result.sampleRate = audio.sampleRate;
    const auto start = std::min(audio.samples(), static_cast<std::size_t>(std::max(0.0, startSeconds) * audio.sampleRate));
    const auto end = std::min(audio.samples(), static_cast<std::size_t>(std::max(startSeconds, endSeconds) * audio.sampleRate));
    result.left.assign(audio.left.begin() + static_cast<std::ptrdiff_t>(start), audio.left.begin() + static_cast<std::ptrdiff_t>(end));
    if (! audio.right.empty()) result.right.assign(audio.right.begin() + static_cast<std::ptrdiff_t>(start), audio.right.begin() + static_cast<std::ptrdiff_t>(end));
    return result;
}

ReferenceNormalization normalizeReference(const StereoAudio& audio, bool reduceRoomTail)
{
    ReferenceNormalization result; result.audio = audio;
    if (result.audio.left.empty()) return result;
    if (result.audio.right.empty()) result.audio.right = result.audio.left;
    double meanLeft = 0.0, meanRight = 0.0;
    for (auto sample : result.audio.left) meanLeft += sample;
    for (auto sample : result.audio.right) meanRight += sample;
    meanLeft /= result.audio.left.size(); meanRight /= result.audio.right.size();
    float peak = 0.0f;
    for (std::size_t index = 0; index < result.audio.samples(); ++index)
    {
        result.audio.left[index] -= static_cast<float>(meanLeft);
        result.audio.right[index] -= static_cast<float>(meanRight);
        peak = std::max({ peak, std::abs(result.audio.left[index]), std::abs(result.audio.right[index]) });
    }
    const auto threshold = peak * 0.005f;
    std::size_t begin {}, end = result.audio.samples();
    while (begin < end && std::abs(result.audio.left[begin]) < threshold && std::abs(result.audio.right[begin]) < threshold) ++begin;
    while (end > begin && std::abs(result.audio.left[end - 1]) < threshold && std::abs(result.audio.right[end - 1]) < threshold) --end;
    if (begin < end)
    {
        result.audio.left = { result.audio.left.begin() + static_cast<std::ptrdiff_t>(begin), result.audio.left.begin() + static_cast<std::ptrdiff_t>(end) };
        result.audio.right = { result.audio.right.begin() + static_cast<std::ptrdiff_t>(begin), result.audio.right.begin() + static_cast<std::ptrdiff_t>(end) };
    }
    const auto inputRms = 0.5f * (rmsRange(result.audio.left, 0, result.audio.samples())
                                  + rmsRange(result.audio.right, 0, result.audio.samples()));
    result.originalLoudnessDb = db(inputRms);
    const auto scale = linear(result.normalizedLoudnessDb) / std::max(inputRms, 1.0e-7f);
    const auto roomDelay = static_cast<std::size_t>(result.audio.sampleRate * 0.06);
    for (std::size_t index = 0; index < result.audio.samples(); ++index)
        for (auto* channel : { &result.audio.left, &result.audio.right })
        {
            auto value = (*channel)[index];
            if (reduceRoomTail && index >= roomDelay) value -= (*channel)[index - roomDelay] * 0.08f;
            (*channel)[index] = std::clamp(value * scale, -2.0f, 2.0f);
        }
    result.roomTailReduced = reduceRoomTail;
    const auto quarter = std::max<std::size_t>(1, result.audio.samples() / 4);
    const auto low = rmsRange(result.audio.left, 0, quarter);
    const auto middle = rmsRange(result.audio.left, quarter, quarter * 3);
    const auto high = rmsRange(result.audio.left, quarter * 3, result.audio.samples());
    const auto average = std::max((low + middle + high) / 3.0f, 1.0e-7f);
    result.broadProductionEqDb = { db(low / average), db(middle / average), db(high / average) };
    return result;
}

ReconstructionResult RigReconstructor::reconstruct(const ReconstructionReference& reference,
                                                   std::span<const float> userDi, double sampleRate,
                                                   std::size_t candidateCount,
                                                   const ProgressCallback& progress,
                                                   std::stop_token stopToken) const
{
    ReconstructionResult result; result.reference = reference;
    if (! reference.tone.success || userDi.empty() || sampleRate < 8000.0 || candidateCount == 0)
    { result.error = "valid tone reference, DI, sample rate, and candidate count are required"; return result; }
    result.neuralConditioning = reference.tone.embedding.values;
    const auto diRms = rmsRange(std::vector<float>(userDi.begin(), userDi.end()), 0, userDi.size());
    const auto targetRms = std::max(reference.tone.features.rms, 1.0e-6f);
    const auto inputTrim = std::clamp(db(targetRms / std::max(diRms, 1.0e-7f)), -18.0f, 18.0f);
    const auto poolSize = std::max<std::size_t>(candidateCount * 3, 12);
    // Two refinement rounds, each stepping half as far as the last, starting at
    // half the coarse grid spacing of 0.06.
    constexpr std::array refinementSteps { 0.03f, 0.015f };
    nts::tone::ToneAnalyzer analyzer;
    std::vector<RigCandidate> pool;
    std::vector<CandidatePoint> evaluatedPoints;

    // Only the voicings this instrument can actually use. Guitar searches seven and bass
    // thirteen, so a guitar reconstruction costs what it did before the bass voicings landed
    // rather than paying six extra renders for amplifiers it will never offer.
    const auto searchable = searchableTopologies(
        reference.target == TargetInstrument::bass ? nts::amp::Instrument::bass
                                                   : nts::amp::Instrument::guitar);

    // Two directions along each refinement axis per round, plus the topology re-check, which
    // renders the refined point under every voicing the coarse pool did not reach.
    const auto totalRenders = poolSize + refinementSteps.size() * refinementAxes * 2
                            + searchable.size() - 1;
    std::size_t rendersDone {};

    const auto evaluate = [&](const CandidatePoint& point) -> std::optional<RigCandidate>
    {
        RigCandidate candidate;
        const auto instrument = reference.target == TargetInstrument::bass ? nts::amp::Instrument::bass
                                                                           : nts::amp::Instrument::guitar;
        // Built from the point's own topology so the values setCandidateParameters
        // leaves alone, the phase inverter and the supply sag timings, come from
        // that voicing. Previously every candidate started from Tight Modern and
        // only had its topology flag flipped afterwards, so a Vintage Bloom
        // candidate carried Tight Modern's power stage.
        candidate.rigPreset = nts::amp::makeOriginalPreset(point.topology, instrument);
        setCandidateParameters(candidate.rigPreset, reference.tone.report, point);
        candidate.adaptation.inputTrimDb = inputTrim;
        candidate.adaptation.lowShelfDb = (reference.tone.report.cleanLowBlendEstimate.value - 0.5f) * 6.0f;
        candidate.adaptation.midEqDb = (0.5f - reference.tone.report.brightness.value) * 3.0f;
        // Reported from the candidate's own brightness rather than the reference's, because
        // this is the same decision `setCandidateParameters` already applies as `postHighDb` --
        // the pre-EQ has no high shelf to put it in. Taken from the unperturbed measurement it
        // described a shelf half a grid step away from the one the preset actually carries.
        candidate.adaptation.highShelfDb = (point.brightness - 0.5f) * 5.0f;
        candidate.adaptation.dynamicRangeScale = std::clamp(1.35f - reference.tone.report.compression.value, 0.45f, 1.35f);
        candidate.rigPreset.parameters.manualInputTrimDb = candidate.adaptation.inputTrimDb;
        candidate.rigPreset.parameters.preEq.lowShelfEnabled = true;
        candidate.rigPreset.parameters.preEq.lowShelfDb = candidate.adaptation.lowShelfDb;
        candidate.rigPreset.parameters.preEq.midEmphasisEnabled = true;
        candidate.rigPreset.parameters.preEq.midEmphasisDb = candidate.adaptation.midEqDb;
        /* Supply sag, half from the search axis and half from the measured compression.

           `dynamicRangeScale` was the only consumer of the `compression` descriptor and had no
           consumer of its own, so the one measurement describing how compressed the reference
           is influenced nothing in the render. Sag is the amplifier's own dynamic-range
           mechanism -- a compressed reference wants more of it, an open one less -- so that is
           where it belongs.

           Blended rather than substituted: `setCandidateParameters` derives sag from tightness,
           and taking it over outright would remove sag from the refinement pass, which steps
           tightness but cannot see this. Half each keeps both able to move it. */
        const auto sagFromCompression = clamp01((1.35f - candidate.adaptation.dynamicRangeScale) / 0.9f);
        auto& powerAmp = candidate.rigPreset.parameters.powerAmp;
        powerAmp.sag = clamp01(powerAmp.sag * 0.5f + sagFromCompression * 0.5f);
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, 512, 1 });
        processor.loadPreset(candidate.rigPreset, 0);
        auto rendered = nts::amp::renderOffline(processor, userDi, 256);
        const auto renderedTone = analyzer.analyze({ rendered, {}, sampleRate,
                                                     nts::tone::SourceType::pluginRender, 1.0f });
        if (! renderedTone.success) return std::nullopt;
        const auto comparison = nts::tone::ToneSimilarity::compare(profile("reference", reference.tone),
                                                                    profile("candidate", renderedTone));
        candidate.toneSimilarity = comparison.score;
        const auto loudnessMatch = 1.0f - clamp01(std::abs(renderedTone.features.dynamic.integratedLoudnessDb
                                                          - reference.tone.features.dynamic.integratedLoudnessDb) / 24.0f);
        const auto productionPenalty = reference.tone.features.spatial.roomReverbEstimate * 0.18f
                                     + reference.tone.features.spatial.doubleTrackingLikelihood * 0.12f;
        candidate.recordingSimilarity = clamp01(candidate.toneSimilarity * 0.76f + loudnessMatch * 0.24f
                                                 - productionPenalty);
        candidate.complexityPenalty = static_cast<float>(candidate.rigPreset.parameters.stageCount - 2) * 0.025f
                                    + (candidate.rigPreset.parameters.preEq.midEmphasisEnabled ? 0.01f : 0.0f);
        candidate.confidence = clamp01(reference.analysisConfidence * 0.65f + candidate.toneSimilarity * 0.35f
                                       - candidate.complexityPenalty);
        if (reference.quality.stereoStability < 0.45f)
            candidate.warnings.emplace_back("double-tracked or unstable stereo reference");
        if (reference.quality.drumLeakage > 0.5f)
            candidate.warnings.emplace_back("candidate may reflect drum leakage");
        candidate.warnings.emplace_back("plausible virtual approximation; original hardware is not identified");
        return candidate;
    };

    const auto rank = [](const RigCandidate& value)
    {
        return value.toneSimilarity * 0.7f + value.recordingSimilarity * 0.3f - value.complexityPenalty;
    };

    const auto record = [&](const CandidatePoint& point, std::string_view stage) -> bool
    {
        if (stopToken.stop_requested()) return false;
        if (auto candidate = evaluate(point))
        {
            pool.push_back(std::move(*candidate));
            evaluatedPoints.push_back(point);
        }
        ++rendersDone;
        if (progress && ! progress({ static_cast<float>(rendersDone) / static_cast<float>(totalRenders),
                                     std::string(stage) }))
            return false;
        return true;
    };

    // Coarse pass: sample the grid. Topology is the slowest digit, so a default pool covers the
    // first voicing here and the rest are reached by the re-check after refinement.
    const auto dynamics = dynamicsSeed(reference.tone.features.dynamic,
                                       reference.tone.features.spatial.doubleTrackingLikelihood);
    for (std::size_t variant = 0; variant < poolSize; ++variant)
        if (! record(coarsePoint(reference.tone.report, dynamics, variant, searchable),
                     "rendering and ranking candidate rigs"))
        { result.error = "reconstruction cancelled"; return result; }

    if (pool.empty()) { result.error = "candidate rendering did not produce a valid tone"; return result; }

    // Refinement pass: coordinate descent around the coarse winner. The coarse
    // grid is far too sparse to land on a good match by itself, so the best point
    // is stepped along each axis and kept whenever the score improves.
    {
        auto bestIndex = static_cast<std::size_t>(std::distance(pool.begin(),
            std::max_element(pool.begin(), pool.end(), [&](const auto& a, const auto& b)
            { return rank(a) < rank(b); })));
        auto bestPoint = evaluatedPoints[bestIndex];
        auto bestScore = rank(pool[bestIndex]);

        for (const auto step : refinementSteps)
        {
            auto improved = false;
            for (const auto& neighbour : refinementNeighbours(bestPoint, step))
            {
                const auto before = pool.size();
                if (! record(neighbour, "refining the closest rig"))
                { result.error = "reconstruction cancelled"; return result; }
                if (pool.size() > before && rank(pool.back()) > bestScore)
                {
                    bestScore = rank(pool.back());
                    bestPoint = neighbour;
                    improved = true;
                }
            }
            // Nothing along any axis beat the centre, so a smaller step around the
            // same point cannot either.
            if (! improved) break;
        }

        // Topology re-check. The descent above deliberately leaves topology alone -- stepping a
        // discrete voicing restarts the search rather than refining it -- but it is the slowest
        // coarse digit, so a default pool of twelve only ever sampled the first voicing. One
        // render of the winning point under each of the others is what keeps them reachable at
        // all, and it is a fairer comparison than the coarse pass gave: every voicing is judged
        // at the refined position rather than at whichever grid corner it landed on.
        //
        // This is the whole reason a voicing added to the enum costs search time: it is
        // `topologyCount - 1` extra offline renders on every reconstruction, not one. That is
        // the price of the voicing being findable, and dropping to a sampled subset would mean
        // some amplifiers could never win a match.
        for (const auto other : searchable)
        {
            if (other == bestPoint.topology) continue;
            auto alternative = bestPoint;
            alternative.topology = other;
            if (! record(alternative, "comparing the other power-stage voicings"))
            { result.error = "reconstruction cancelled"; return result; }
        }
    }

    std::sort(pool.begin(), pool.end(), [&](const auto& first, const auto& second)
    {
        return rank(first) > rank(second);
    });
    // The shortlist is something the user auditions and edits, so near-identical
    // entries waste its slots. Refinement deliberately produces points close to
    // the winner, so take the best first and then only entries that actually
    // sound like a different rig, falling back to rank order if too few qualify.
    // Topology is deliberately not one of the tests. It is a discrete flag, so including it
    // guaranteed that flipping it alone qualified as a different rig, and the shortlist filled
    // with pairs that differed by little else -- which is why topology read as the axis the
    // match cared most about. Judge difference on what a listener hears instead.
    const auto audiblyDifferent = [](const nts::amp::AmpParameters& first,
                                     const nts::amp::AmpParameters& second)
    {
        return std::abs(first.stages[0].driveDb - second.stages[0].driveDb) > 1.5f
            || std::abs(first.toneStack.treble - second.toneStack.treble) > 0.05f
            || std::abs(first.toneStack.mid - second.toneStack.mid) > 0.05f
            || std::abs(first.preEq.highCutHz - second.preEq.highCutHz) > 400.0f
            || first.stageCount != second.stageCount;
    };

    std::vector<std::size_t> chosen;
    for (std::size_t index = 0; index < pool.size() && chosen.size() < candidateCount; ++index)
    {
        const auto distinct = std::all_of(chosen.begin(), chosen.end(), [&](std::size_t existing)
        { return audiblyDifferent(pool[existing].rigPreset.parameters, pool[index].rigPreset.parameters); });
        if (distinct) chosen.push_back(index);
    }
    for (std::size_t index = 0; index < pool.size() && chosen.size() < candidateCount; ++index)
        if (std::find(chosen.begin(), chosen.end(), index) == chosen.end()) chosen.push_back(index);

    // Back into rank order: pool is already sorted, so ascending index is descending score.
    std::sort(chosen.begin(), chosen.end());
    std::vector<RigCandidate> shortlist;
    shortlist.reserve(chosen.size());
    for (const auto index : chosen) shortlist.push_back(std::move(pool[index]));
    result.candidates = std::move(shortlist);
    if (reference.analysisConfidence < 0.55f)
        result.warnings.emplace_back("ambiguous reference: candidates have reduced confidence");
    result.warnings.emplace_back("shared result contains parameters and embeddings only; source audio is excluded");
    // Refinement stops early once an axis sweep finds no improvement, so the
    // render count is an upper bound and the fraction above can end short of 1.
    if (progress) progress({ 1.0f, "reconstruction complete" });
    result.success = true;
    return result;
}

std::string serializeResult(const ReconstructionResult& result, bool pretty)
{
    const auto newline = pretty ? "\n" : "";
    const auto indent = pretty ? "  " : "";
    std::ostringstream stream; stream << std::setprecision(7);
    stream << '{' << newline << indent << "\"schemaVersion\":\"" << result.reconstructionVersion << "\","
           << newline << indent << "\"reference\":{"
           << "\"sourceHash\":\"" << result.reference.sourceHash << "\","
           << "\"regionStartSeconds\":" << result.reference.regionStartSeconds << ','
           << "\"regionEndSeconds\":" << result.reference.regionEndSeconds << ','
           << "\"separationModelVersion\":\"" << result.reference.separationModelVersion << "\"," 
           << "\"analysisConfidence\":" << result.reference.analysisConfidence << ','
           << "\"dominantPitch\":\"" << result.reference.dominantPitch << "\","
           << "\"estimatedTuning\":\"" << result.reference.estimatedTuning << "\","
           << "\"gainCharacter\":\"" << result.reference.gainCharacter << "\","
           << "\"pitchConfidence\":" << result.reference.pitchConfidence << ','
           << "\"tuningOffsetCents\":" << result.reference.tuningOffsetCents << "},"
           << newline << indent << "\"candidates\":[";
    for (std::size_t index = 0; index < result.candidates.size(); ++index)
    {
        if (index > 0) stream << ',';
        const auto& candidate = result.candidates[index];
        stream << newline << indent << indent << '{'
               << "\"rigPreset\":" << nts::amp::serializePreset(candidate.rigPreset, false) << ','
               << "\"toneSimilarity\":" << candidate.toneSimilarity << ','
               << "\"recordingSimilarity\":" << candidate.recordingSimilarity << ','
               << "\"confidence\":" << candidate.confidence << '}';
    }
    stream << newline << indent << "]" << newline << '}';
    return stream.str();
}

std::string_view toString(TargetInstrument value) noexcept
{ return value == TargetInstrument::bass ? "bass" : "guitar"; }
std::string_view toString(StereoMode value) noexcept
{
    switch (value)
    {
        case StereoMode::left: return "left"; case StereoMode::right: return "right";
        case StereoMode::mid: return "mid"; case StereoMode::side: return "side";
        case StereoMode::pannedEstimate: return "panned-estimate"; default: return "full-stereo";
    }
}
std::string_view toString(GainCharacter value) noexcept
{
    switch (value)
    {
        case GainCharacter::crunch: return "crunch";
        case GainCharacter::distorted: return "distorted";
        default: return "clean";
    }
}
} // namespace nts::reconstruction
