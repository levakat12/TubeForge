#include <nts/ir/CabinetMatch.h>

#include <nts/dsp/Analysis.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

namespace nts::ir
{
namespace
{
constexpr std::size_t analysisSize = 4096;

/// Band centre for one of the `cabinetMatchBands`, log-spaced across the fitted range.
[[nodiscard]] float bandFrequency(const CabinetMatchOptions& options, std::size_t band) noexcept
{
    const auto position = static_cast<float>(band) / static_cast<float>(cabinetMatchBands - 1);
    return options.lowLimitHz * std::pow(options.highLimitHz / options.lowLimitHz, position);
}

/** Long-term average magnitude spectrum, in dB per band.

    Welch-style: Hann-windowed frames, 50% overlap, magnitudes averaged as *power* across every
    frame and then across the bins falling in each band. Averaging power rather than decibels is
    the part worth stating -- a mean of logarithms is dominated by the quietest frames, so a
    passage with rests in it would read as darker than the same passage without them.

    Returns false when there is not enough signal to measure, rather than returning a spectrum of
    silence that the caller would then difference against something real.
*/
[[nodiscard]] bool averageSpectrum(std::span<const float> audio, double sampleRate,
                                   const CabinetMatchOptions& options, const dsp::Fft& fft,
                                   std::vector<float>& bandsDb)
{
    bandsDb.assign(cabinetMatchBands, 0.0f);
    if (audio.size() < analysisSize) return false;

    std::vector<float> window(analysisSize);
    for (std::size_t index = 0; index < analysisSize; ++index)
        window[index] = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float>
                                                * static_cast<float>(index)
                                                / static_cast<float>(analysisSize - 1)));

    const auto bins = analysisSize / 2;
    std::vector<double> power(bins + 1, 0.0);
    std::vector<std::complex<float>> frame(analysisSize);
    std::size_t frames {};
    for (std::size_t start = 0; start + analysisSize <= audio.size(); start += analysisSize / 2)
    {
        for (std::size_t index = 0; index < analysisSize; ++index)
            frame[index] = { audio[start + index] * window[index], 0.0f };
        fft.transform(frame, false);
        for (std::size_t bin = 0; bin <= bins; ++bin)
        {
            const auto magnitude = std::abs(frame[bin]);
            power[bin] += static_cast<double>(magnitude) * magnitude;
        }
        ++frames;
    }
    if (frames == 0) return false;
    for (auto& value : power) value /= static_cast<double>(frames);

    /* One sixth of an octave either side of each band centre.

       Wide enough that individual notes average out -- a guitar part is a comb of harmonics, and a
       narrower band would fit the player's choice of chord rather than the rig's response -- and
       narrow enough to keep a cone-breakup peak distinguishable from the dip beside it. */
    constexpr auto halfBandwidth = 1.0f / 12.0f;
    auto measured = false;
    for (std::size_t band = 0; band < cabinetMatchBands; ++band)
    {
        const auto centre = bandFrequency(options, band);
        const auto lowest = centre * std::pow(2.0f, -halfBandwidth);
        const auto highest = centre * std::pow(2.0f, halfBandwidth);
        const auto firstBin = static_cast<std::size_t>(
            std::max(1.0, std::floor(lowest * analysisSize / sampleRate)));
        const auto lastBin = std::min(bins, static_cast<std::size_t>(
            std::ceil(highest * analysisSize / sampleRate)));
        double sum {};
        std::size_t count {};
        for (auto bin = firstBin; bin <= lastBin; ++bin) { sum += power[bin]; ++count; }
        // A band narrower than one bin -- which happens at the bottom of the range at low
        // transform sizes -- takes the nearest bin rather than reading as silence.
        if (count == 0)
        {
            const auto nearest = std::min(bins, std::max<std::size_t>(1, static_cast<std::size_t>(
                std::lround(centre * analysisSize / sampleRate))));
            sum = power[nearest];
            count = 1;
        }
        const auto mean = sum / static_cast<double>(count);
        bandsDb[band] = static_cast<float>(10.0 * std::log10(std::max(1.0e-20, mean)));
        if (mean > 1.0e-14) measured = true;
    }
    return measured;
}
} // namespace

