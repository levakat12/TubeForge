#include <nts/reconstruction/SourceReconstruction.h>

#include <nts/ir/CabinetMatch.h>

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

/** What distortion does that brightness does not: energy at integer multiples of the fundamental.

    The measure this replaces was adjacent-sample slew, and slew is a spectral-centroid detector
    wearing a distortion label -- `2*sin(pi*f/fs)` for energy centred at f, so it needs roughly
    3 kHz of centroid to saturate and reads about 0.16 of its 0.60 allowance for a guitar sitting
    near 800 Hz. Measured against engine renders of every voicing at three drive settings, 19 of 21
    classified clean, including high-gain voicings driven hot. It read cymbals, production and
    separator artifacts as distortion, and an isolated guitar as clean however hard it was driven.

    `harmonicity` is the share of measured energy that lands on multiples of the fundamental rather
    than between them. It is the term that rejects what the old measure fell for: cymbals, hiss and
    separation artifacts are inharmonic, so they raise the between-harmonic floor and push this
    down rather than up.

    `richness` is the share of *harmonic* energy above the third partial. This is the distortion
    cue proper. A clean string's partials fall away steeply; clipping generates upper harmonics and
    flattens that decay. Because it is a ratio within the harmonic series, it does not care how
    bright the signal is overall -- a dark distorted amplifier still reads rich, which is precisely
    the case the centroid measure could not see.

    Windowed and averaged rather than measured once, because a region is a riff and not a note. Each
    window gets its own fundamental from a decimated autocorrelation -- the same trick the tuner
    uses to stay cheap -- and windows with no clear pitch are skipped rather than guessed at, so
    percussive or silent stretches contribute nothing instead of contributing noise.
*/
struct HarmonicMeasurement
{
    /// Share of measured energy on harmonics rather than between them. Low means noise-like.
    float harmonicity {};
    /// Share of harmonic energy above the third partial. High means clipped.
    float richness {};
    /// Fraction of windows that had a usable pitch at all.
    float coverage {};
};

/// Energy at one frequency, by Goertzel. Cheaper than a transform when a handful of bins is all
/// that is wanted, and exact at the bin rather than smeared across one.
double goertzelPower(std::span<const float> window, double frequency, double sampleRate) noexcept
{
    if (window.empty() || frequency <= 0.0 || frequency >= sampleRate * 0.5) return 0.0;
    const auto omega = 2.0 * std::numbers::pi * frequency / sampleRate;
    const auto coefficient = 2.0 * std::cos(omega);
    double first {}, second {};
    const auto count = static_cast<double>(window.size());
    for (std::size_t index = 0; index < window.size(); ++index)
    {
        // Hann, applied here rather than into a scratch buffer: the harmonics of a low note sit
        // close together, and an unwindowed probe leaks one partial into its neighbours badly
        // enough to wash out the ratio being measured.
        const auto taper = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi
                                                * static_cast<double>(index) / count);
        const auto sample = static_cast<double>(window[index]) * taper;
        const auto current = sample + coefficient * first - second;
        second = first;
        first = current;
    }
    return std::max(0.0, first * first + second * second - coefficient * first * second);
}

/// Fundamental of one window, by autocorrelation on a decimated copy. Returns 0 when nothing in
/// the searched range stands out, which is the answer for a percussive or silent window.
double windowFundamental(std::span<const float> window, double sampleRate) noexcept
{
    constexpr int decimation = 6;
    const auto reducedRate = sampleRate / decimation;
    const auto reducedCount = window.size() / decimation;
    if (reducedCount < 64) return 0.0;
    std::vector<float> reduced(reducedCount);
    for (std::size_t index = 0; index < reducedCount; ++index)
    {
        auto sum = 0.0f;
        for (int tap = 0; tap < decimation; ++tap) sum += window[index * decimation + static_cast<std::size_t>(tap)];
        reduced[index] = sum / static_cast<float>(decimation);
    }
    // 70 Hz to 800 Hz covers a detuned low string up to the top of where a riff's fundamental sits.
    const auto shortestLag = static_cast<std::size_t>(reducedRate / 800.0);
    const auto longestLag = std::min(reducedCount / 2, static_cast<std::size_t>(reducedRate / 70.0));
    if (longestLag <= shortestLag) return 0.0;
    double zeroLag {};
    for (const auto sample : reduced) zeroLag += static_cast<double>(sample) * sample;
    if (zeroLag <= 1.0e-12) return 0.0;
    auto bestLag = std::size_t {};
    double bestScore {};
    for (auto lag = shortestLag; lag <= longestLag; ++lag)
    {
        double sum {};
        for (std::size_t index = 0; index + lag < reducedCount; ++index)
            sum += static_cast<double>(reduced[index]) * reduced[index + lag];
        if (sum > bestScore) { bestScore = sum; bestLag = lag; }
    }
    // A periodic window correlates strongly with itself one period later; noise does not.
    if (bestLag == 0 || bestScore < zeroLag * 0.30) return 0.0;
    return reducedRate / static_cast<double>(bestLag);
}

