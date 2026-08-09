#include <nts/ir/CabinetModel.h>

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
/** One enclosure, as the few numbers that actually separate one from another.

    Deliberately small. A cabinet's identity in a mix is its resonance frequency and how sharp it
    is, where the top end stops, and what the cone does in between -- four numbers and a pair of
    bumps. Adding a dozen more would give the illusion of physical modelling without the substance
    of it, which is the failure mode this table exists to avoid.
*/
struct CabinetVoicing
{
    std::string_view name;
    /// Free-air resonance of the driver in its box. The thump, and where the bottom stops.
    float resonanceHz;
    /// How sharp that resonance is. Sealed boxes ring; open backs damp and leak.
    float resonanceQ;
    /// Peak height at resonance, in dB.
    float resonanceDb;
    /// Slope below resonance, in dB per octave. 12 for a sealed box, 6 for a dipole open back.
    float lowSlopeDbPerOctave;
    /// Where cone mass takes the top end away.
    float highCornerHz;
    /// How fast, in dB per octave. A guitar speaker is startlingly steep here.
    float highSlopeDbPerOctave;
    /// The two cone-breakup peaks, which are most of what "which cabinet is this" sounds like.
    float firstBreakupHz, firstBreakupDb;
    float secondBreakupHz, secondBreakupDb;
    /// Broad low-mid weight, in dB, centred around 400 Hz. Positive is boxy, negative is scooped.
    float bodyDb;
};

constexpr std::array<CabinetVoicing, static_cast<std::size_t>(CabinetKind::count)> cabinets { {
    // Closed 4x12: the tight, weighted one. High resonance Q, hard stop above 5 kHz.
    { "Closed 4x12",   95.0f, 1.25f, 5.5f, 12.0f, 4900.0f, 30.0f, 1750.0f,  4.0f, 3100.0f, -5.0f,  1.5f },
    // Open 2x12: lower Q and a 6 dB/oct dipole roll-off, which is why an open back sounds
    // lighter underneath rather than merely quieter.
    { "Open 2x12",     88.0f, 0.72f, 3.0f,  6.0f, 5400.0f, 26.0f, 1900.0f,  3.2f, 3400.0f, -3.5f, -1.0f },
    { "Combo 1x12",   105.0f, 0.85f, 4.0f,  8.0f, 5100.0f, 24.0f, 2100.0f,  4.5f, 3600.0f, -2.5f,  0.5f },
    // Bass cabinets: an octave lower, far more extended on top, and no breakup peaks worth
    // speaking of -- a 10" bass driver is a much better piston than a guitar 12".
    { "Bass 4x10",     58.0f, 0.95f, 4.5f, 12.0f, 3800.0f, 14.0f, 1500.0f,  1.5f, 2600.0f, -1.5f,  0.0f },
    { "Bass 8x10",     48.0f, 1.10f, 5.5f, 12.0f, 3200.0f, 18.0f, 1200.0f,  1.0f, 2400.0f, -2.0f,  2.0f },
    // The three the circuit engine used to carry, kept reachable. `reactive` was its default and
    // was the most neutral of them; the other two are named for what they were.
    { "Reactive 2x12", 92.0f, 1.00f, 4.5f, 10.0f, 5200.0f, 27.0f, 1850.0f,  3.6f, 3200.0f, -4.0f,  0.5f },
    { "Open back 1x12",84.0f, 0.65f, 2.5f,  6.0f, 5800.0f, 22.0f, 2000.0f,  3.0f, 3500.0f, -3.0f, -2.0f },
    { "Bass sealed",   52.0f, 1.20f, 6.0f, 12.0f, 3000.0f, 20.0f, 1100.0f,  0.8f, 2200.0f, -1.5f,  2.5f },
    /* The original built-in responses, as a curve. See `CabinetKind::legacy`.

       A near-flat row with one gentle resonance, because that is roughly what `0.72·δ +
       0.16·e^(-t/68)·sin(0.31t)` measures as: a spike with a small damped ring on it and no
       meaningful roll-off at either end. The *audio* for this entry does not come from here -- the
       original samples are used verbatim -- so this row exists only so the response plot and the
       matcher have something to draw and score. */
    { "Legacy built-in", 380.0f, 0.60f, 2.0f, 4.0f, 16000.0f, 6.0f, 2400.0f, 0.5f, 5000.0f, -0.5f, 0.0f },
} };