CabinetMatchResult measureCabinetResidual(std::span<const float> reference,
                                          std::span<const float> render, double sampleRate,
                                          const CabinetMatchOptions& options)
{
    CabinetMatchResult result;
    result.frequencies.resize(cabinetMatchBands);
    for (std::size_t band = 0; band < cabinetMatchBands; ++band)
        result.frequencies[band] = bandFrequency(options, band);

    dsp::Fft fft;
    fft.prepare(analysisSize);
    std::vector<float> referenceDb, renderDb;
    if (! averageSpectrum(reference, sampleRate, options, fft, referenceDb))
    {
        result.error = "the reference is too short or too quiet to measure";
        return result;
    }
    if (! averageSpectrum(render, sampleRate, options, fft, renderDb))
    {
        result.error = "the render is too short or too quiet to measure";
        return result;
    }

    /* The difference, levelled first.

       Both spectra are normalised to their own mean across the fitted bands before differencing,
       so the residual describes *shape* and not loudness. Without this a reference mastered 6 dB
       hotter than the render would produce a flat 6 dB lift and be reported as a cabinet -- and
       the one thing a cabinet is not is a gain control. */
    const auto meanOf = [](const std::vector<float>& values)
    {
        auto sum = 0.0f;
        for (const auto value : values) sum += value;
        return sum / static_cast<float>(values.size());
    };
    const auto referenceMean = meanOf(referenceDb);
    const auto renderMean = meanOf(renderDb);

    result.residualDb.resize(cabinetMatchBands);
    auto totalCorrection = 0.0;
    auto clampedBands = 0;
    for (std::size_t band = 0; band < cabinetMatchBands; ++band)
    {
        const auto raw = (referenceDb[band] - referenceMean) - (renderDb[band] - renderMean);
        const auto clamped = std::clamp(raw, -options.limitDb, options.limitDb);
        if (std::abs(raw) > options.limitDb) ++clampedBands;
        result.residualDb[band] = clamped;
        totalCorrection += static_cast<double>(clamped) * clamped;
    }

    /* Confidence, from how much correction is being asked for.

       A residual near zero means the render already matches and a cabinet has little to add -- a
       high-confidence result that happens to be uninteresting. A residual pinned at the limit
       across the band means the two signals are not comparable, and the fit is guesswork with a
       number attached. Both ends have to be distinguishable from the middle, so this is the RMS
       correction mapped onto a falling curve, with bands that hit the clamp counted twice: a
       clamp is not merely a large correction, it is evidence the model has run out of range. */
    const auto rmsCorrection = std::sqrt(totalCorrection / static_cast<double>(cabinetMatchBands));
    const auto clampPenalty = static_cast<double>(clampedBands) / static_cast<double>(cabinetMatchBands);
    result.confidence = static_cast<float>(
        std::clamp(1.0 - rmsCorrection / 18.0 - clampPenalty, 0.0, 1.0));
    result.valid = true;
    return result;
}

