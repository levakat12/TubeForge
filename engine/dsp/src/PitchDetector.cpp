#include "nts/dsp/PitchDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace nts::dsp
{
namespace
{
constexpr float minimumConfidence = 0.30f;
/// How close a earlier peak has to be to the best one before it is preferred.
constexpr float firstPeakTolerance = 0.90f;
} // namespace

void PitchDetector::prepare(double sampleRate, std::size_t maximumFrameSamples,
                            double lowestFrequency, double highestFrequency)
{
    rate = std::max(1000.0, sampleRate);
    lowestFrequency = std::clamp(lowestFrequency, 10.0, rate * 0.25);
    highestFrequency = std::clamp(highestFrequency, lowestFrequency * 2.0, rate * 0.45);

    minimumLag = std::max(2, static_cast<int>(rate / highestFrequency));
    maximumLag = std::max(minimumLag + 1, static_cast<int>(rate / lowestFrequency) + 1);

    work.assign(maximumFrameSamples, 0.0f);
    correlations.assign(static_cast<std::size_t>(maximumLag) + 2, 0.0f);
}

std::size_t PitchDetector::recommendedFrameSamples() const noexcept
{
    return static_cast<std::size_t>(maximumLag) * 2;
}

PitchReading PitchDetector::analyse(std::span<const float> frame) noexcept
{
    PitchReading reading;
    const auto samples = std::min(frame.size(), work.size());
    // Needs two periods of the lowest note before the correlation has anything to find.
    if (samples < static_cast<std::size_t>(maximumLag) + 2) return reading;

    // Remove DC: a pickup's offset would otherwise dominate the correlation at every lag.
    double sum {};
    for (std::size_t index = 0; index < samples; ++index) sum += frame[index];
    const auto mean = static_cast<float>(sum / static_cast<double>(samples));

    double energy {};
    for (std::size_t index = 0; index < samples; ++index)
    {
        work[index] = frame[index] - mean;
        energy += static_cast<double>(work[index]) * work[index];
    }
    if (energy / static_cast<double>(samples) < 1.0e-8) return reading;

    const auto usableLag = std::min(maximumLag, static_cast<int>(samples / 2));
    if (usableLag <= minimumLag) return reading;

    auto bestLag = minimumLag;
    auto bestCorrelation = -1.0f;
    for (auto lag = minimumLag; lag <= usableLag; ++lag)
    {
        double cross {}, here {}, there {};
        for (auto index = static_cast<std::size_t>(lag); index < samples; ++index)
        {
            const auto a = work[index];
            const auto b = work[index - static_cast<std::size_t>(lag)];
            cross += static_cast<double>(a) * b;
            here += static_cast<double>(a) * a;
            there += static_cast<double>(b) * b;
        }
        const auto correlation = static_cast<float>(cross / std::sqrt(std::max(1.0e-18, here * there)));
        correlations[static_cast<std::size_t>(lag)] = correlation;
        if (correlation > bestCorrelation) { bestCorrelation = correlation; bestLag = lag; }
    }

    // Prefer the earliest peak that is nearly as strong as the best. Without this a plucked
    // string reads an octave low as often as not, because correlation at twice the period can
    // edge out the true one.
    for (auto lag = minimumLag + 1; lag < usableLag; ++lag)
    {
        const auto here = correlations[static_cast<std::size_t>(lag)];
        if (here >= bestCorrelation * firstPeakTolerance
            && here >= correlations[static_cast<std::size_t>(lag - 1)]
            && here >= correlations[static_cast<std::size_t>(lag + 1)])
        {
            bestLag = lag;
            bestCorrelation = here;
            break;
        }
    }

    if (bestCorrelation < minimumConfidence) return reading;

    // Parabolic interpolation through the peak and its neighbours. Whole-lag resolution is
    // far too coarse for a tuner; this recovers the fractional lag the samples imply.
    auto refinedLag = static_cast<double>(bestLag);
    if (bestLag > minimumLag && bestLag < usableLag)
    {
        const auto left = static_cast<double>(correlations[static_cast<std::size_t>(bestLag - 1)]);
        const auto centre = static_cast<double>(correlations[static_cast<std::size_t>(bestLag)]);
        const auto right = static_cast<double>(correlations[static_cast<std::size_t>(bestLag + 1)]);
        const auto denominator = 2.0 * (2.0 * centre - left - right);
        if (std::abs(denominator) > 1.0e-12)
        {
            const auto shift = std::clamp((right - left) / denominator, -0.5, 0.5);
            refinedLag += shift;
        }
    }

    reading.frequencyHz = static_cast<float>(rate / std::max(1.0e-9, refinedLag));
    reading.confidence = std::clamp(bestCorrelation, 0.0f, 1.0f);
    reading.voiced = true;
    return reading;
}

NoteReading nearestNote(float frequencyHz, float concertPitch) noexcept
{
    NoteReading reading;
    if (! (frequencyHz > 0.0f) || ! (concertPitch > 0.0f)) return reading;

    const auto midiFloat = 69.0f + 12.0f * std::log2(frequencyHz / concertPitch);
    if (! std::isfinite(midiFloat) || midiFloat < 0.0f || midiFloat > 127.0f) return reading;

    reading.midiNote = static_cast<int>(std::lround(midiFloat));
    reading.cents = (midiFloat - static_cast<float>(reading.midiNote)) * 100.0f;
    return reading;
}

const char* noteName(int midiNote) noexcept
{
    if (midiNote < 0 || midiNote > 127) return "--";
    // Indexed by pitch class then octave, so the caller gets a ready string with no formatting
    // and no allocation on a path the UI polls continuously.
    static constexpr std::array<const char*, 12> names {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    static constexpr std::array<const char*, 11> octaves {
        "-1", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
    };
    static std::array<std::array<char, 8>, 128> table {};
    static const bool built = []
    {
        for (int note = 0; note < 128; ++note)
        {
            const auto* name = names[static_cast<std::size_t>(note % 12)];
            const auto* octave = octaves[static_cast<std::size_t>(note / 12)];
            auto& cell = table[static_cast<std::size_t>(note)];
            std::size_t position {};
            for (const auto* character = name; *character != '\0'; ++character) cell[position++] = *character;
            for (const auto* character = octave; *character != '\0'; ++character) cell[position++] = *character;
            cell[position] = '\0';
        }
        return true;
    }();
    (void) built;
    return table[static_cast<std::size_t>(midiNote)].data();
}
} // namespace nts::dsp
