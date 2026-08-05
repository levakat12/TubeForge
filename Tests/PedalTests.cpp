// The pedalboard that sits in front of the amplifier.
//
// The cases that matter here are the ones about *absence*: a slot set to None, a slot at zero
// mix and a bypassed slot all have to be exactly transparent, because the rig they leave
// behind -- amplifier and cabinet alone -- is the default and has to sound identical to a
// build with no pedalboard in it at all. "Almost transparent" would be a regression that no
// listening test would ever isolate.

#include "TestHarness.h"

#include <nts/pedals/PedalBoard.h>

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr std::size_t blockSize = 64;
constexpr std::size_t channels = 2;

nts::dsp::ProcessSpec testSpec() { return { sampleRate, blockSize, channels }; }

/// A steady tone, well below any of the input high passes so nothing here is measuring a
/// filter roll-off by accident.
std::vector<float> makeTone(std::size_t samples, double frequencyHz = 220.0, float amplitude = 0.25f)
{
    std::vector<float> tone(samples);
    for (std::size_t index = 0; index < samples; ++index)
        tone[index] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequencyHz
                                                              * static_cast<double>(index) / sampleRate));
    return tone;
}

/// Runs `blocks` blocks of `source` through the board and returns what came out.
std::vector<float> render(nts::pedals::PedalBoard& board, const std::vector<float>& source,
                          std::size_t blocks)
{
    std::vector<float> output;
    output.reserve(blocks * blockSize);
    std::array<std::vector<float>, channels> work;
    std::array<float*, channels> pointers {};
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        work[channel].assign(blockSize, 0.0f);
        pointers[channel] = work[channel].data();
    }
    for (std::size_t block = 0; block < blocks; ++block)
    {
        for (std::size_t channel = 0; channel < channels; ++channel)
            for (std::size_t sample = 0; sample < blockSize; ++sample)
                work[channel][sample] = source[(block * blockSize + sample) % source.size()];
        board.process(pointers.data(), channels, blockSize);
        output.insert(output.end(), work[0].begin(), work[0].end());
    }
    return output;
}

/// The same signal the render above was fed, so the two can be compared sample for sample.
std::vector<float> expand(const std::vector<float>& source, std::size_t blocks)
{
    std::vector<float> expanded(blocks * blockSize);
    for (std::size_t index = 0; index < expanded.size(); ++index)
        expanded[index] = source[index % source.size()];
    return expanded;
}

float rootMeanSquare(const std::vector<float>& samples, std::size_t from = 0)
{
    double energy {};
    for (std::size_t index = from; index < samples.size(); ++index)
        energy += static_cast<double>(samples[index]) * samples[index];
    const auto count = samples.size() - from;
    return count == 0 ? 0.0f : static_cast<float>(std::sqrt(energy / static_cast<double>(count)));
}

nts::pedals::PedalParameters engaged(nts::pedals::PedalKind kind, float drive = 8.0f,
                                     float mix = 100.0f)
{
    nts::pedals::PedalParameters parameters;
    parameters.kind = kind;
    parameters.drive = drive;
    parameters.tone = 7.0f;
    parameters.levelDb = 0.0f;
    parameters.mix = mix;
    return parameters;
}

void testEmptyBoardIsExactlyTransparent(TestHarness& harness)
{
    nts::pedals::PedalBoard board;
    board.prepare(testSpec());
    const auto tone = makeTone(blockSize * 4);
    constexpr std::size_t blocks = 8;
    const auto output = render(board, tone, blocks);
    const auto expected = expand(tone, blocks);

    auto identical = true;
    for (std::size_t index = 0; index < output.size(); ++index)
        identical = identical && output[index] == expected[index];
    harness.expect(identical, "a board with every slot set to None is bit-exact passthrough");
    harness.expect(! board.anyActive(), "a board of None slots reports nothing active");
}

void testZeroMixIsExactlyTransparent(TestHarness& harness)
{
    nts::pedals::PedalBoard board;
    board.prepare(testSpec());
    // Deliberately the most violent setting available: at zero mix none of it may reach the
    // output, so the drive setting is the point rather than an incidental detail.
    board.setParameters(0, engaged(nts::pedals::PedalKind::fuzz, 10.0f, 0.0f));

    const auto tone = makeTone(blockSize * 4);
    constexpr std::size_t blocks = 8;
    const auto output = render(board, tone, blocks);
    const auto expected = expand(tone, blocks);

    auto identical = true;
    for (std::size_t index = 0; index < output.size(); ++index)
        identical = identical && output[index] == expected[index];
    harness.expect(identical, "a slot at zero mix is bit-exact passthrough whatever else it is set to");
    harness.expect(board.anyActive(), "a slot at zero mix still counts as switched in");
}

void testDriveRaisesLevel(TestHarness& harness)
{
    nts::pedals::PedalBoard board;
    board.prepare(testSpec());
    board.setParameters(0, engaged(nts::pedals::PedalKind::boost, 10.0f));

    const auto tone = makeTone(blockSize * 4);
    constexpr std::size_t blocks = 64;
    const auto output = render(board, tone, blocks);
    // Skipping the first half leaves out the blend ramp, so this measures the settled level.
    const auto driven = rootMeanSquare(output, output.size() / 2);
    const auto dry = rootMeanSquare(expand(tone, blocks));

    harness.expect(driven > dry * 2.0f, "a boost at full drive raises the level well above dry");
    auto finite = true;
    for (const auto sample : output) finite = finite && std::isfinite(sample);
    harness.expect(finite, "a boost at full drive produces finite output");
}

