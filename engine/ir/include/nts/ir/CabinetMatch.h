#pragma once

#include "CabinetModel.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

/** Fitting a cabinet to a reference recording.

    **Why this is a different kind of problem from fitting an amplifier, and a much easier one.**
    The rig reconstruction searches: it renders a candidate amplifier, measures it, scores it
    against the reference and tries again, because an amplifier is non-linear and there is no
    closed form for "what drive setting produces this spectrum". A cabinet is a *linear,
    time-invariant filter*. The difference between two spectra **is** the filter that turns one
    into the other, and recovering it costs one transform rather than a hundred renders.

    That matters beyond the arithmetic. Cabinet colouration lands on the same measurement the amp
    search reads as brightness, so a dark reference pushes the fit towards a darker **amplifier**
    when what was dark was the speaker. Fitting the cabinet separately, afterwards, is what stops
    the expensive search spending its pool on a problem with a cheap exact answer.

    **What this cannot do, and the interface has to say so.** The residual is a long-term average
    spectrum. It carries nothing about the reference's transient behaviour or its non-linearity,
    and it cannot tell a speaker apart from the microphone, the room, the bus compression and the
    mastering EQ that were recorded with it. A synthesised match therefore absorbs all of them into
    something labelled "cabinet". That is why `matchFromLibrary` exists alongside it and is the one
    to offer first: a real cabinet with a name generalises to the next song, and a curve fitted to
    one mix does not.
*/
namespace nts::ir
{
/// Bands the residual is measured in. One sixth of an octave from 70 Hz to 8 kHz, which is 41
/// bands -- fine enough to see a cone-breakup peak, coarse enough not to fit individual notes.
inline constexpr std::size_t cabinetMatchBands = 41;

struct CabinetMatchOptions
{
    /** Below this the residual is held flat, because bass and kick leak into a guitar stem there
        and a fit would put their energy into the speaker. */
    float lowLimitHz { 70.0f };
    /// Above this the same is true of cymbals, and no guitar speaker has content up there anyway.
    float highLimitHz { 8000.0f };
    /** Ceiling on the correction, in dB either way. A cabinet is not a 30 dB filter; a residual
        that large is a mix decision, or evidence that the render and the reference are not
        comparable, and either way clamping it is more honest than applying it. */
    float limitDb { 12.0f };
};

struct CabinetMatchResult
{
    /// Band centre frequencies, log-spaced across the fitted range.
    std::vector<float> frequencies;
    /// Reference minus render, in dB, smoothed, band-limited and clamped.
    std::vector<float> residualDb;
    /** How much of the reference's spectrum the residual actually explains, 0 to 1.

        Derived from how much correction is being asked for: a residual sitting near zero means
        the render already matches and there is little for a cabinet to do, while a residual
        pinned at the limit across the whole band means the two signals are not comparable and the
        fit is guesswork wearing a number. Reported rather than acted on -- the caller decides
        whether to believe it, and the interface says so either way.
    */
    float confidence {};
    /// True when there was enough signal in both inputs to measure anything at all.
    bool valid {};
    std::string error;
};

/** Measures what a cabinet would have to do to turn `render` into `reference`.

    Both are expected to be the *same performance* -- the reconstruction renders the user's DI
    through the applied rig, and the reference is the region that rig was matched from. They are
    compared as long-term average spectra, so they do not need to be sample-aligned, but they do
    need to be the same music: comparing a render of one riff against a recording of another
    measures the arrangement rather than the rig.
*/
[[nodiscard]] CabinetMatchResult measureCabinetResidual(std::span<const float> reference,
                                                        std::span<const float> render,
                                                        double sampleRate,
                                                        const CabinetMatchOptions& options = {});

struct CabinetMatchCandidate
{
    CabinetModelSettings settings;
    /// How much of the residual this cabinet removes, 0 to 1. Higher is better.
    float score {};
    /// What is left after choosing it, in dB RMS across the fitted bands.
    float remainingDb {};
};

/** Ranks built-in cabinets by how much of the residual each one removes.

    Scored against the *measurement cabinet* the render was made through, because the residual
    already contains that cabinet's own response: swapping cabinet A for cabinet B changes the
    output by `B - A`, not by `B`. Getting this wrong produces a shortlist that is confidently
    upside down, which is worse than no shortlist.

    Returns at most `count` candidates, best first. The search is exhaustive over the model's two
    tables and a small grid of positions and distances -- a few hundred curve evaluations, which is
    microseconds, and is why this needs no cleverness.
*/
[[nodiscard]] std::vector<CabinetMatchCandidate> matchFromLibrary(
    const CabinetMatchResult& residual, const CabinetModelSettings& measurementCabinet,
    std::size_t count = 3);

/** Builds a minimum-phase impulse from the residual, scaled by `depth`.

    Fits better than any library pick and generalises worse, for the reason given at the top of
    this file. `depth` is 0 to 1 and scales the correction in dB, so 0 is a flat response and 1
    applies the whole measured difference.

    The result is intended to sit in a cabinet slot *in place of* the response the render was made
    through -- so it carries the measurement cabinet's own response as well as the correction, and
    is a complete cabinet rather than an EQ to be stacked on one.
*/
[[nodiscard]] std::vector<float> synthesizeMatchedCabinet(const CabinetMatchResult& residual,
                                                          const CabinetModelSettings& measurementCabinet,
                                                          float depth, double sampleRate,
                                                          std::size_t taps = 1024);
} // namespace nts::ir