/** One microphone, as the curve it imposes on whatever is in front of it.

    `proximityStrength` scales the low-frequency lift a pressure-gradient microphone gets close up.
    A ribbon is figure-of-eight and gets a great deal of it; a moving coil is closer to
    omnidirectional at the bottom and gets much less. That difference is the reason a ribbon two
    inches from a grille sounds enormous and the same ribbon a foot away sounds thin.
*/
struct MicrophoneVoicing
{
    std::string_view name;
    /// Presence peak: where, how tall, how sharp.
    float presenceHz, presenceDb, presenceQ;
    /// Where the capsule itself stops, and how fast.
    float highCornerHz, highSlopeDbPerOctave;
    /// Its own low roll-off, before proximity is added back.
    float lowCornerHz, lowSlopeDbPerOctave;
    float proximityStrength;
    /// How much of the room this microphone hears at a given distance, relative to the others.
    float roomSensitivity;
};

constexpr std::array<MicrophoneVoicing, static_cast<std::size_t>(MicrophoneKind::count)> microphones { {
    { "Dynamic (presence)", 5200.0f,  5.0f, 1.10f, 13000.0f, 12.0f, 110.0f, 6.0f, 0.55f, 0.7f },
    { "Dynamic (full)",     3200.0f,  3.0f, 0.85f, 15000.0f, 10.0f,  60.0f, 6.0f, 0.70f, 0.8f },
    { "Ribbon",             2600.0f,  1.5f, 0.70f,  6200.0f, 18.0f,  45.0f, 6.0f, 1.00f, 1.0f },
    { "Condenser",          9000.0f,  3.5f, 0.80f, 19000.0f,  8.0f,  35.0f, 6.0f, 0.35f, 1.2f },
    { "Room pair",          1800.0f,  1.0f, 0.60f, 14000.0f, 10.0f,  55.0f, 6.0f, 0.15f, 2.4f },
} };

/// A resonant bell in dB, from a normalised frequency ratio. Used for every peak in the model, so
/// there is one shape and not five slightly different ones.
[[nodiscard]] float bellDb(float frequencyHz, float centreHz, float gainDb, float q) noexcept
{
    if (frequencyHz <= 0.0f || centreHz <= 0.0f || gainDb == 0.0f) return 0.0f;
    // Octaves from centre, weighted by Q. A bell in log-frequency is what a listener hears as a
    // symmetrical bump; one in linear frequency is not.
    const auto octaves = std::log2(frequencyHz / centreHz) * q;
    return gainDb * std::exp(-octaves * octaves * 2.0f);
}

/// A one-sided slope in dB: flat inside the corner, falling at `slope` dB per octave outside it.
[[nodiscard]] float shelfDb(float frequencyHz, float cornerHz, float slopeDbPerOctave,
                            bool fallAbove) noexcept
{
    if (frequencyHz <= 0.0f || cornerHz <= 0.0f) return 0.0f;
    const auto octaves = std::log2(frequencyHz / cornerHz);
    if (fallAbove) return octaves > 0.0f ? -slopeDbPerOctave * octaves : 0.0f;
    return octaves < 0.0f ? slopeDbPerOctave * octaves : 0.0f;
}
} // namespace

std::string_view cabinetName(CabinetKind kind) noexcept
{
    const auto index = static_cast<std::size_t>(kind);
    return index < cabinets.size() ? cabinets[index].name : cabinets.front().name;
}

std::string_view microphoneName(MicrophoneKind kind) noexcept
{
    const auto index = static_cast<std::size_t>(kind);
    return index < microphones.size() ? microphones[index].name : microphones.front().name;
}