void testBypassReturnsToDryWithoutStepping(TestHarness& harness)
{
    nts::pedals::PedalBoard board;
    board.prepare(testSpec());
    board.setParameters(0, engaged(nts::pedals::PedalKind::distortion, 9.0f));

    const auto tone = makeTone(blockSize * 4);
    static_cast<void>(render(board, tone, 64));

    auto bypassed = engaged(nts::pedals::PedalKind::distortion, 9.0f);
    bypassed.bypassed = true;
    board.setParameters(0, bypassed);

    // One block is 1.3 ms and the blend takes 30, so the pedal must still be audible here.
    // This is the guard against a footswitch that steps: an implementation that switched
    // rather than faded would already be exactly dry.
    const auto firstBlock = render(board, tone, 1);
    const auto firstExpected = expand(tone, 1);
    auto changedImmediately = false;
    for (std::size_t index = 0; index < firstBlock.size(); ++index)
        changedImmediately = changedImmediately || firstBlock[index] != firstExpected[index];
    harness.expect(changedImmediately, "engaging bypass fades rather than switching");

    // And once the fade is over it has to be exactly dry, not nearly dry.
    static_cast<void>(render(board, tone, 64));
    constexpr std::size_t blocks = 8;
    const auto settled = render(board, tone, blocks);
    const auto expected = expand(tone, blocks);
    auto identical = true;
    for (std::size_t index = 0; index < settled.size(); ++index)
        identical = identical && settled[index] == expected[index];
    harness.expect(identical, "a bypassed slot settles to bit-exact passthrough");
    harness.expect(! board.anyActive(), "a bypassed slot reports nothing active");
}

void testSwitchingToNoneReturnsToDry(TestHarness& harness)
{
    nts::pedals::PedalBoard board;
    board.prepare(testSpec());
    board.setParameters(0, engaged(nts::pedals::PedalKind::overdrive, 9.0f));
    const auto tone = makeTone(blockSize * 4);
    static_cast<void>(render(board, tone, 64));

    board.setParameters(0, {});
    static_cast<void>(render(board, tone, 64));

    constexpr std::size_t blocks = 8;
    const auto settled = render(board, tone, blocks);
    const auto expected = expand(tone, blocks);
    auto identical = true;
    for (std::size_t index = 0; index < settled.size(); ++index)
        identical = identical && settled[index] == expected[index];
    harness.expect(identical, "a slot switched back to None returns to bit-exact passthrough");
}

void testNeuralSlotWithoutModelPassesAudio(TestHarness& harness)
{
    // At every drive setting, not just the middle one. The drive control is a pre-gain into the
    // model, and it used to be applied whether or not a model was there -- which was invisible
    // only for as long as the centre of this kind's travel happened to land on exactly 0 dB.
    // Widening the range so the knob can actually drive a capture turned the same code into a
    // 9 dB boost out of a slot with nothing loaded in it. Sweeping the control is what makes
    // this test about the invariant rather than about one lucky coordinate.
    for (const auto drive : { 0.0f, 2.5f, 5.0f, 7.5f, 10.0f })
    {
        nts::pedals::PedalBoard board;
        board.prepare(testSpec());
        auto parameters = engaged(nts::pedals::PedalKind::neuralCapture, drive);
        parameters.tone = 10.0f;
        board.setParameters(0, parameters);

        const auto tone = makeTone(blockSize * 4);
        constexpr std::size_t blocks = 64;
        const auto output = render(board, tone, blocks);
        const auto expected = expand(tone, blocks);

        // Levels rather than samples. The tone control is a real filter even at the top of its
        // travel, so it shifts the phase of a 220 Hz tone by about a degree; comparing sample for
        // sample would be measuring that rather than whether the signal survived.
        const auto passed = rootMeanSquare(output, output.size() / 2);
        const auto dry = rootMeanSquare(expected, expected.size() / 2);
        harness.expect(dry > 0.0f && std::abs(passed - dry) < dry * 0.01f,
                       "a neural slot with no capture loaded passes the signal at unity, drive "
                           + std::to_string(drive));
    }
}

void testEveryKindStaysFiniteAtExtremes(TestHarness& harness)
{
    const auto tone = makeTone(blockSize * 4, 82.4, 0.9f);
    for (std::size_t index = 1; index < nts::pedals::kindCount; ++index)
    {
        nts::pedals::PedalBoard board;
        board.prepare(testSpec());
        auto parameters = engaged(static_cast<nts::pedals::PedalKind>(index), 10.0f);
        parameters.tone = 10.0f;
        parameters.levelDb = 24.0f;
        // Every slot on the same kind, so this also covers four of them in series.
        for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
            board.setParameters(slot, parameters);

        const auto output = render(board, tone, 32);
        auto finite = true;
        for (const auto sample : output) finite = finite && std::isfinite(sample);
        harness.expect(finite, "kind " + std::to_string(index)
                                   + " stays finite through four slots at full drive and level");
    }
}
} // namespace

int main()
{
    TestHarness harness;
    testEmptyBoardIsExactlyTransparent(harness);
    testZeroMixIsExactlyTransparent(harness);
    testDriveRaisesLevel(harness);
    testBypassReturnsToDryWithoutStepping(harness);
    testSwitchingToNoneReturnsToDry(harness);
    testNeuralSlotWithoutModelPassesAudio(harness);
    testEveryKindStaysFiniteAtExtremes(harness);
    return harness.result();
}