HarmonicMeasurement measureHarmonicStructure(const StereoAudio& audio, std::size_t begin, std::size_t end)
{
    HarmonicMeasurement measurement;
    end = std::min(end, audio.samples());
    if (end <= begin) return measurement;

    const auto monoAt = [&audio](std::size_t index)
    {
        const auto right = audio.right.empty() ? audio.left[index] : audio.right[index];
        return 0.5f * (audio.left[index] + right);
    };

    // ~93 ms at 48 kHz: long enough to resolve the partials of a low note, short enough that one
    // window holds one note of a riff rather than three.
    const auto windowLength = std::min(end - begin, static_cast<std::size_t>(audio.sampleRate * 0.093));
    if (windowLength < 512) return measurement;
    const auto available = (end - begin) / windowLength;
    // Bounded so a long region costs the same as a short one; spread rather than taken from the
    // front, because the front of a region is often its quietest part.
    const auto windowCount = std::max<std::size_t>(1, std::min<std::size_t>(available, 24));

    std::vector<float> window(windowLength);
    double harmonicTotal {}, interTotal {}, upperTotal {}, voiced {};
    for (std::size_t index = 0; index < windowCount; ++index)
    {
        const auto start = begin + (available <= 1 ? 0 : (available - 1) * index / windowCount * windowLength);
        if (start + windowLength > end) break;
        for (std::size_t sample = 0; sample < windowLength; ++sample) window[sample] = monoAt(start + sample);

        const auto fundamental = windowFundamental(window, audio.sampleRate);
        if (fundamental <= 0.0) continue;
        ++voiced;

        double harmonics {}, between {}, upper {};
        for (int partial = 1; partial <= 12; ++partial)
        {
            const auto frequency = fundamental * partial;
            if (frequency >= audio.sampleRate * 0.45) break;
            const auto power = goertzelPower(window, frequency, audio.sampleRate);
            harmonics += power;
            /* From the third partial up.

               The third is not an arbitrary place to start: symmetric clipping -- which is what a
               push-pull output stage and most pedals do -- generates odd harmonics, and the third
               is by far the largest of them. Starting at the fourth, as this first did, discarded
               the single strongest evidence of clipping there is and rated a hard-clipped sine as
               clean, because a square wave's energy is still dominated by its fundamental. The
               second is deliberately excluded: a plucked string has a strong second of its own, so
               counting it would measure the instrument rather than the amplifier. */
            if (partial >= 3) upper += power;
            // Halfway to the next partial: nothing musical lives there, so whatever does is noise,
            // leakage, another instrument, or a separation artifact.
            const auto betweenFrequency = fundamental * (static_cast<double>(partial) + 0.5);
            if (betweenFrequency < audio.sampleRate * 0.45)
                between += goertzelPower(window, betweenFrequency, audio.sampleRate);
        }
        const auto measured = harmonics + between;
        if (measured <= 1.0e-12) continue;
        harmonicTotal += harmonics;
        interTotal += between;
        upperTotal += harmonics > 0.0 ? upper / harmonics : 0.0;
    }

    if (voiced <= 0.0) return measurement;
    measurement.coverage = static_cast<float>(voiced / static_cast<double>(windowCount));
    const auto measured = harmonicTotal + interTotal;
    measurement.harmonicity = measured > 0.0 ? static_cast<float>(harmonicTotal / measured) : 0.0f;
    measurement.richness = static_cast<float>(upperTotal / voiced);
    return measurement;
}