float cabinetMagnitudeDb(const CabinetModelSettings& settings, float frequencyHz) noexcept
{
    if (frequencyHz <= 0.0f) return -120.0f;
    const auto cabinetIndex = std::min(static_cast<std::size_t>(settings.cabinet), cabinets.size() - 1);
    const auto microphoneIndex = std::min(static_cast<std::size_t>(settings.microphone),
                                          microphones.size() - 1);
    const auto& cabinet = cabinets[cabinetIndex];
    const auto& microphone = microphones[microphoneIndex];
    const auto position = std::clamp(settings.position, 0.0f, 1.0f);
    const auto distance = std::clamp(settings.distanceInches, 1.0f, 24.0f);

    auto decibels = 0.0f;

    // ---- The driver in its box ----------------------------------------------------------
    decibels += bellDb(frequencyHz, cabinet.resonanceHz, cabinet.resonanceDb, cabinet.resonanceQ);
    decibels += shelfDb(frequencyHz, cabinet.resonanceHz, cabinet.lowSlopeDbPerOctave, false);
    decibels += shelfDb(frequencyHz, cabinet.highCornerHz, cabinet.highSlopeDbPerOctave, true);
    decibels += bellDb(frequencyHz, 400.0f, cabinet.bodyDb, 0.55f);

    /* Cone breakup, and why it moves with microphone position.

       The peaks a 12" guitar speaker makes between 1.5 and 4 kHz are the cone ceasing to move as
       one piece, and they are strongest on the dust cap where that motion is most concentrated.
       Off at the edge they are markedly weaker -- which is the physical reason edge-of-cone is the
       smoother position, and getting it from the same parameter that darkens the top end is what
       makes one control behave like moving a real microphone rather than like two EQs. */
    const auto breakupScale = 1.0f - position * 0.55f;
    decibels += bellDb(frequencyHz, cabinet.firstBreakupHz,
                       cabinet.firstBreakupDb * breakupScale, 2.2f);
    decibels += bellDb(frequencyHz, cabinet.secondBreakupHz,
                       cabinet.secondBreakupDb * breakupScale, 2.6f);

    // ---- Where the microphone is pointed -------------------------------------------------
    /* Off-axis is a low-pass whose corner falls as the microphone moves out to the edge, plus a
       little low-mid gain because the edge of the cone radiates the bottom more evenly. Both are
       what a player hears when they slide a microphone across a grille, and the low-pass is much
       the larger of the two. */
    const auto axisCornerHz = 6500.0f * std::pow(0.32f, position);
    decibels += shelfDb(frequencyHz, axisCornerHz, 7.5f * position + 1.5f, true);
    decibels += bellDb(frequencyHz, 260.0f, position * 2.5f, 0.7f);

    // ---- How far away it is --------------------------------------------------------------
    /* Proximity effect: a pressure-gradient capsule gains low end as it approaches, and the gain
       is roughly inverse with distance. Referenced to six inches, where it is taken to be zero, so
       "close" adds bottom and "far" does not remove it -- removing it would be modelling a
       different microphone rather than the same one further away. */
    const auto proximityDb = microphone.proximityStrength * 9.0f * (6.0f / distance - 1.0f);
    decibels += shelfDb(frequencyHz, 220.0f, std::clamp(proximityDb, -4.0f, 12.0f), false)
              * -1.0f;

    /* Air, and the first reflection off the floor.

       Distance costs top end -- a real loss, though a small one over two feet -- and introduces a
       comb from the boundary the cabinet is standing on. The comb is deliberately shallow: a
       strong one would be modelling a specific room, and this model has no room in it. */
    decibels += shelfDb(frequencyHz, 20000.0f / std::max(1.0f, distance * 0.5f), 2.0f, true);
    const auto reflectionHz = 13500.0f / std::max(1.0f, distance);
    decibels += std::cos(std::numbers::pi_v<float> * frequencyHz / reflectionHz)
              * microphone.roomSensitivity * std::min(distance / 24.0f, 1.0f) * 1.8f;

    // ---- The microphone's own curve --------------------------------------------------------
    decibels += bellDb(frequencyHz, microphone.presenceHz, microphone.presenceDb, microphone.presenceQ);
    decibels += shelfDb(frequencyHz, microphone.highCornerHz, microphone.highSlopeDbPerOctave, true);
    decibels += shelfDb(frequencyHz, microphone.lowCornerHz, microphone.lowSlopeDbPerOctave, false);

    // Bounded, because everything above is a sum of unbounded slopes and the exponential in the
    // minimum-phase reconstruction is not forgiving about what it is handed.
    return std::clamp(decibels, -90.0f, 24.0f);
}

