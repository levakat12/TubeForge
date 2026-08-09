// The pedalboard that sits in front of the amplifier.
//
// The cases that matter here are the ones about *absence*: a slot set to None, a slot at zero
// mix and a bypassed slot all have to be exactly transparent, because the rig they leave
// behind -- amplifier and cabinet alone -- is the default and has to sound identical to a
// build with no pedalboard in it at all. "Almost transparent" would be a regression that no
// listening test would ever isolate.

#include "TestHarness.h"

#include <nts/pedals/PedalBoard.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>
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

/// Takes a `PedalKind` rather than a bare index so these read as the archetype they mean, which
/// is also the guard that the archetypes keep their model indices.
nts::pedals::PedalParameters engaged(nts::pedals::PedalKind kind, float drive = 8.0f,
                                     float mix = 100.0f)
{
    nts::pedals::PedalParameters parameters;
    parameters.model = nts::pedals::modelIndexOf(kind);
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

/** Every model's shaping stage is bounded.

    Separate from the four-slot finiteness test above, and it has to be: four slots at +24 dB of
    make-up each is 96 dB of legitimate gain, which swamps any threshold small enough to catch a
    curve that is actually diverging. So this runs *one* slot at unity level, where the only
    thing that can make the output large is the shape itself.

    Worth its own test because published pedal blueprints contain unbounded curves -- a cubic
    with a positive-leading tail runs away past roughly |x| > 2 instead of saturating, and it
    builds, loads and plays perfectly until someone turns the drive up.
*/
void testEveryModelIsBounded(TestHarness& harness)
{
    // Hot, but no hotter than a loud pickup: what is being tested is the curve, not headroom.
    const auto tone = makeTone(blockSize * 4, 82.4, 1.0f);
    for (std::size_t index = 1; index < nts::pedals::modelCount(); ++index)
    {
        nts::pedals::PedalBoard board;
        board.prepare(testSpec());
        nts::pedals::PedalParameters parameters;
        parameters.model = static_cast<int>(index);
        parameters.drive = 10.0f;
        parameters.tone = 10.0f;
        parameters.levelDb = 0.0f;
        parameters.mix = 100.0f;
        parameters.auxA = 10.0f;
        parameters.auxB = 10.0f;
        board.setParameters(0, parameters);

        auto peak = 0.0f;
        for (const auto sample : render(board, tone, 32)) peak = std::max(peak, std::abs(sample));
        // A bounded shaper cannot exceed its own ceiling however hard it is driven, and every
        // model's make-up trim is under 24 dB. Sixteen is far above any of them and far below
        // what a diverging polynomial reaches within one block.
        harness.expect(peak < 16.0f,
                       std::string(nts::pedals::pedalModel(static_cast<int>(index)).name)
                           + " saturates rather than diverging at full drive");
    }
}

/// The model with a given name, or -1. By name because indices move when the table grows.
int modelNamed(std::string_view name)
{
    for (std::size_t index = 0; index < nts::pedals::modelCount(); ++index)
        if (nts::pedals::pedalModel(static_cast<int>(index)).name == name)
            return static_cast<int>(index);
    return -1;
}

/** Modulation modulates, and a phaser is not a delay.

    A steady sine through a swept delay comes out with its amplitude moving, because the delayed
    copy drifts in and out of phase with itself. Measuring that the envelope varies is the
    difference between an engine that is running and one that is merely not crashing -- which is
    all a finiteness test proves.
*/
void testModulationSweeps(TestHarness& harness)
{
    const auto envelopeSpread = [](const std::vector<float>& signal)
    {
        // Peak per 512-sample window, then how far the loudest window is above the quietest.
        auto lowest = 1.0e9f;
        auto highest = 0.0f;
        for (std::size_t start = 0; start + 512 <= signal.size(); start += 512)
        {
            auto peak = 0.0f;
            for (std::size_t n = start; n < start + 512; ++n) peak = std::max(peak, std::abs(signal[n]));
            lowest = std::min(lowest, peak);
            highest = std::max(highest, peak);
        }
        return lowest <= 0.0f ? 0.0f : highest / lowest;
    };

    for (const auto name : { "Ensemble", "Jet Flanger", "Orange Phase" })
    {
        const auto model = modelNamed(name);
        harness.expect(model >= 0, std::string(name) + " is in the model table");
        if (model < 0) continue;

        nts::pedals::PedalBoard board;
        board.prepare(testSpec());
        nts::pedals::PedalParameters parameters;
        parameters.model = model;
        parameters.drive = 8.0f;    // rate, well up so a few cycles fit in the render
        parameters.tone = 9.0f;     // depth
        parameters.mix = 100.0f;
        parameters.auxA = 7.0f;     // feedback where the model has one
        board.setParameters(0, parameters);

        // 220 Hz so the comb notches land somewhere the tone can actually be cancelled.
        const auto output = render(board, makeTone(blockSize * 256, 220.0, 0.5f), 256);
        auto finite = true;
        for (const auto sample : output) finite = finite && std::isfinite(sample);
        harness.expect(finite, std::string(name) + " stays finite with feedback up");
        harness.expect(envelopeSpread(output) > 1.15f,
                       std::string(name) + " actually sweeps rather than sitting still");
    }
}

/** The crusher holds its output flat and then jumps, which is what sample-and-hold means.

    Two earlier attempts measured the wrong thing and are worth recording. Counting distinct
    output levels fails because the resonant filter after the reduction smooths the steps back
    into a continuum. Measuring inharmonic energy fails because an unwindowed transform of a
    strong sine leaks across every bin, so both signals are dominated by leakage from their own
    fundamental -- and the crushed one, being quieter, scored *lower*.

    What cannot be confounded is the shape of the first difference. A sine steps by a smooth
    amount every sample, so its largest step is only about pi/2 times its average one. A
    sample-and-hold signal barely moves for tens of samples and then jumps, so that ratio is
    several times larger. It survives the filter because the filter is near Nyquist and the
    jumps are what it passes.
*/
void testCrushQuantises(TestHarness& harness)
{
    const auto jumpiness = [](const std::vector<float>& signal)
    {
        auto largest = 0.0;
        auto total = 0.0;
        for (std::size_t n = 1; n < signal.size(); ++n)
        {
            const auto step = std::abs(static_cast<double>(signal[n]) - signal[n - 1]);
            largest = std::max(largest, step);
            total += step;
        }
        const auto mean = total / static_cast<double>(std::max<std::size_t>(1, signal.size() - 1));
        return mean <= 0.0 ? 0.0 : largest / mean;
    };

    const auto model = modelNamed("Bit Mapper");
    harness.expect(model >= 0, "Bit Mapper is in the model table");
    if (model < 0) return;

    // Long enough that `render` never wraps its source: a tone that does not fit a whole number
    // of cycles into the buffer clicks at every wrap, and a click is exactly the artefact this
    // measurement is looking for.
    const auto tone = makeTone(blockSize * 64, 620.0, 0.8f);

    nts::pedals::PedalBoard board;
    board.prepare(testSpec());
    nts::pedals::PedalParameters parameters;
    parameters.model = model;
    parameters.drive = 9.0f;     // heavy downsampling
    parameters.tone = 10.0f;     // filter wide open, so this measures the reduction not the filter
    parameters.mix = 100.0f;
    parameters.auxA = 10.0f;     // fewest bits
    board.setParameters(0, parameters);
    const auto crushed = render(board, tone, 64);

    nts::pedals::PedalBoard empty;
    empty.prepare(testSpec());
    const auto clean = render(empty, tone, 64);

    auto finite = true;
    for (const auto sample : crushed) finite = finite && std::isfinite(sample);
    harness.expect(finite, "the crusher stays finite at its most extreme setting");
    harness.expect(jumpiness(crushed) > jumpiness(clean) * 3.0,
                   "the crusher holds and jumps rather than moving smoothly like its input");
}
/** The pitch shifter shifts pitch, up when asked up and down when asked down.

    Measured by comparing energy at the shifted frequency against energy at the original. A
    finiteness test would pass on an engine that did nothing at all; this one cannot.
*/
void testPitchShifts(TestHarness& harness)
{
    constexpr double input = 440.0;

    const auto energyAt = [](const std::vector<float>& signal, double frequency)
    {
        double real {}, imaginary {};
        // The last half only: the first is the slot's blend ramping in from dry, which still
        // contains the unshifted tone and would be measured as a failure to shift.
        for (std::size_t n = signal.size() / 2; n < signal.size(); ++n)
        {
            const auto phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(n) / sampleRate;
            real += signal[n] * std::cos(phase);
            imaginary += signal[n] * std::sin(phase);
        }
        return real * real + imaginary * imaginary;
    };

    const auto model = modelNamed("Dive Bomb");
    harness.expect(model >= 0, "Dive Bomb is in the model table");
    if (model < 0) return;

    // Long enough that `render` never wraps -- see the crusher test for why that matters.
    const auto tone = makeTone(blockSize * 256, input, 0.6f);

    const auto shiftedEnergy = [&](float pitchControl, double atFrequency)
    {
        nts::pedals::PedalBoard board;
        board.prepare(testSpec());
        nts::pedals::PedalParameters parameters;
        parameters.model = model;
        parameters.drive = pitchControl;
        parameters.tone = 10.0f;
        parameters.mix = 100.0f;
        board.setParameters(0, parameters);
        return energyAt(render(board, tone, 256), atFrequency);
    };

    // The control spans -24 to +24 semitones, so 7.5 is +12 and 2.5 is -12.
    const auto up = shiftedEnergy(7.5f, input * 2.0);
    const auto upFundamental = shiftedEnergy(7.5f, input);
    harness.expect(up > upFundamental,
                   "shifting up puts more energy an octave above than at the original pitch");

    const auto down = shiftedEnergy(2.5f, input * 0.5);
    const auto downFundamental = shiftedEnergy(2.5f, input);
    harness.expect(down > downFundamental,
                   "shifting down puts more energy an octave below than at the original pitch");

    // And at the centre of the control it is a straight wire with a window on it.
    const auto unity = shiftedEnergy(5.0f, input);
    harness.expect(unity > 0.0, "no shift leaves the original pitch in place");
}

void testEveryKindStaysFiniteAtExtremes(TestHarness& harness)
{
    const auto tone = makeTone(blockSize * 4, 82.4, 0.9f);
    // Every model in the table, not only the seven archetypes: a modelled pedal added with an
    // unbounded shaping curve -- which at least one published blueprint has -- would otherwise
    // reach a user before it reached a test.
    for (std::size_t index = 1; index < nts::pedals::modelCount(); ++index)
    {
        nts::pedals::PedalBoard board;
        board.prepare(testSpec());
        nts::pedals::PedalParameters parameters;
        parameters.model = static_cast<int>(index);
        parameters.drive = 10.0f;
        parameters.tone = 10.0f;
        parameters.levelDb = 24.0f;
        parameters.mix = 100.0f;
        parameters.auxA = 10.0f;
        parameters.auxB = 10.0f;
        // Every slot on the same model, so this also covers four of them in series.
        for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
            board.setParameters(slot, parameters);

        const auto output = render(board, tone, 32);
        auto finite = true;
        for (const auto sample : output) finite = finite && std::isfinite(sample);
        const auto name = std::string(nts::pedals::pedalModel(static_cast<int>(index)).name);
        harness.expect(finite, name + " stays finite through four slots at full drive and level");
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
    testEveryModelIsBounded(harness);
    testModulationSweeps(harness);
    testCrushQuantises(harness);
    testPitchShifts(harness);
    return harness.result();
}
