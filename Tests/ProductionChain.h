#pragma once

/** A synthetic production chain with exact labels, and the analytic response of its EQ.

    **This is the generator, and it is the harness's weakest link.** `nts_production_recovery`
    scores `estimateProductionProfile` by applying a known chain and asking how much of it comes
    back. That is self-supervision -- the labels are exact because they were chosen -- and it is
    the same trick `nts_rig_recovery` relies on. The cost is that the harness and the thing it
    measures share an idea of what a mix is. If the reverb below is nothing like a plate, the
    estimator will read plates badly and *this harness will not notice*, because it never sees
    one. `nts_song_probe` against real material is the cross-check, and it is not optional before
    any of these numbers are used to justify a change.

    Kept deliberately simple and deliberately deterministic. It is not trying to sound good: it is
    trying to apply a known, measurable amount of a known effect, so that "how much came back" is
    a number rather than an opinion. Every stage is bypassed exactly when its control is zero, so
    a swept field is the only thing moving.

    Stage order is the order a mix is built in -- dynamics, then tone, then space, then level --
    because applying the level first would make the compressor's threshold mean something
    different at every setting and turn one swept field into two.

    P9 of the production-match plan promotes this to `ml/nts_ml/` as a training-data generator.
    That is why the settings are a plain struct of labels rather than arguments: the pair
    `(rendered audio, ProductionSettings)` is already the training example.
*/

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace nts::recovery
{
/// What was applied, which is also the label. Every field is zero-is-bypassed.
struct ProductionSettings
{
    /// Gain in dB applied last, after everything else. The thing `levelOffsetDb` should recover.
    float levelOffsetDb {};
    /** High-shelf height in dB; positive is brighter. The thing the spectral residual should
        recover, and the only field with an exact analytic answer -- see `shelfResponseDb`. */
    float tiltDb {};
    float tiltHz { 1000.0f };
    /// 0..1, into a small Schroeder network. Not a plate and not a room; see the note above.
    float reverbAmount {};
    /// 0..1, a single delayed repeat at `delayMs`.
    float delayAmount {};
    float delayMs { 220.0f };
    /// 0..1, a short delayed copy panned opposite the dry -- what a double-track does to an image.
    float doubling {};
    /// 0..1, a peak compressor above -18 dBFS. Ratio and makeup both follow the amount.
    float compression {};
};

/** The top of each sampled range, and the divisor that puts a field on the 0..1 the model uses.

    **These four numbers are duplicated in `ml/nts_ml/production/generator.py` as
    `ProductionRanges`, and the duplication is checked rather than trusted**: the harness's
    `--ranges` mode prints them and `ml/tests/production_parity_driver.py` asserts they match. If
    they drifted, the model would be trained against one normalisation and scored against another,
    and both halves would look internally consistent while disagreeing about what 0.5 means.

    They exist at all because "how much room" has no natural unit. Both the deterministic estimator
    and the learned one report 0..1, so the applied amount has to be expressed on the same axis for
    the two to be comparable at all -- and the axis has to be the sampled range, or a sigmoid
    spends most of its output on values that never occur.
*/
struct ProductionCeilings
{
    float reverb { 0.63f };
    float delay { 0.55f };
    float doubling { 0.90f };
    float compression { 0.80f };
};

inline constexpr ProductionCeilings productionCeilings {};

/** The high shelf's magnitude response in dB at `frequency`.

    Exact rather than measured, so scoring the spectral residual compares it against what was
    actually applied instead of against a second estimate of it. The stage is
    `y = x + g (x - lowpass(x))` with a one-pole lowpass, so

        H(z) = 1 + g a (1 - z^-1) / (1 - a z^-1)

    and this evaluates it on the unit circle.
*/
[[nodiscard]] inline double shelfResponseDb(const ProductionSettings& settings, double frequency,
                                            double sampleRate)
{
    if (std::abs(settings.tiltDb) < 1.0e-6) return 0.0;
    const auto pole = std::exp(-2.0 * std::numbers::pi * settings.tiltHz / sampleRate);
    const auto gain = std::pow(10.0, settings.tiltDb / 20.0) - 1.0;
    const std::complex<double> unit { 0.0, 2.0 * std::numbers::pi * frequency / sampleRate };
    const auto z = std::exp(-unit);   // z^-1
    const auto response = 1.0 + gain * pole * (1.0 - z) / (1.0 - pole * z);
    return 20.0 * std::log10(std::max(std::abs(response), 1.0e-9));
}

/** Applies the chain to a mono render, returning stereo.

    Stereo because half of what is being measured is spatial: a doubled, widened part is not a
    mono signal with a number attached to it, and handing the analyser a mono buffer would make
    `stereoWidth` and `doubleTrackingLikelihood` unmeasurable by construction.
*/
[[nodiscard]] inline std::pair<std::vector<float>, std::vector<float>> applyProductionChain(
    std::span<const float> dry, double sampleRate, const ProductionSettings& settings)
{
    const auto count = dry.size();
    std::vector<float> left(dry.begin(), dry.end());
    std::vector<float> right(dry.begin(), dry.end());
    const auto sampleAt = [count](const std::vector<float>& channel, std::size_t index,
                                  std::size_t delay) -> float
    { return index >= delay && index < count ? channel[index - delay] : 0.0f; };

    // ---- Dynamics. A peak follower into a fixed -18 dBFS threshold, so the only thing the
    // amount changes is the ratio and the makeup that compensates for it.
    if (settings.compression > 1.0e-4f)
    {
        const auto ratio = 1.0f + settings.compression * 7.0f;
        const auto threshold = std::pow(10.0f, -18.0f / 20.0f);
        const auto attack = std::exp(-1.0f / static_cast<float>(sampleRate * 0.005));
        const auto release = std::exp(-1.0f / static_cast<float>(sampleRate * 0.120));
        const auto makeup = std::pow(10.0f, settings.compression * 4.0f / 20.0f);
        auto envelope = 0.0f;
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto level = std::abs(left[index]);
            const auto coefficient = level > envelope ? attack : release;
            envelope = coefficient * envelope + (1.0f - coefficient) * level;
            auto gain = 1.0f;
            if (envelope > threshold)
                gain = (threshold + (envelope - threshold) / ratio) / envelope;
            left[index] *= gain * makeup;
            right[index] = left[index];
        }
    }

    // ---- Tone. The shelf `shelfResponseDb` describes analytically.
    if (std::abs(settings.tiltDb) > 1.0e-6f)
    {
        const auto pole = static_cast<float>(std::exp(-2.0 * std::numbers::pi * settings.tiltHz / sampleRate));
        const auto gain = std::pow(10.0f, settings.tiltDb / 20.0f) - 1.0f;
        auto lowpass = 0.0f;
        for (std::size_t index = 0; index < count; ++index)
        {
            lowpass = lowpass * pole + left[index] * (1.0f - pole);
            left[index] += (left[index] - lowpass) * gain;
            right[index] = left[index];
        }
    }

    /* ---- Doubling. A short delayed copy sent to the opposite side.

       Deliberately *not* symmetric: a real double-track is two performances, and the closest
       honest approximation with one performance in hand is one image sitting a few milliseconds
       behind the other. The delay is scaled so the estimator sees a spread rather than a comb at
       one fixed frequency. */
    if (settings.doubling > 1.0e-4f)
    {
        const auto delay = static_cast<std::size_t>(sampleRate * (0.008 + 0.014 * settings.doubling));
        const auto amount = settings.doubling * 0.85f;
        const auto dryLeft = left;
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto late = sampleAt(dryLeft, index, delay);
            left[index] = dryLeft[index] * (1.0f - amount * 0.35f) + late * amount * 0.25f;
            right[index] = dryLeft[index] * (1.0f - amount * 0.35f) * (1.0f - amount * 0.6f)
                         + late * amount * 0.95f;
        }
    }

    // ---- A single delayed repeat. One tap rather than a feedback line, so `delayMs` is
    // unambiguous and a swept amount changes the amount and nothing else.
    if (settings.delayAmount > 1.0e-4f)
    {
        const auto delay = static_cast<std::size_t>(sampleRate * settings.delayMs / 1000.0);
        const auto amount = settings.delayAmount * 0.5f;
        const auto dryLeft = left;
        const auto dryRight = right;
        for (std::size_t index = 0; index < count; ++index)
        {
            left[index] += sampleAt(dryRight, index, delay) * amount;
            right[index] += sampleAt(dryLeft, index, delay) * amount * 0.9f;
        }
    }

    /* ---- Space. Four combs into two all-passes, which is the smallest thing that produces a
       decaying diffuse tail rather than a discrete echo. The prime-ish delay lengths are what
       stop the combs lining up into a pitched ring. */
    if (settings.reverbAmount > 1.0e-4f)
    {
        constexpr double combMs[] = { 29.7, 37.1, 41.1, 43.7 };
        constexpr double allPassMs[] = { 5.0, 1.7 };
        const auto feedback = 0.70f + settings.reverbAmount * 0.16f;
        const auto mix = settings.reverbAmount * 0.55f;
        const auto dryLeft = left;
        const auto dryRight = right;
        std::vector<float> wetLeft(count, 0.0f), wetRight(count, 0.0f);
        for (std::size_t comb = 0; comb < std::size(combMs); ++comb)
        {
            // The two sides get slightly different lengths, which is what makes the tail wide
            // rather than a mono tail sitting in the middle.
            const auto lengthLeft = static_cast<std::size_t>(sampleRate * combMs[comb] / 1000.0);
            const auto lengthRight = lengthLeft + 37 + comb * 11;
            std::vector<float> stateLeft(count, 0.0f), stateRight(count, 0.0f);
            for (std::size_t index = 0; index < count; ++index)
            {
                stateLeft[index] = dryLeft[index] + feedback * sampleAt(stateLeft, index, lengthLeft);
                stateRight[index] = dryRight[index] + feedback * sampleAt(stateRight, index, lengthRight);
                wetLeft[index] += stateLeft[index] * 0.25f;
                wetRight[index] += stateRight[index] * 0.25f;
            }
        }
        for (const auto milliseconds : allPassMs)
        {
            const auto length = static_cast<std::size_t>(sampleRate * milliseconds / 1000.0);
            auto pass = [&](std::vector<float>& channel)
            {
                std::vector<float> out(count, 0.0f);
                for (std::size_t index = 0; index < count; ++index)
                    out[index] = -0.5f * channel[index] + sampleAt(channel, index, length)
                               + 0.5f * sampleAt(out, index, length);
                channel = std::move(out);
            };
            pass(wetLeft);
            pass(wetRight);
        }
        for (std::size_t index = 0; index < count; ++index)
        {
            left[index] = dryLeft[index] * (1.0f - mix) + wetLeft[index] * mix * 0.22f;
            right[index] = dryRight[index] * (1.0f - mix) + wetRight[index] * mix * 0.22f;
        }
    }

    // ---- Level, last, so nothing downstream of it has a threshold that moves with it.
    if (std::abs(settings.levelOffsetDb) > 1.0e-6f)
    {
        const auto gain = std::pow(10.0f, settings.levelOffsetDb / 20.0f);
        for (std::size_t index = 0; index < count; ++index) { left[index] *= gain; right[index] *= gain; }
    }
    return { std::move(left), std::move(right) };
}
} // namespace nts::recovery