std::vector<float> minimumPhaseImpulse(const std::function<float(float)>& magnitudeDb,
                                       double sampleRate, std::size_t taps)
{
    taps = std::clamp<std::size_t>(taps, 64, 4096);
    // Four times the requested length, rounded to a power of two: the cepstral fold below needs
    // room for the impulse to decay inside the transform, or its tail wraps onto its own head.
    std::size_t transformSize = 64;
    while (transformSize < taps * 4) transformSize *= 2;

    dsp::Fft fft;
    fft.prepare(transformSize);
    std::vector<std::complex<float>> spectrum(transformSize);

    /* Step 1: the log-magnitude spectrum, which is what the caller actually describes.

       Natural log, not dB: the cepstral method below is defined on the complex logarithm, and
       feeding it decibels produces a filter 8.686 times too aggressive -- which does not look
       wrong, it looks like a cabinet with a personality disorder. */
    const auto bins = transformSize / 2;
    for (std::size_t bin = 0; bin <= bins; ++bin)
    {
        const auto frequency = static_cast<float>(static_cast<double>(bin) * sampleRate
                                                  / static_cast<double>(transformSize));
        // Bin 0 is DC, where a magnitude of zero would be a logarithm of minus infinity. Asked at
        // the first real bin instead, which is also physically right: no speaker passes DC.
        const auto safeFrequency = bin == 0 ? static_cast<float>(sampleRate / transformSize)
                                            : frequency;
        const auto logMagnitude = std::clamp(magnitudeDb(safeFrequency), -90.0f, 24.0f) * 0.11512925f;
        spectrum[bin] = { logMagnitude, 0.0f };
        if (bin > 0 && bin < bins) spectrum[transformSize - bin] = { logMagnitude, 0.0f };
    }

    /* Step 2: the real cepstrum, and the fold that makes it causal.

       This is the whole minimum-phase construction. The cepstrum of a minimum-phase sequence is
       causal, so folding the anticausal half onto the causal one -- keeping bin 0 and the Nyquist
       bin, doubling everything between, zeroing the rest -- produces the cepstrum of the
       minimum-phase sequence with the same magnitude response. Exponentiating back gives a filter
       whose energy is packed as early as it can be, which is what a speaker's is. */
    fft.transform(spectrum, true);
    for (std::size_t index = 1; index < bins; ++index)
    {
        spectrum[index] *= 2.0f;
        spectrum[transformSize - index] = { 0.0f, 0.0f };
    }

    // Step 3: back to a complex log spectrum, exponentiate, and back to the time domain.
    fft.transform(spectrum, false);
    for (auto& bin : spectrum) bin = std::exp(bin);
    fft.transform(spectrum, true);

    std::vector<float> impulse(taps, 0.0f);
    for (std::size_t index = 0; index < taps; ++index) impulse[index] = spectrum[index].real();

    /* A raised-cosine fade over the last eighth, then peak normalisation.

       The fade is not cosmetic: a minimum-phase impulse is front-loaded but not finite, so cutting
       it at `taps` leaves a step, and a step in an impulse is broadband splatter at the top of the
       spectrum -- audible as a thin crackle over the whole cabinet. */
    const auto fadeLength = std::max<std::size_t>(1, taps / 8);
    for (std::size_t index = 0; index < fadeLength; ++index)
    {
        const auto position = static_cast<float>(index) / static_cast<float>(fadeLength);
        const auto gain = 0.5f * (1.0f + std::cos(std::numbers::pi_v<float> * position));
        impulse[taps - fadeLength + index] *= gain;
    }

    auto peak = 0.0f;
    for (const auto sample : impulse) peak = std::max(peak, std::abs(sample));
    // Silence rather than a division by something near zero. Reachable only if the caller asks for
    // a response entirely below the clamp's floor.
    if (peak < 1.0e-9f) return std::vector<float>(taps, 0.0f);
    // -1 dBFS, matching what a user's loaded response is normalised to, so switching between a
    // model and a file is a change of cabinet rather than a change of level.
    const auto scale = 0.891251f / peak;
    for (auto& sample : impulse) sample *= scale;
    return impulse;
}

