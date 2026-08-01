#pragma once

#include "Common.h"

#include <cstddef>
#include <span>
#include <vector>

namespace nts::dsp
{
struct PitchReading
{
    /// Detected fundamental in Hz, or zero when nothing convincing was found.
    float frequencyHz {};
    /// Normalised autocorrelation at the chosen lag, 0 to 1. Below ~0.3 means "no note".
    float confidence {};
    /// True when the frame carried enough energy and periodicity to trust the reading.
    bool voiced {};
};

/** Monophonic fundamental estimator for a tuner.

    Normalised autocorrelation with a first-peak preference, which is what makes it read the
    fundamental of a guitar rather than an octave above it: a plucked string's correlation at
    twice the period is often marginally higher than at the true period, so the search takes
    the earliest peak within a tolerance of the best rather than the tallest one outright.

    The lag is then refined by parabolic interpolation across its neighbours. That step is not
    optional for this job -- at an 8 kHz analysis rate a low E sits near lag 97.5, so rounding
    to a whole lag is a nine-cent error, and a tuner that reads nine cents off is useless.

    All storage is claimed in prepare, so analyse allocates nothing. It is still meant for a
    worker or the message thread rather than the audio callback: the search is O(lags x frame).
*/
class PitchDetector
{
public:
    /** @param sampleRate       rate of the frames that will be passed to analyse
        @param maximumFrameSamples largest frame analyse will be given
        @param lowestFrequency  lowest fundamental to look for; 30 Hz covers a 5-string bass's
                                low B, 55 Hz is enough for guitar in standard tuning
        @param highestFrequency highest fundamental to look for
    */
    void prepare(double sampleRate, std::size_t maximumFrameSamples,
                 double lowestFrequency = 30.0, double highestFrequency = 1400.0);

    /** Estimates the fundamental of one frame. Allocation-free; the frame is not modified. */
    [[nodiscard]] PitchReading analyse(std::span<const float> frame) noexcept;

    /** Shortest frame that can resolve the configured lowest frequency.

        Two full periods of the lowest note, which is the minimum for the autocorrelation to
        have a peak to find at all.
    */
    [[nodiscard]] std::size_t recommendedFrameSamples() const noexcept;

private:
    double rate { 48000.0 };
    int minimumLag { 2 };
    int maximumLag { 2 };
    std::vector<float> work;
    std::vector<float> correlations;
};

/** Nearest equal-tempered note to a frequency, and how far off it is.

    @param frequencyHz  measured fundamental
    @param concertPitch reference for A4, so alternate tunings like A=432 are expressible
*/
struct NoteReading
{
    int midiNote { -1 };
    /// Signed distance from the nearest note, -50 to +50. Negative is flat.
    float cents {};
};

[[nodiscard]] NoteReading nearestNote(float frequencyHz, float concertPitch = 440.0f) noexcept;

/** Note name with octave, for example "E2". Returns "--" for an invalid note. */
[[nodiscard]] const char* noteName(int midiNote) noexcept;
} // namespace nts::dsp
