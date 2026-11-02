#pragma once

/** The playing the recovery harnesses measure against, in one place.

    Shared by `nts_rig_recovery` and `nts_production_recovery` rather than copied into each,
    because the sample-size argument below is the whole reason either harness is trustworthy and a
    second divergent copy of it would quietly undo that. A change to the phrases changes both
    numbers, which is correct: they are measuring different stages of the same pipeline over the
    same material.
*/

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace nts::recovery
{
/** What the player is doing, which turns out to matter as much as which amplifier they are using.

    One phrase is not a benchmark. Recovery measured over a single riff at three drive settings is
    twenty-one attempts against a 14.3% chance baseline, which cannot resolve a one-attempt
    difference -- and two changes were reverted on exactly that basis, having moved the figure by
    one. Four phrases across five drive settings is 140, where the standard error is around three
    points and a real six-point improvement becomes visible.

    The phrases differ in the ways that change what the analyser sees: how many notes sound at once
    (polyphony drives intermodulation, which is most of what makes a distorted guitar spectrally
    dense), how long they ring, and how hard they are picked. All of them build partials at 1/k^2
    rather than 1/k, because a fixture that starts as rich as a sawtooth leaves distortion nothing
    to add -- a mistake this file made three times before it was measured.
*/
struct Phrase
{
    const char* name;
    std::vector<double> notes;   ///< Simultaneous notes, in Hz. One entry is a single-note line.
    double noteSeconds;          ///< How long before the next note.
    double decay;                ///< Envelope decay rate; low sustains, high is percussive.
    double level;
};

/// Which instrument's fixtures to render. See `phrases`.
enum class Fixture { guitar, bass };

inline const std::vector<Phrase>& guitarPhrases()
{
    static const std::vector<Phrase> all {
        // A low single-note riff, picked hard and stopped short: the densest, driest case.
        { "low riff",     { 82.41 },                  0.30, 7.0, 0.95 },
        // Root and fifth, which is what a rhythm guitar is playing most of the time.
        { "power chords", { 110.0, 164.81 },          0.55, 3.0, 0.90 },
        // Four notes ringing together and for longer: maximum intermodulation.
        { "open chord",   { 82.41, 123.47, 164.81, 207.65 }, 1.10, 1.4, 0.75 },
        // A sustained single-note line up the neck, lightly picked: the sparsest case.
        { "lead line",    { 293.66 },                 0.70, 1.1, 0.55 },
    };
    return all;
}

/** The same four playing styles an octave and a half down, for the bass target.

    **Not the guitar fixtures transposed, and the difference matters.** A bass is played
    differently, not merely lower: it is overwhelmingly monophonic where a guitar is not, its notes
    ring longer, and the one chord-shaped case it does have is an octave rather than a triad. Using
    the guitar set with the frequencies scaled would measure a bass amplifier playing guitar parts,
    which is a fixture nobody's recording resembles.

    The pitches are the real ones: 30.87 is the low B of a five-string, 41.20 the low E of a four,
    and the walking line sits where most bass parts actually live. The bottom two exist to put
    energy under 70 Hz, because that is the octave a guitar-shaped residual measurement throws away
    and the whole reason the band floor moves for this target.
*/
inline const std::vector<Phrase>& bassPhrases()
{
    static const std::vector<Phrase> all {
        // Low B, stopped short: the case that lives entirely below a guitar residual's floor.
        { "low b",        { 30.87 },                   0.32, 6.0, 0.95 },
        // Low E root, the staple, held long enough for the amplifier's sag to matter.
        { "root e",       { 41.20 },                   0.55, 2.2, 0.92 },
        // Root and octave together, which is the only "chord" most bass parts contain.
        { "octave",       { 55.00, 110.00 },           0.80, 1.6, 0.80 },
        // A walking line up where a bass is most audible and least fundamental-dominated.
        { "walking line", { 98.00 },                   0.42, 2.8, 0.62 },
    };
    return all;
}

/// The fixtures for one target. `guitar` is the default so existing callers are unchanged.
inline const std::vector<Phrase>& phrases(Fixture kind = Fixture::guitar)
{
    return kind == Fixture::bass ? bassPhrases() : guitarPhrases();
}

inline std::vector<float> renderPhrase(const Phrase& phrase, double sampleRate, double seconds)
{
    const auto count = static_cast<std::size_t>(sampleRate * seconds);
    std::vector<float> di(count);
    // Walked up and down so the fixture is not one pitch repeated, which would let a lucky
    // resonance stand in for a match.
    constexpr double steps[] = { 1.0, 1.1892, 1.3348, 1.1892 };
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto time = static_cast<double>(index) / sampleRate;
        const auto position = static_cast<std::size_t>(time / phrase.noteSeconds);
        const auto shift = steps[position % std::size(steps)];
        const auto within = std::fmod(time, phrase.noteSeconds);
        // Hard and soft alternating, so loudness range is not a constant.
        const auto stroke = (position % 2 == 0) ? 1.0 : 0.55;
        const auto envelope = std::exp(-within * phrase.decay) * (1.0 - std::exp(-within * 900.0));
        double value = 0.0;
        for (const auto note : phrase.notes)
            for (int partial = 1; partial <= 8; ++partial)
                value += (1.0 / (partial * partial))
                       * std::sin(2.0 * std::numbers::pi * note * shift * partial * time);
        value /= static_cast<double>(phrase.notes.size());
        di[index] = static_cast<float>(value * envelope * stroke * phrase.level * 0.30);
    }
    return di;
}
} // namespace nts::recovery