std::pair<GainCharacter, float> estimateGainCharacter(const StereoAudio& audio,
                                                       std::size_t begin, std::size_t end)
{
    end = std::min(end, audio.samples());
    if (end <= begin) return { GainCharacter::clean, 0.0f };

    /* Distortion, measured from harmonic structure rather than from brightness.

       The `richness` span below is calibrated against two measured distributions rather than
       chosen: engine renders of every guitar voicing at three drive settings through a clean DI
       are the anchors, all measured: an engine render of the cleanest voicing sits near 0.04, the
       highest-gain voicing driven hard near 0.28, the genuinely clean intro of a metal track near
       0.20, and its rhythm sections above 0.45. Mapping 0.03 to 0.61 across the term is the span
       that keeps all four on the right side of the two thresholds, and it is tight -- the clean
       intro and the driven render sit close together, because a real clean recording carries more
       harmonic content than a synthetic driven one. Re-measure before moving these numbers; they
       are a fit to data, not a derivation, and `nts_rig_recovery` and `nts_song_probe` produced
       them.

       `harmonicity` and `coverage` gate the term rather than adding to it. Both answer "should
       this reading be trusted at all" -- inharmonic material has no harmonic series to be rich in,
       and a region where no window held a pitch has nothing to measure. Untrusted collapses toward
       clean, which is the safe direction: it declines to claim distortion rather than inventing
       it from noise, and inventing it from noise is exactly what the previous measure did.

       Crest was tried as corroborating evidence and removed after measurement. An undistorted
       sine has a crest factor of 1.41, which a "low crest means compressed" term reads as heavily
       driven -- it scored 0.18 on a pure tone before any harmonic content was considered. That is
       the same mistake as the slew term in a different coat: a property of the waveform's shape
       standing in for a property of the amplifier.

       Zero-crossing rate and adjacent-sample slew are gone. Both were brightness measures: slew is
       `2*sin(pi*f/fs)` for energy centred at f, so it needed roughly 3 kHz of centroid to saturate
       and read a cabinet-filtered guitar as clean at any drive, while reading cymbals and
       separation artifacts as heavy distortion. */
    const auto harmonic = measureHarmonicStructure(audio, begin, end);
    const auto harmonicTrust = clamp01((harmonic.harmonicity - 0.45f) / 0.25f);
    const auto coverageTrust = clamp01(harmonic.coverage / 0.30f);
    const auto score = clamp01((harmonic.richness - 0.03f) / 0.58f) * harmonicTrust * coverageTrust;
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
/** How many voicings the coarse grid actually samples.

    **The bug this exists to stop was that the answer used to be one.** Topology was the slowest
    digit -- `(variant / 45)` -- and the pool is twelve, so the coarse pass never advanced past
    index 0 and the entire refinement budget was spent on whichever voicing happened to be first.
    Every other voicing then met that refined point cold, in a parameter neighbourhood fitted for
    a different amplifier. Combined with a gain classifier that answered "crunch" for nearly
    everything, that produced the same amplifier for every song.

    Three is a deliberate compromise, not a magic number. Topology is *not* a refinement axis --
    `refinementNeighbours` leaves it alone, because stepping a discrete voicing restarts the
    search rather than refining it -- so a voicing the coarse pass never samples cannot be
    improved later, only compared cold. Brightness and tightness *are* refinement axes and can be
    recovered, which is why they yield their coarse budget to topology here. Every voicing is
    still rendered by the topology re-check afterwards; this decides only which few get a fitted
    starting point.
*/
constexpr std::size_t coarseTopologySamples = 3;

CandidatePoint coarsePoint(const nts::tone::ToneReport& report, float dynamics, std::size_t variant,
                           std::span<const nts::amp::Topology> eligible)
{
    const auto topologyChoices = eligible.empty()
        ? std::size_t { 1 } : std::min(eligible.size(), coarseTopologySamples);
    const auto gainStep = centredGainSteps[variant % 5];
    const auto topologyStep = (variant / 5) % topologyChoices;
    const auto brightnessStep = centredSteps[(variant / (5 * topologyChoices)) % 3];
    const auto tightnessStep = centredSteps[(variant / (15 * topologyChoices)) % 3];
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

float topologyGainAffinity(nts::amp::Topology topology, GainCharacter character) noexcept
{
    /* One row per voicing in `Topology` order, one column per `GainCharacter`: clean, crunch,
       distorted.

       Read a row as "how much does this measurement argue for this amplifier". The rows are not
       normalised and do not need to be -- only the spread within a column matters, because every
       candidate in one search is scored against the same measured character.

       Three columns rather than the five of `nts::tone::GainCategory`, and that is a correction
       rather than a simplification. The five-way category is computed from a gain axis on which a
       pure undistorted sine reads 0.475 against a `clean` threshold of 0.16 -- clean and
       edge-of-breakup are unreachable, so it answered "crunch" for nearly everything and the
       search collapsed onto the one voicing that wins the crunch column. Three columns measured
       from something that discriminates beat five that do not.

       The guitar block first. `studioDirect` and `americanClean` are the two with real headroom,
       so they answer for clean and fall away fastest as the reference gets dirtier. `classAChime`
       is cathode-biased with almost no negative feedback, so it is compressing before it is
       distorting: it leans clean but stays useful into crunch. `vintageBloom` and `britishCrunch`
       are the two that break up in the *power* section, which is what crunch is. `tightModern` and
       `saggingRectifier` are the high-gain pair.

       The bass-native block is read the same way from its own entries: `hybridMosfet` is the one
       built to disappear, `cathodeVintage` compresses the moment it is played, `shortPathGrind`
       fuzzes out early by design, and `cmosModern` is the high-gain one. */
    constexpr std::size_t characterCount = 3;
    constexpr std::array<std::array<float, characterCount>, nts::amp::topologyCount> affinity { {
        //                    clean  crunch  distorted
        /* tightModern      */ { -0.8f, +0.3f, +1.0f },
        /* vintageBloom     */ { -0.3f, +0.9f, +0.1f },
        /* americanClean    */ { +0.9f,  0.0f, -0.7f },
        /* britishCrunch    */ { -0.4f, +1.0f, +0.4f },
        /* classAChime      */ { +0.7f, +0.5f, -0.5f },
        /* saggingRectifier */ { -0.9f, +0.4f, +1.0f },
        /* studioDirect     */ { +1.0f, -0.4f, -0.9f },
        /* valveFlagship    */ { +0.1f, +0.8f, +0.5f },
        /* cathodeVintage   */ { +0.7f, +0.4f, -0.4f },
        /* hybridMosfet     */ { +1.0f, -0.3f, -0.7f },
        /* shortPathGrind   */ { -0.7f, +0.5f, +0.9f },
        /* solidStateBiAmp  */ { +0.7f, +0.2f, -0.2f },
        /* cmosModern       */ { -0.8f, +0.4f, +1.0f },
    } };
    const auto row = static_cast<std::size_t>(topology);
    const auto column = static_cast<std::size_t>(character);
    if (row >= affinity.size() || column >= characterCount) return 0.0f;
    return affinity[row][column];
}

std::vector<nts::amp::Topology> searchableTopologies(nts::amp::Instrument instrument)
{
    std::vector<nts::amp::Topology> eligible;
    eligible.reserve(nts::amp::topologyCount);
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
    {
        const auto topology = static_cast<nts::amp::Topology>(index);
        const auto bassNative = nts::amp::topologyAffinity(topology) == nts::amp::TopologyAffinity::bass;
        // A bass-native voicing still answers for guitar -- `makeOriginalPreset` gives every one a
        // guitar reading -- so this decides what is *searched*, not what exists.
        if (instrument == nts::amp::Instrument::guitar && bassNative) continue;
        // See the note above. Temporary, and deliberately the only line that reads the switch.
        if (bassSearchesBassVoicingsOnly && instrument == nts::amp::Instrument::bass && ! bassNative)
            continue;
        eligible.push_back(topology);
    }
    return eligible;
}

/** How much a voicing built for this instrument is preferred at equal gain fit.

    **A bass reference used to search guitar amplifiers almost exclusively, and this is why.**
    Instrument affinity filtered the pool and then never touched the order, so the sort ran on gain
    character alone and ties broke on enum order -- where the seven guitar voicings occupy indices
    0-6 and the six bass-native ones 7-12. A bass-native voicing could therefore never win a tie.
    The measured consequence, at `poolSize` 12 and `coarseTopologySamples` 3, which give the top
    three voicings five, five and two coarse fits:

        clean      studioDirect  first, guitar  7/12 of the coarse pool
        crunch     britishCrunch first, guitar 10/12
        distorted  tightModern   first, guitar 10/12

    In all three the *first* entry was a guitar voicing, and the first entry is where the whole
    coordinate descent is spent -- the topology re-check afterwards only re-renders the point that
    descent already fitted to something else, which the note at that re-check calls out as an
    unfair comparison between voicings.

    0.25 is a tie-break plus a modest prior, not an override. The affinity column spans -1 to +1
    and the differences that mean anything are around 0.1 to 0.2, so a guitar voicing that fits the
    measured gain character by more than a quarter still wins -- which is the intended outcome for
    a bass part that genuinely sounds like a guitar amplifier. At 0.4 the clean column collapses to
    bass-only and that choice stops being the measurement's to make; at 0.15 the crunch column does
    not move at all. Both were computed against the table above before this landed on 0.25.
*/
constexpr float instrumentNativeBonus = 0.25f;

std::vector<nts::amp::Topology> searchOrderForGain(nts::amp::Instrument instrument,
                                                   GainCharacter character)
{
    auto eligible = searchableTopologies(instrument);
    // Zero for guitar, because `searchableTopologies` has already removed every bass-native voicing
    // from a guitar search -- there is nothing left for a bonus to reorder, and a guitar
    // reconstruction therefore searches exactly the order it always did.
    const auto nativeBonus = [instrument](nts::amp::Topology topology)
    {
        return instrument == nts::amp::Instrument::bass
            && nts::amp::topologyAffinity(topology) == nts::amp::TopologyAffinity::bass
                ? instrumentNativeBonus : 0.0f;
    };
    // Stable, so equal affinities keep enum order and the same reference always searches the
    // same way. A reconstruction that reordered itself between runs would be untraceable.
    std::stable_sort(eligible.begin(), eligible.end(),
                     [character, &nativeBonus](nts::amp::Topology first, nts::amp::Topology second)
                     {
                         return topologyGainAffinity(first, character) + nativeBonus(first)
                              > topologyGainAffinity(second, character) + nativeBonus(second);
                     });
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
    /* There was a `broadProductionEqDb` here, three numbers named as a spectral balance and
       computed from `rmsRange(left, 0, quarter)`, `(quarter, quarter * 3)` and `(quarter * 3,
       end)`. Those arguments are *sample indices*: it measured the first quarter, middle half and
       last quarter of the region's duration -- an envelope, under a spectral name. Nothing ever
       read it. `estimateProductionProfile` measures the real thing, in sixth-octave bands,
       against a render of the chosen candidate. */
    return result;
}

std::vector<float> productionFeatures(const nts::tone::ToneAnalysisResult& tone,
                                       const ProductionProfile& profile)
{
    if (profile.spectralResidualDb.size() != 41) return {};
    std::vector<float> features;
    features.reserve(productionFeatureCount);
    const auto& spatial = tone.features.spatial;
    const auto& dynamics = tone.features.dynamic;
    for (const auto value : { spatial.stereoWidth, spatial.roomReverbEstimate,
                              spatial.doubleTrackingLikelihood, spatial.panning,
                              spatial.modulation, spatial.delayPresence,
                              spatial.channelCorrelation })
        features.push_back(value);
    for (const auto value : { dynamics.crestFactorDb, dynamics.transientPreservation,
                              dynamics.compressionAmount, dynamics.attackMilliseconds,
                              dynamics.releaseMilliseconds, dynamics.sustain,
                              dynamics.gainReductionEstimateDb, dynamics.loudnessRangeDb,
                              dynamics.integratedLoudnessDb })
        features.push_back(value);
    for (const auto value : profile.spectralResidualDb) features.push_back(value);
    return features;
}

nts::ir::CabinetMatchOptions productionResidualOptions(TargetInstrument target) noexcept
{
    nts::ir::CabinetMatchOptions options;
    // See the header. 30 Hz reaches a five-string's low B; the guitar default stays at 70, where
    // everything below is somebody else's instrument.
    if (target == TargetInstrument::bass) options.lowLimitHz = 30.0f;
    return options;
}

ProductionProfile estimateProductionProfile(const ReferenceNormalization& normalized,
                                             const nts::tone::ToneAnalysisResult& tone,
                                             std::span<const float> referenceMono,
                                             std::span<const float> candidateRender,
                                             double sampleRate,
                                             TargetInstrument target,
                                             const nts::ml::PackedFeatureRegressor* model)
{
    ProductionProfile profile;
    /* The level the guitar actually sat at, relative to what normalisation forced it to. Not
       folded into the amplifier's own output gain: that is a property of the voicing, and
       overwriting it would make every matched rig of a loud part clip and every quiet one
       disappear. The clamp matters -- a near-silent region gives `originalLoudnessDb` near
       -120, and the difference would be an offset no output stage could survive. */
    profile.levelOffsetDb = std::clamp(normalized.originalLoudnessDb - normalized.normalizedLoudnessDb,
                                       -24.0f, 24.0f);

    const auto& spatial = tone.features.spatial;
    const auto& dynamics = tone.features.dynamic;
    profile.roomAmount = clamp01(spatial.roomReverbEstimate);
    profile.delayAmount = clamp01(spatial.delayPresence);
    profile.doubling = clamp01(spatial.doubleTrackingLikelihood);
    profile.stereoWidth = clamp01(spatial.stereoWidth);
    profile.compression = clamp01(dynamics.compressionAmount);

    /* The spectral half, when there is a render to compare against.

       `measureCabinetResidual` levels each spectrum to its own mean before differencing, so what
       comes back is a shape rather than a loudness -- which is exactly what is wanted here,
       because the loudness is already `levelOffsetDb` and counting it twice would apply it
       twice. The residual conflates mix EQ with cabinet error: if the matched cabinet is wrong,
       that error lands here and would be re-applied as "production". `confidence` is how the
       caller is told, and it is why the interface runs Cabinet Match first where it can. */
    auto residualConfidence = 0.0f;
    auto haveResidual = false;
    if (! referenceMono.empty() && ! candidateRender.empty())
    {
        const auto residual = nts::ir::measureCabinetResidual(referenceMono, candidateRender, sampleRate,
                                                               productionResidualOptions(target));
        if (residual.valid)
        {
            profile.spectralResidualDb = residual.residualDb;
            profile.residualFrequencies = residual.frequencies;
            residualConfidence = clamp01(residual.confidence);
            haveResidual = true;
        }
    }
    /* Halved when there is no residual rather than left at the analysis confidence, because a
       profile without a spectrum is a less complete answer and should not present as an equally
       sure one. */
    const auto analysis = clamp01(tone.report.confidence.aggregate);
    profile.confidence = haveResidual ? clamp01(analysis * 0.5f + residualConfidence * 0.5f)
                                      : clamp01(analysis * 0.5f);
    profile.valid = tone.success;

    /* The learned estimator, when one is installed and fits.

       **Computed after the deterministic path rather than instead of it**, and that ordering is
       the fallback: the built-in answer is always produced, so a model that is absent, the wrong
       shape, or refuses the vector leaves a complete profile behind rather than an empty one.
       There is no branch anywhere downstream -- `applyProductionProfile` reads the same four
       fields either way and only the `learned` flag differs, which is what the interface reports.

       It replaces only the four 0..1 amounts. `levelOffsetDb` and the residual are recovered
       exactly by measurement, and a model in front of an exact answer can only lose.

       Every precondition is checked rather than assumed. A model trained against a different
       feature list would still load -- the weights are internally consistent -- and would then be
       handed a vector whose columns mean something else, producing four plausible numbers from a
       misaligned input. That is precisely the failure the packed format's shape checks exist for,
       so the shape is checked here too rather than trusted from the file name. */
    if (model != nullptr && model->isLoaded() && haveResidual
        && model->inputCount() == productionFeatureCount && model->outputCount() == 4)
    {
        const auto features = productionFeatures(tone, profile);
        std::array<float, 4> predicted {};
        if (features.size() == productionFeatureCount && model->predict(features, predicted))
        {
            profile.roomAmount = clamp01(predicted[0]);
            profile.delayAmount = clamp01(predicted[1]);
            profile.doubling = clamp01(predicted[2]);
            profile.compression = clamp01(predicted[3]);
            profile.learned = true;
        }
    }
    return profile;
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

    /* What the chosen region measured as. Read once here because two separate things need it:
       where the search starts looking, and how the results are weighed.

       `reference.gainCharacter` rather than `reference.tone.report.context.gainCategory`, and the
       difference is the whole reason this search stopped answering "British Crunch" for every
       song. That tone-report category is derived from a 0..1 gain axis built as
       `0.52*saturationOnset + 0.25*compressionAmount + 0.23*spectralFlatness`, and measured
       against real signals it does not reach its own lower categories: a pure undistorted sine
       reads 0.475 and a clean plucked note 0.341, where `clean` requires below 0.16 and
       `edgeOfBreakup` below 0.34. Nearly everything therefore classified as crunch, the search
       seeded on the voicing that wins the crunch column, and the refinement budget did the rest.

       This one is measured by `estimateGainCharacter` from adjacent-sample slew, zero-crossing
       rate and crest factor on the region audio -- and it discriminates, which is the only
       property that matters here. It is also what the interface prints beside the region, so a
       part labelled CLEAN is now a part the search treats as clean. */
    const auto referenceGain = reference.gainCharacter;

    /* Only the voicings this instrument can actually use, most-likely first. Guitar searches seven;
       bass searches six or thirteen depending on `bassSearchesBassVoicingsOnly`. Either way a
       guitar reconstruction costs what it did before the bass voicings landed rather than paying
       extra renders for amplifiers it will never offer.

       The ordering is load-bearing rather than tidy: the coarse pass never advances past the first
       entry, so that entry is the voicing the whole refinement is spent on. See
       `searchOrderForGain`. */
    const auto searchable = searchOrderForGain(
        reference.target == TargetInstrument::bass ? nts::amp::Instrument::bass
                                                   : nts::amp::Instrument::guitar,
        referenceGain);

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

    /* The prior is in `rank` rather than in the pool construction, and that is where it belongs.

       Topology is the slowest coarse digit, so a small pool never samples more than the first
       voicing; every other voicing reaches the comparison through the topology re-check, which
       renders the refined winning point under each of them. That re-check is the one moment the
       voicings are judged against each other on equal terms, and it is decided by `rank` -- so
       adding the term here is adding it exactly where the choice is made, and nowhere else.

       Deliberately not folded into `candidate.confidence`. Confidence answers "how sure is this
       measurement", and a preference for a family of amplifiers is not evidence about that. */
    const auto rank = [referenceGain](const RigCandidate& value)
    {
        return value.toneSimilarity * 0.7f + value.recordingSimilarity * 0.3f - value.complexityPenalty
             + topologyGainPriorWeight
                 * topologyGainAffinity(value.rigPreset.parameters.topology, referenceGain);
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

    // Coordinate descent around the coarse winner, then the topology re-check. See the note at
    // the re-check for why that comparison is unfair between voicings and what has been measured
    // about fixing it.
    {
        auto bestIndex = static_cast<std::size_t>(std::distance(pool.begin(),
            std::max_element(pool.begin(), pool.end(), [&](const auto& a, const auto& b)
            { return rank(a) < rank(b); })));
        auto bestPoint = evaluatedPoints[bestIndex];
        auto bestScore = rank(pool[bestIndex]);
        auto cancelled = false;

        const auto refineAround = [&](CandidatePoint& point, float& score)
        {
            for (const auto step : refinementSteps)
            {
                auto improved = false;
                for (const auto& neighbour : refinementNeighbours(point, step))
                {
                    const auto before = pool.size();
                    if (! record(neighbour, "refining the closest rig")) { cancelled = true; return; }
                    if (pool.size() > before && rank(pool.back()) > score)
                    {
                        score = rank(pool.back());
                        point = neighbour;
                        improved = true;
                    }
                }
                // Nothing along any axis beat the centre, so a smaller step around the same point
                // cannot either.
                if (! improved) break;
            }
        };

        refineAround(bestPoint, bestScore);
        if (cancelled) { result.error = "reconstruction cancelled"; return result; }

        /* Every voicing the coarse grid did not reach, rendered at the refined point.

           This is the whole reason a voicing added to the enum costs search time: it is
           `topologyCount - 1` extra offline renders on every reconstruction, not one. That is the
           price of the voicing being findable, and dropping to a sampled subset would mean some
           amplifiers could never win a match.

           **This comparison is not fair between voicings, and knowing exactly how it is unfair is
           worth more than the two attempts that failed to fix it.** By this point the pool holds
           roughly twenty-eight entries at the coarse winner's voicing -- twelve coarse points and
           sixteen refinement neighbours -- and one at each of the others, so ranking the pool
           compares the best of twenty-eight draws against the best of one.

           Selecting from the re-check set alone was tried, on the theory that it puts every
           voicing on one render of one point. It changed nothing, to the attempt: 33/140 top-1
           before and after. The reason is that the shared point was *fitted for the coarse
           winner*, so equal render counts do not make it an equal contest -- the coordinates
           themselves favour one amplifier. Refining every voicing separately would, at seven times
           the descent cost, and that is the experiment `nts_rig_recovery` should be pointed at
           next. Do not re-try the cheap version; it is measured and it does not work. */
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
    /* The shortlist is something the user auditions and edits, so it has to contain choices.

       **Topology is a uniqueness key here, not a difference test, and conflating the two produced
       a shortlist of seven copies of one amplifier.** Including it in `audiblyDifferent` was tried
       and rightly removed: flipping a discrete flag alone qualified as a different rig, so the
       list filled with pairs that differed by nothing else and topology read as the axis the match
       cared most about. But leaving it out entirely is the opposite failure. Refinement produces
       sixteen neighbours of the coarse winner and every other voicing gets exactly one render, so
       the pool is dominated by one amplifier -- and those neighbours differ by more than the
       thresholds below, so they all pass as "audibly different" while being the same amp at
       slightly different settings. Measured on a Vintage Bloom reference, a seven-entry shortlist
       came back as seven British Crunches.

       So: one entry per voicing first, in rank order, which guarantees the list is a set of
       different amplifiers. Then audibly different variants if slots remain, which is what the
       original test was for and is still worth having when there are more slots than voicings.
       Then rank order, so a short pool still fills the list.

       The top entry is unaffected by all of this -- the pool is already sorted, and the
       highest-ranked candidate's voicing is by definition unseen when the first pass starts. */
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
    std::vector<nts::amp::Topology> seenVoicings;
    for (std::size_t index = 0; index < pool.size() && chosen.size() < candidateCount; ++index)
    {
        const auto topology = pool[index].rigPreset.parameters.topology;
        if (std::find(seenVoicings.begin(), seenVoicings.end(), topology) != seenVoicings.end())
            continue;
        seenVoicings.push_back(topology);
        chosen.push_back(index);
    }
    for (std::size_t index = 0; index < pool.size() && chosen.size() < candidateCount; ++index)
    {
        if (std::find(chosen.begin(), chosen.end(), index) != chosen.end()) continue;
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
           << "\"gainCharacter\":\"" << toString(result.reference.gainCharacter) << "\","
           << "\"pitchConfidence\":" << result.reference.pitchConfidence << ','
           << "\"tuningOffsetCents\":" << result.reference.tuningOffsetCents << "},"
           << newline << indent << "\"production\":";
    /* Written only when it was measured. A reader that finds no `production` key has a v1 file,
       or a v2 one whose match could not measure production, and either way the honest reading is
       "no estimate" -- which is a different statement from a block full of zeros, and the reason
       this is `null` rather than a default-constructed object. */
    if (! result.reference.production.valid) stream << "null,";
    else
    {
        const auto& production = result.reference.production;
        stream << '{'
               << "\"levelOffsetDb\":" << production.levelOffsetDb << ','
               << "\"roomAmount\":" << production.roomAmount << ','
               << "\"delayAmount\":" << production.delayAmount << ','
               << "\"doubling\":" << production.doubling << ','
               << "\"stereoWidth\":" << production.stereoWidth << ','
               << "\"compression\":" << production.compression << ','
               << "\"confidence\":" << production.confidence << ','
               << "\"learned\":" << (production.learned ? "true" : "false") << ','
               << "\"residualFrequencies\":[";
        for (std::size_t index = 0; index < production.residualFrequencies.size(); ++index)
            stream << (index > 0 ? "," : "") << production.residualFrequencies[index];
        stream << "],\"spectralResidualDb\":[";
        for (std::size_t index = 0; index < production.spectralResidualDb.size(); ++index)
            stream << (index > 0 ? "," : "") << production.spectralResidualDb[index];
        stream << "]},";
    }
    stream << newline << indent << "\"candidates\":[";
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