std::vector<float> renderCabinetImpulse(const CabinetModelSettings& settings, double sampleRate,
                                        std::size_t taps)
{
    return minimumPhaseImpulse([&settings](float frequency)
                               { return cabinetMagnitudeDb(settings, frequency); },
                               sampleRate, taps);
}

std::vector<float> minimumPhaseFromImpulse(std::span<const float> impulse, std::size_t taps)
{
    if (impulse.empty()) return {};
    taps = std::clamp<std::size_t>(taps, 64, 4096);
    std::size_t transformSize = 64;
    while (transformSize < std::max(taps, impulse.size()) * 4) transformSize *= 2;

    dsp::Fft fft;
    fft.prepare(transformSize);
    std::vector<std::complex<float>> spectrum(transformSize, { 0.0f, 0.0f });
    for (std::size_t index = 0; index < std::min(impulse.size(), transformSize); ++index)
        spectrum[index] = { impulse[index], 0.0f };
    fft.transform(spectrum, false);

    /* The magnitude, floored before its logarithm is taken.

       A measured response can have genuine nulls in it -- a comb from a boundary reflection puts
       exact zeros in the spectrum -- and the logarithm of zero is what turns the whole cepstrum
       into NaN. Floored 100 dB below the peak, which is far under anything audible and is a
       number rather than an infinity. */
    auto peak = 0.0f;
    for (const auto& bin : spectrum) peak = std::max(peak, std::abs(bin));
    if (peak <= 0.0f) return std::vector<float>(taps, 0.0f);
    const auto floorMagnitude = peak * 1.0e-5f;
    for (auto& bin : spectrum)
        bin = { std::log(std::max(std::abs(bin), floorMagnitude)), 0.0f };

    // The same causal fold as `minimumPhaseImpulse`; see the note there for why it works.
    const auto bins = transformSize / 2;
    fft.transform(spectrum, true);
    for (std::size_t index = 1; index < bins; ++index)
    {
        spectrum[index] *= 2.0f;
        spectrum[transformSize - index] = { 0.0f, 0.0f };
    }
    fft.transform(spectrum, false);
    for (auto& bin : spectrum) bin = std::exp(bin);
    fft.transform(spectrum, true);

    std::vector<float> result(taps, 0.0f);
    for (std::size_t index = 0; index < taps; ++index) result[index] = spectrum[index].real();

    const auto fadeLength = std::max<std::size_t>(1, taps / 8);
    for (std::size_t index = 0; index < fadeLength; ++index)
    {
        const auto position = static_cast<float>(index) / static_cast<float>(fadeLength);
        const auto gain = 0.5f * (1.0f + std::cos(std::numbers::pi_v<float> * position));
        result[taps - fadeLength + index] *= gain;
    }

    // Matched to the *input's* peak rather than normalised to a fixed level: converting a response
    // must not also change how loud it is, or every blend a user had set would move.
    auto inputPeak = 0.0f;
    for (const auto sample : impulse) inputPeak = std::max(inputPeak, std::abs(sample));
    auto outputPeak = 0.0f;
    for (const auto sample : result) outputPeak = std::max(outputPeak, std::abs(sample));
    if (outputPeak > 1.0e-9f && inputPeak > 0.0f)
        for (auto& sample : result) sample *= inputPeak / outputPeak;
    return result;
}
} // namespace nts::ir