std::vector<CabinetMatchCandidate> matchFromLibrary(const CabinetMatchResult& residual,
                                                    const CabinetModelSettings& measurementCabinet,
                                                    std::size_t count)
{
    if (! residual.valid || residual.residualDb.size() != cabinetMatchBands) return {};

    // The measurement cabinet's own contribution, subtracted from every candidate: the residual
    // was measured through it, so swapping it for another cabinet changes the output by the
    // *difference* between the two and not by the new one alone.
    std::vector<float> measurementDb(cabinetMatchBands);
    for (std::size_t band = 0; band < cabinetMatchBands; ++band)
        measurementDb[band] = cabinetMagnitudeDb(measurementCabinet, residual.frequencies[band]);

    // A coarse grid over the continuous axes. Three positions and three distances is enough to
    // tell a close cap mic from a distant edge one, which is the distinction that matters; the
    // user then has continuous controls to refine it by ear.
    constexpr std::array positions { 0.1f, 0.4f, 0.8f };
    constexpr std::array distances { 1.5f, 4.0f, 12.0f };

    std::vector<CabinetMatchCandidate> candidates;
    const auto initialError = [&]
    {
        auto sum = 0.0;
        for (const auto value : residual.residualDb) sum += static_cast<double>(value) * value;
        return std::sqrt(sum / static_cast<double>(cabinetMatchBands));
    }();

    for (std::size_t cabinet = 0; cabinet < static_cast<std::size_t>(CabinetKind::count); ++cabinet)
    {
        // The legacy stand-in is not a cabinet anybody is trying to match to; it exists so old
        // projects keep their sound, and offering it as a "best match" would be nonsense.
        if (static_cast<CabinetKind>(cabinet) == CabinetKind::legacy) continue;
        for (std::size_t microphone = 0;
             microphone < static_cast<std::size_t>(MicrophoneKind::count); ++microphone)
            for (const auto position : positions)
                for (const auto distance : distances)
                {
                    CabinetModelSettings settings;
                    settings.cabinet = static_cast<CabinetKind>(cabinet);
                    settings.microphone = static_cast<MicrophoneKind>(microphone);
                    settings.position = position;
                    settings.distanceInches = distance;

                    /* What this candidate would leave behind, levelled the same way the residual
                       was. Without removing the candidate's own mean, a cabinet that is simply
                       quieter than the measurement one would score as a correction. */
                    std::vector<float> change(cabinetMatchBands);
                    auto mean = 0.0f;
                    for (std::size_t band = 0; band < cabinetMatchBands; ++band)
                    {
                        change[band] = cabinetMagnitudeDb(settings, residual.frequencies[band])
                                     - measurementDb[band];
                        mean += change[band];
                    }
                    mean /= static_cast<float>(cabinetMatchBands);

                    auto sum = 0.0;
                    for (std::size_t band = 0; band < cabinetMatchBands; ++band)
                    {
                        const auto remaining = residual.residualDb[band] - (change[band] - mean);
                        sum += static_cast<double>(remaining) * remaining;
                    }
                    const auto remaining = std::sqrt(sum / static_cast<double>(cabinetMatchBands));
                    CabinetMatchCandidate candidate;
                    candidate.settings = settings;
                    candidate.remainingDb = static_cast<float>(remaining);
                    // How much of the error it removed, so a score of 0 means "no better than
                    // leaving the measurement cabinet where it is" rather than "no match".
                    candidate.score = initialError <= 1.0e-6 ? 0.0f
                        : static_cast<float>(std::clamp(1.0 - remaining / initialError, 0.0, 1.0));
                    candidates.push_back(candidate);
                }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& first, const auto& second) { return first.score > second.score; });
    if (candidates.size() > count) candidates.resize(count);
    return candidates;
}

std::vector<float> synthesizeMatchedCabinet(const CabinetMatchResult& residual,
                                            const CabinetModelSettings& measurementCabinet,
                                            float depth, double sampleRate, std::size_t taps)
{
    if (! residual.valid || residual.residualDb.size() != cabinetMatchBands) return {};
    const auto scale = std::clamp(depth, 0.0f, 1.0f);

    /* The measurement cabinet plus the scaled residual, interpolated in log-frequency.

       Interpolated rather than stepped: the bands are a sixth of an octave apart and a staircase
       between them would put forty-one small discontinuities into the magnitude response, each of
       which is a little ringing in the impulse. Outside the fitted range the correction is held at
       its end value rather than falling to zero, so there is no cliff at 70 Hz or 8 kHz -- the
       range limits exist to stop the fit chasing drum leakage, not to notch the cabinet. */
    const auto correctionAt = [&residual, scale](float frequency)
    {
        const auto& frequencies = residual.frequencies;
        if (frequency <= frequencies.front()) return residual.residualDb.front() * scale;
        if (frequency >= frequencies.back()) return residual.residualDb.back() * scale;
        const auto upper = std::lower_bound(frequencies.begin(), frequencies.end(), frequency);
        const auto index = static_cast<std::size_t>(std::distance(frequencies.begin(), upper));
        const auto lowFrequency = frequencies[index - 1];
        const auto highFrequency = frequencies[index];
        const auto position = std::log(frequency / lowFrequency) / std::log(highFrequency / lowFrequency);
        const auto low = residual.residualDb[index - 1];
        const auto high = residual.residualDb[index];
        return (low + (high - low) * position) * scale;
    };

    return minimumPhaseImpulse(
        [&measurementCabinet, &correctionAt](float frequency)
        { return cabinetMagnitudeDb(measurementCabinet, frequency) + correctionAt(frequency); },
        sampleRate, taps);
}
} // namespace nts::ir
