// The measurements behind `nts_pedal_compare`. They are checked on signals whose answer is known
// by construction, because a comparison tool that is itself off by a few dB would turn every
// pedal-versus-capture number it prints into a mistake nobody could see.

#include "PedalMeasure.h"
#include "TestHarness.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;

/// A sum of sines, `(frequency, level in dB relative to a 0.5 amplitude)` each, one second long.
std::vector<float> makeTones(std::initializer_list<std::pair<double, double>> tones)
{
    std::vector<float> signal(static_cast<std::size_t>(sampleRate), 0.0f);
    for (const auto& [frequency, levelDb] : tones)
    {
        const auto amplitude = 0.5 * std::pow(10.0, levelDb / 20.0);
        for (std::size_t index = 0; index < signal.size(); ++index)
            signal[index] += static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency
                                                                     * static_cast<double>(index) / sampleRate));
    }
    return signal;
}

void testHarmonicsReadsAKnownWave(TestHarness& tests)
{
    const auto signal = makeTones({ { 220.0, 0.0 }, { 660.0, -20.0 } });
    const auto harmonics = pedalmeasure::harmonicsDb(signal, sampleRate, 220.0);
    tests.expectNear(harmonics[0], 0.0, 1e-9, "the fundamental is the reference");
    tests.expectNear(harmonics[2], -20.0, 0.5, "the third harmonic reads -20 dB");
}

void testThdOfPureSineIsSmall(TestHarness& tests)
{
    const auto signal = makeTones({ { 220.0, 0.0 } });
    tests.expect(pedalmeasure::thdPercent(signal, sampleRate, 220.0) < 0.1,
                 "a pure sine has under 0.1% THD");
}

void testThirdOctavesSeeATone(TestHarness& tests)
{
    const auto levels = pedalmeasure::thirdOctaveLevelsDb(makeTones({ { 1000.0, 0.0 } }), sampleRate);
    tests.expectEqual(levels.size(), std::size_t { 20 }, "there are twenty bands");
    // Band 10 is 1 kHz: 100 Hz is band 0 and each step is a third of an octave.
    auto top = std::size_t {};
    for (std::size_t band = 0; band < levels.size(); ++band)
        if (levels[band] > levels[top]) top = band;
    tests.expectEqual(top, std::size_t { 10 }, "the loudest band is the one at 1 kHz");
    tests.expect(levels[10] - levels[8] >= 20.0 && levels[10] - levels[12] >= 20.0,
                 "bands two away are at least 20 dB down");
}

void testAliasFloorSeesInharmonicTone(TestHarness& tests)
{
    const auto signal = makeTones({ { 220.0, 0.0 }, { 1000.0, -40.0 } });
    tests.expectNear(pedalmeasure::aliasFloorDb(signal, sampleRate, 220.0), -40.0, 1.0,
                     "an inharmonic tone at -40 dB sets the alias floor");
}

void testSpreadIsLargestBandRange(TestHarness& tests)
{
    const std::vector<std::vector<double>> levels { { 0.0, 1.0, 2.0 }, { 1.0, 5.0, 3.0 } };
    tests.expectNear(pedalmeasure::spreadDb(levels), 4.0, 1e-9, "the spread is the largest per-band range");
}
} // namespace

int main()
{
    TestHarness tests;
    testHarmonicsReadsAKnownWave(tests);
    testThdOfPureSineIsSmall(tests);
    testThirdOctavesSeeATone(tests);
    testAliasFloorSeesInharmonicTone(tests);
    testSpreadIsLargestBandRange(tests);
    return tests.result();
}
