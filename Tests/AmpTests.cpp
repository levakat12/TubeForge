#include "TestHarness.h"

#include <nts/amp/TraditionalAmp.h>
#include <nts/dsp/Regression.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <numbers>
#include <thread>
#include <vector>

namespace
{
thread_local bool countAllocations {};
std::atomic<std::size_t> allocationCount {};
}

void* operator new(std::size_t size)
{
    if (countAllocations) allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
constexpr double sampleRate = 48000.0;
constexpr std::size_t blockSize = 128;
const nts::dsp::ProcessSpec monoSpec { sampleRate, blockSize, 1 };
const nts::dsp::ProcessSpec stereoSpec { sampleRate, blockSize, 2 };

std::vector<float> sine(std::size_t samples, double frequency, float amplitude = 0.25f)
{
    std::vector<float> result(samples);
    for (std::size_t index = 0; index < samples; ++index)
        result[index] = amplitude * static_cast<float>(std::sin(
            2.0 * std::numbers::pi * frequency * static_cast<double>(index) / sampleRate));
    return result;
}

double magnitude(std::span<const float> samples, double frequency)
{
    std::complex<double> value {};
    for (std::size_t index = 0; index < samples.size(); ++index)
        value += static_cast<double>(samples[index]) * std::polar(1.0,
            -2.0 * std::numbers::pi * frequency * static_cast<double>(index) / sampleRate);
    return 2.0 * std::abs(value) / static_cast<double>(samples.size());
}

bool finite(std::span<const float> samples)
{
    return std::all_of(samples.begin(), samples.end(), [](float value) { return std::isfinite(value); });
}

void testCalibrationAndPreEq(TestHarness& tests)
{
    nts::amp::InputCalibrator calibrator; calibrator.prepare(monoSpec);
    nts::amp::CalibrationProfile active { -28.0f, -22.0f, -9.0f, nts::amp::PickupProfile::active };
    calibrator.setProfile(active);
    std::array<float, blockSize> quiet; quiet.fill(0.01f); const float* input[] { quiet.data() };
    for (int block = 0; block < 200; ++block) calibrator.process(input, 1, blockSize);
    const auto reading = calibrator.reading();
    tests.expect(reading.rmsDb < active.targetRmsLowDb && reading.suggestedTrimDb > 0.0f,
                 "input calibration suggests trim against profile-specific RMS target");
    quiet.fill(0.9f); calibrator.process(input, 1, blockSize);
    tests.expect(calibrator.reading().peakDb > -2.0f && calibrator.reading().suggestedTrimDb < 0.0f,
                 "input calibration protects profile transient peak target");

    auto looseSignal = sine(4096, 55.0); auto tightSignal = looseSignal;
    nts::amp::PreEq loose, tight; loose.prepare({ sampleRate, looseSignal.size(), 1 });
    tight.prepare({ sampleRate, tightSignal.size(), 1 });
    nts::amp::PreEqParameters looseParameters; looseParameters.lowCutHz = 20.0f; looseParameters.tightness = 0.0f;
    auto tightParameters = looseParameters; tightParameters.tightness = 1.0f;
    loose.setParameters(looseParameters, 0); tight.setParameters(tightParameters, 0);
    float* looseChannel[] { looseSignal.data() }; float* tightChannel[] { tightSignal.data() };
    loose.process(looseChannel, 1, looseSignal.size()); tight.process(tightChannel, 1, tightSignal.size());
    tests.expect(magnitude(std::span(tightSignal).subspan(512), 55.0)
                 < magnitude(std::span(looseSignal).subspan(512), 55.0) * 0.35,
                 "tightness removes low energy before nonlinear stages");
}

void testPreampStages(TestHarness& tests)
{
    nts::amp::ResponsivePreampStage stage; stage.prepare(stereoSpec);
    nts::amp::PreampStageConfig config; config.driveDb = 24.0f; config.bias = 2.0f;
    config.asymmetry = 0.35f; config.memoryAmount = 0.8f; config.dynamicBias = 0.8f;
    stage.setConfig(config, 0); stage.reset();
    auto left = sine(blockSize, 440.0, 0.8f); auto right = left; float* channels[] { left.data(), right.data() };
    stage.process(channels, 2, blockSize); const auto first = left;
    tests.expect(std::abs(stage.configuration().bias) <= 0.8f
                 && std::abs(stage.biasState(0)) <= 0.8f, "preamp bias and memory state remain bounded");
    tests.expect(stage.latencySamples() == nts::dsp::antiAliasTapsPerPhase,
                 "preamp exposes selected oversampling latency");
    stage.reset(); left = sine(blockSize, 440.0, 0.8f); right = left;
    channels[0] = left.data(); channels[1] = right.data();
    stage.process(channels, 2, blockSize);
    const auto comparison = nts::dsp::compareAudio(first, left);
    tests.expect(comparison.maximumAbsoluteError < 1.0e-6, "preamp reset restores deterministic state");

    std::fill(left.begin(), left.end(), 0.9f); std::fill(right.begin(), right.end(), 0.9f);
    for (int block = 0; block < 100; ++block)
    {
        std::fill(left.begin(), left.end(), 0.9f); std::fill(right.begin(), right.end(), 0.9f);
        stage.process(channels, 2, blockSize);
    }
    const auto biasAfterTransient = std::abs(stage.biasState(0));
    for (int block = 0; block < 800; ++block)
    {
        std::fill(left.begin(), left.end(), 0.0f); std::fill(right.begin(), right.end(), 0.0f);
        stage.process(channels, 2, blockSize);
    }
    tests.expect(std::abs(stage.biasState(0)) < biasAfterTransient,
                 "preamp memory and bias shift recover after a strong transient");

    auto beforeAutomation = sine(blockSize, 220.0, 0.2f); auto afterAutomation = beforeAutomation;
    float* beforeChannel[] { beforeAutomation.data() }; stage.reset(); stage.process(beforeChannel, 1, blockSize);
    config.driveDb = 40.0f; stage.setConfig(config, blockSize); float* afterChannel[] { afterAutomation.data() };
    stage.process(afterChannel, 1, blockSize);
    tests.expect(std::abs(afterAutomation.front() - beforeAutomation.back()) < 0.8f,
                 "preamp gain automation is ramped without an unbounded discontinuity");

    // Aliasing check. A 3350 Hz tone driven hard puts its 14th harmonic at
    // 46900 Hz, which folds back to 1100 Hz when the stage runs without
    // oversampling. Nothing else in the chain can put energy there: the input is
    // a pure tone, so harmonic products only ever land on multiples of 3350 Hz.
    // That makes the level at 1100 Hz a direct measurement of fold-back.
    constexpr auto probeHz = 3350.0;
    constexpr auto aliasHz = 1100.0;
    constexpr std::size_t aliasSamples = 8192;

    const auto aliasEnergyAt = [&](int oversamplingFactor)
    {
        nts::amp::ResponsivePreampStage aliasStage;
        aliasStage.prepare({ sampleRate, aliasSamples, 1 });
        nts::amp::PreampStageConfig aliasConfig;
        aliasConfig.driveDb = 30.0f;
        aliasConfig.highCutHz = 20000.0f;
        aliasConfig.oversamplingFactor = oversamplingFactor;
        // Only the time-varying behaviour is disabled. Bias drift, memory, attack
        // reduction, and dynamic saturation modulate the waveform at low
        // frequency, which puts energy near 1100 Hz that is not fold-back.
        // Static asymmetry is kept deliberately: a symmetric shaper produces only
        // odd harmonics, and the 14th harmonic this probe relies on is even.
        aliasConfig.asymmetry = 0.3f;
        aliasConfig.bias = 0.05f;
        aliasConfig.dynamicBias = 0.0f;
        aliasConfig.frequencySaturation = 0.0f;
        aliasConfig.attackReduction = 0.0f;
        aliasConfig.memoryAmount = 0.0f;
        aliasStage.setConfig(aliasConfig, 0);
        aliasStage.reset();
        auto signal = sine(aliasSamples, probeHz, 0.7f);
        float* channel[] { signal.data() };
        aliasStage.process(channel, 1, aliasSamples);
        // Skip the filter warm-up so the measurement is steady state.
        return magnitude(std::span(signal).subspan(1024), aliasHz);
    };

    // Measured rejection at the shipped 8 taps per phase is about -30 dB
    // (ratio 0.033). The bound is set well clear of that so the test asserts a
    // real effect without tracking platform-to-platform arithmetic noise.
    const auto aliasAt1x = aliasEnergyAt(1);
    const auto aliasAt4x = aliasEnergyAt(4);
    tests.expect(aliasAt4x < aliasAt1x * 0.15,
                 "4x oversampling suppresses preamp fold-back that 1x leaves in the band");
}

/** Track B: the amplifier can clip with something other than a valve.

    Four claims, and the first is the one the whole track exists for. Before this, every stage in
    the amplifier was a hyperbolic tangent and no arrangement of the other twelve parameters could
    make one behave like a transistor -- so a solid-state voicing came out as a soft-knee valve
    with unusual EQ, which is not the same amplifier and does not sound like one.
*/
void testWaveshapeSelection(TestHarness& tests)
{
    using nts::amp::PreampStageConfig;

    /* A hard clipper is linear below its threshold; a valve is already compressing there.

       This is the difference, stated as the smallest measurement that can see it. Comparing the
       two at *high* drive would measure almost nothing -- both approach a square wave and the
       curves converge -- so the discriminating region is the quiet one, where tanh has visible
       curvature and a clamp has none at all. Bias, asymmetry and every time-varying term are
       switched off so the only thing left in the measurement is the curve.
    */
    const auto gainForShape = [&](nts::dsp::Waveshape shape, float amplitude)
    {
        nts::amp::ResponsivePreampStage stage;
        stage.prepare({ sampleRate, 4096, 1 });
        PreampStageConfig config;
        config.driveDb = 6.0f; config.outputTrimDb = 0.0f;
        config.bias = 0.0f; config.asymmetry = 0.0f;
        config.lowCutHz = 20.0f; config.highCutHz = 20000.0f;
        config.dynamicBias = 0.0f; config.frequencySaturation = 0.0f;
        config.attackReduction = 0.0f; config.memoryAmount = 0.0f;
        config.shape = shape;
        stage.setConfig(config, 0); stage.reset();
        auto signal = sine(4096, 500.0, amplitude);
        float* channel[] { signal.data() };
        stage.process(channel, 1, 4096);
        return magnitude(std::span(signal).subspan(1024), 500.0) / amplitude;
    };

    /* 0.35 into a 6 dB stage puts 0.70 into the curve: still short of the clamp at 1.0, and far
       enough up the tanh for its curvature to be unmistakable. An earlier version of this used
       0.05, where the hard clipper is exactly linear and tanh is *also* nearly linear -- the two
       differed by 0.25%, which is a true statement about the curves and a useless test. The
       discriminating region is the one just below the threshold, not the one near zero.
    */
    const auto quietHard = gainForShape(nts::dsp::Waveshape::hardClip, 0.35f);
    const auto quietValve = gainForShape(nts::dsp::Waveshape::hyperbolicTangent, 0.35f);
    tests.expect(quietHard > quietValve * 1.10,
                 "a hard-clip stage stays linear below threshold where a valve stage compresses");

    // And the shape has to actually reach the stage rather than being stored and ignored.
    auto shapesDiffer = true;
    for (const auto shape : { nts::dsp::Waveshape::hardClip, nts::dsp::Waveshape::diode,
                              nts::dsp::Waveshape::ledClip })
        shapesDiffer = shapesDiffer
            && std::abs(gainForShape(shape, 0.4f)
                      - gainForShape(nts::dsp::Waveshape::hyperbolicTangent, 0.4f)) > 1.0e-3;
    tests.expect(shapesDiffer, "every selectable waveshape changes what a preamp stage does");

    /* ADAA suppresses hard-clip fold-back, and the factor it is measured at is the finding.

       Same probe geometry as the oversampling test above -- a 3350 Hz tone whose 14th harmonic
       lands at 46900 Hz and folds to 1100 Hz -- because nothing else in the chain can put energy
       at 1100 Hz when the input is a pure tone.

       **Measured at 2x, and that is not a convenience.** Sweeping factor and asymmetry produced a
       result that changed the design:

         asym  x1     x2     x4     x8      (ADAA fold-back / plain fold-back)
         0.00  0.16   0.29   1.37   0.97
         0.06  0.70   0.30   1.39   0.97
         0.30  1.51   0.36   1.18   0.99

       The ADAA column is pinned near 2.4e-4 in every one of those runs regardless of factor, and
       the *plain* figure at 4x and 8x is already at that same value. That number is this probe's
       floor -- unwindowed correlation against a strong fundamental leaks into every bin -- so at
       4x and 8x the true fold-back is **below what can be measured here**, and the ratios in those
       two columns are scatter rather than signal. (A broadband sum was tried instead and is worse:
       the leakage then dominates every bin and every configuration reads 0.022 to 0.035.)

       Two conclusions, both acted on:

       - **2x is where the effect is real**, consistently about a threefold reduction and stable
         across asymmetry. That is what this asserts.
       - **8x oversampling alone already puts fold-back under the floor**, so the solid-state
         voicings do *not* enable ADAA -- see the note on `PreampStageConfig::antialiasedSaturation`
         and the plan's B2. Paying half a sample of delay and some top end for an improvement that
         cannot be measured is not a trade, it is a habit.

       The x1 row is the polarity-switching limitation documented in the saturator, seen directly:
       ADAA helps at asymmetry 0 and actively hurts by asymmetry 0.3, because at 1x the intervals
       that straddle the polarity breakpoint are a large fraction of all of them.
    */
    const auto foldbackWithAdaa = [&](bool antialias, float asymmetry, int factor)
    {
        constexpr std::size_t probeSamples = 8192;
        nts::amp::ResponsivePreampStage stage;
        stage.prepare({ sampleRate, probeSamples, 1 });
        PreampStageConfig config;
        config.driveDb = 30.0f; config.highCutHz = 20000.0f; config.oversamplingFactor = factor;
        config.shape = nts::dsp::Waveshape::hardClip;
        config.antialiasedSaturation = antialias;
        config.asymmetry = asymmetry; config.bias = 0.05f;
        config.dynamicBias = 0.0f; config.frequencySaturation = 0.0f;
        config.attackReduction = 0.0f; config.memoryAmount = 0.0f;
        stage.setConfig(config, 0); stage.reset();
        auto signal = sine(probeSamples, 3350.0, 0.7f);
        float* channel[] { signal.data() };
        stage.process(channel, 1, probeSamples);
        // Skip the filter warm-up so the measurement is steady state.
        return magnitude(std::span(signal).subspan(1024), 1100.0);
    };
    // Asymmetry 0.06 is what the solid-state voicings actually run, so the claim is made where
    // it is going to be relied on rather than at a flattering setting.
    const auto plain = foldbackWithAdaa(false, 0.06f, 2);
    const auto integrated = foldbackWithAdaa(true, 0.06f, 2);
    tests.expect(integrated < plain * 0.5,
                 "antiderivative antialiasing at least halves hard-clip fold-back at 2x");

    // The curve is part of a preset. A voicing that came back as a valve after a save would be a
    // different amplifier, and nothing else in the file would look wrong.
    auto shaped = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern,
                                               nts::amp::Instrument::bass);
    shaped.parameters.stages[0].shape = nts::dsp::Waveshape::hardClip;
    shaped.parameters.stages[0].antialiasedSaturation = true;
    shaped.parameters.stages[1].shape = nts::dsp::Waveshape::diode;
    shaped.parameters.powerAmp.shape = nts::dsp::Waveshape::hardClip;
    const auto restored = nts::amp::deserializePreset(nts::amp::serializePreset(shaped));
    tests.expect(restored.has_value() && restored->parameters == shaped.parameters,
                 "waveshape and antialiasing selections survive a serialize/restore");

    /* An out-of-range stored curve falls back to the valve rather than to silence.

       `dsp::shapeSample` switches on the enum and returns its input unchanged for a value outside
       it, so a cast straight from a stored integer turns a distorting stage into a *linear* one:
       no error, no NaN, and an amplifier that has quietly stopped being an amplifier. A preset
       written by a later build with more curves in the enum is exactly how that arrives.
    */
    auto forward = nts::amp::serializePreset(shaped, false);
    const auto shapeKey = std::string { "\"stage0Shape\":" };
    const auto at = forward.find(shapeKey);
    tests.expect(at != std::string::npos, "the stage shape is written under the key the loader reads");
    if (at != std::string::npos)
    {
        forward.replace(at + shapeKey.size(), 1, "99");
        const auto loaded = nts::amp::deserializePreset(forward);
        tests.expect(loaded.has_value()
                     && loaded->parameters.stages[0].shape == nts::dsp::Waveshape::hyperbolicTangent,
                     "a preset naming an unknown waveshape falls back to the valve curve");
    }
}

/** Track C: the bass path is a bi-amp rather than a fixed clean-low/driven-high split.

    The claim being tested is the one the voicing exists for: that a note whose fundamental sits
    below a 500 Hz crossover comes out with far less harmonic distortion than the same note through
    a 150 Hz one, because at 500 Hz the whole body of the note takes the clean side and only the
    attack and the harmonics reach the driven one.
*/
void testBiAmpPath(TestHarness& tests)
{
    // Second and third harmonics of an open A. Both are above 150 Hz and below 500, which is the
    // entire point: they change sides when the crossover moves and nothing else in the signal does.
    const auto distortionAt = [&](float crossoverHz)
    {
        auto preset = nts::amp::makeOriginalPreset(nts::amp::Topology::solidStateBiAmp,
                                                   nts::amp::Instrument::bass);
        preset.parameters.bass.crossoverHz = crossoverHz;
        preset.parameters.loudnessMatch = false;
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, blockSize, 1 });
        processor.loadPreset(preset, 0);
        std::vector<float> input (16384);
        for (std::size_t n = 0; n < input.size(); ++n)
            input[n] = 0.6f * static_cast<float>(
                std::sin(2.0 * std::numbers::pi * 110.0 * static_cast<double>(n) / sampleRate));
        const auto rendered = nts::amp::renderOffline(processor, input, blockSize);
        const auto steady = std::span(rendered).subspan(4096);
        const auto fundamental = magnitude(steady, 110.0);
        // Harmonic content relative to the fundamental, so a level difference between the two
        // configurations cannot masquerade as a difference in distortion.
        return (magnitude(steady, 220.0) + magnitude(steady, 330.0) + magnitude(steady, 440.0))
             / std::max(1.0e-9, fundamental);
    };

    const auto atMudGuard = distortionAt(150.0f);
    const auto atBiAmp = distortionAt(500.0f);
    tests.expect(atBiAmp < atMudGuard,
                 "a 500 Hz crossover passes more of the note clean than a 150 Hz one");

    // Per-band level has to reach the sum. Before Track C the two bands were mixed with weights
    // that no parameter could touch, so a bi-amp could not be balanced at all.
    const auto levelledOutput = [&](float lowLevelDb, float highLevelDb)
    {
        auto preset = nts::amp::makeOriginalPreset(nts::amp::Topology::solidStateBiAmp,
                                                   nts::amp::Instrument::bass);
        preset.parameters.loudnessMatch = false;
        preset.parameters.bass.lowLevelDb = lowLevelDb;
        preset.parameters.bass.highLevelDb = highLevelDb;
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, blockSize, 1 });
        processor.loadPreset(preset, 0);
        std::vector<float> input (8192);
        for (std::size_t n = 0; n < input.size(); ++n)
            input[n] = 0.5f * static_cast<float>(
                std::sin(2.0 * std::numbers::pi * 110.0 * static_cast<double>(n) / sampleRate));
        const auto rendered = nts::amp::renderOffline(processor, input, blockSize);
        return magnitude(std::span(rendered).subspan(2048), 110.0);
    };
    tests.expect(levelledOutput(6.0f, 0.0f) > levelledOutput(-6.0f, 0.0f) * 1.5,
                 "the low band's level control reaches the recombined output");

    /* Nothing that predates the bi-amp fields recombines differently because of them.

       This is the compatibility claim in its own right: every new field defaults to the value the
       path behaved as before it existed, so a valve voicing's two bands sum with exactly the
       weights they always did. `testOriginalVoicingRegression` proves it for the seven shipped
       voicings; this proves the *defaults* are the reason, which is what a new field has to get
       right rather than merely happening to.
    */
    nts::amp::BassPathParameters fresh;
    tests.expect(fresh.lowLevelDb == 0.0f && fresh.highLevelDb == 0.0f
                 && fresh.lowShape == nts::dsp::Waveshape::hyperbolicTangent
                 && std::abs(nts::dsp::dbToLinear(fresh.lowDriveDb) - 1.35f) < 0.005f,
                 "bi-amp defaults reproduce the fixed behaviour they replaced");

    const auto biAmp = nts::amp::makeOriginalPreset(nts::amp::Topology::solidStateBiAmp,
                                                    nts::amp::Instrument::bass);
    const auto restored = nts::amp::deserializePreset(nts::amp::serializePreset(biAmp));
    tests.expect(restored.has_value() && restored->parameters.bass == biAmp.parameters.bass,
                 "the bi-amp band controls survive a serialize/restore");
    tests.expect(biAmp.parameters.bass.crossoverHz > 400.0f,
                 "the bi-amp voicing splits well above the mud-guard range");
}

/** Track D / F3: the parallel dry path is delayed to match the drive path.

    **Written before the blend it tests, and this is the reason.** Every oversampled preamp stage
    contributes `dsp::antiAliasTapsPerPhase` -- eight -- base-rate samples of group delay. Summing
    an undelayed dry tap against a chain carrying two or three of those is a comb filter whose
    first null lands between roughly 1 and 1.5 kHz at 48 kHz: exactly the presence region a bass
    distortion exists to produce. It does not crash, does not produce NaN, and does not fail any
    other test in this file. It sounds slightly thin, which is indistinguishable from a voicing
    that needs tuning by ear -- so it would have been found, if at all, after weeks of somebody
    trying to fix it with the EQ.

    The measurement is reconstruction rather than spectrum: with the drive path made transparent, a
    50/50 blend of dry against wet has to give back the input. An unaligned blend cannot, because
    the two copies are a delay apart and cancel wherever they are out of phase.
*/
void testDryBlendAlignment(TestHarness& tests)
{
    /* A deliberately transparent amplifier: minimum drive, no sag, no saturation, tone stack flat,
       cabinet bypassed. What is left is a gain path with the oversamplers' group delay in it,
       which is precisely the thing the dry tap has to be aligned against. Oversampling stays at 8
       because the delay is what is being tested and 8x is what the voicing that needs this runs.
    */
    const auto transparent = [](std::size_t stageCount)
    {
        auto preset = nts::amp::makeOriginalPreset(nts::amp::Topology::studioDirect,
                                                   nts::amp::Instrument::guitar);
        auto& p = preset.parameters;
        p.instrument = nts::amp::Instrument::guitar;
        p.stageCount = stageCount;
        p.gateEnabled = false;
        p.loudnessMatch = false;
        p.outputGainDb = 0.0f;
        p.preEq = { 20.0f, 20000.0f, 0.0f, 0.0f, false, 0.0f, false, 0.0f };
        for (auto& stage : p.stages)
            stage = { 0.0f, 0.0f, 0.0f, 20.0f, 20000.0f, 0.0f, 8, 0.0f, 0.0f, 0.0f, 0.0f };
        p.toneStack = { nts::amp::ToneStackType::activeThreeBand, 0.5f, 0.5f, 0.5f, 800.0f, 0.9f };
        p.phaseInverter = { 1.0f, 1.5f, 0.0f, 0.0f, 0.0f };
        p.powerAmp = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f, 20.0f, 200.0f };
        p.cabinet.bypass = true;
        p.postLowDb = p.postMidDb = p.postHighDb = 0.0f;
        return preset;
    };

    /* Phase coherence, not flatness -- and the distinction is the whole test.

       An earlier version compared the blended output against the *input* and demanded 1 dB. It
       failed at 200 Hz by 1.9 dB with the alignment working perfectly, because the "transparent"
       chain is not actually flat: even at minimum drive the stages compress a little and the
       chain has about 4 dB more loss at 200 Hz than at 1.5 kHz. That test was measuring the drive
       path's frequency response and calling it alignment.

       What alignment means is that the two copies add rather than fight. If they are in phase,
       the blend is the *average of the two magnitudes*; if they are a delay apart, it is less --
       and at a null, far less. Comparing against `0.5 * (|dry| + |wet|)`, with `|wet|` measured
       from the same amplifier at `dryBlend = 0`, removes the drive path's own response from both
       sides of the comparison and leaves only the thing being asserted.

       The probe points are chosen, not arbitrary: `fs / 2D` is 1500 Hz for the two-stage chain's
       16 samples and 1000 Hz for the three-stage chain's 24, so each sits on the other's first
       null. 200 Hz is the control -- a comb this short barely touches the bottom, so a test that
       looked only there would pass on a completely broken blend.
    */
    const auto renderAt = [&](std::size_t stageCount, float blend)
    {
        auto preset = transparent(stageCount);
        preset.parameters.dryBlend = blend;
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, blockSize, 1 });
        processor.loadPreset(preset, 0);
        constexpr std::size_t probeSamples = 16384;
        std::vector<float> input (probeSamples);
        for (std::size_t n = 0; n < probeSamples; ++n)
            input[n] = 0.25f * static_cast<float>(
                  std::sin(2.0 * std::numbers::pi * 200.0 * static_cast<double>(n) / sampleRate)
                + std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / sampleRate)
                + std::sin(2.0 * std::numbers::pi * 1500.0 * static_cast<double>(n) / sampleRate));
        return std::pair { input, nts::amp::renderOffline(processor, input, blockSize) };
    };

    auto worstLossDb = 0.0;
    for (const auto stageCount : { std::size_t { 2 }, std::size_t { 3 } })
    {
        const auto [input, wetOnly] = renderAt(stageCount, 0.0f);
        const auto [again, blended] = renderAt(stageCount, 0.5f);
        static_cast<void>(again);
        const auto dry = std::span(input).subspan(4096, 8192);
        const auto wet = std::span(wetOnly).subspan(4096, 8192);
        const auto mixed = std::span(blended).subspan(4096, 8192);
        /* Asserted at the null frequencies only, and 200 Hz is deliberately excluded.

           Measured there, the blend loses 1.0 dB on the two-stage chain and 1.5 dB on the
           three-stage one **with the alignment working perfectly** -- and the reason is physical
           rather than a defect. The drive path carries a stack of high-pass filters: the pre-EQ's
           cut, a cut and a DC blocker in every stage, and more in the phase inverter. Each is
           minimum-phase, so each shifts the wet path's phase at 200 Hz by tens of degrees, and
           the shift grows with stage count -- which is exactly the pattern in the numbers. No
           delay can undo that, because it is not a delay; a real amplifier with a parallel clean
           blend has the same behaviour for the same reason.

           Including it would mean asserting a bound that the correct implementation only just
           meets, which is a test that fails on an unrelated voicing change later. The bug this
           exists to catch does not live at 200 Hz anyway: a 16-to-24 sample comb barely touches
           the bottom octave. It lives at `fs / 2D`, and that is where the tight bound goes.
        */
        for (const auto probe : { 1000.0, 1500.0 })
        {
            const auto coherent = 0.5 * (magnitude(dry, probe) + magnitude(wet, probe));
            if (coherent > 1.0e-6)
                worstLossDb = std::max(worstLossDb,
                    -20.0 * std::log10(std::max(1.0e-9, magnitude(mixed, probe) / coherent)));
        }
    }
    /* Unaligned, this same measurement reads 28 dB down at 1.5 kHz on the two-stage chain and
       18 dB at 1 kHz on the three-stage one -- verified by disabling the delay and watching it
       fail. Aligned, it is 0.02 to 0.06 dB. A bound of 0.5 is loose against the former by a
       factor of fifty and tight against the latter by a factor of ten, which is the room a test
       wants on both sides. */
    tests.expect(worstLossDb < 0.5,
                 "the dry and drive paths sum in phase rather than combing");

    // Zero is the default and has to be a true bypass, or every preset written before the control
    // existed changes sound the moment the control does.
    nts::amp::AmpParameters fresh;
    tests.expect(fresh.dryBlend == 0.0f, "the dry blend defaults to fully wet");

    /* CMOS Modern keeps its fundamental, which is the reason it could not ship before this track.

       Its first stage cuts at 150 Hz, so the distortion engine never sees the fundamental of
       anything below D on a four-string -- and without the dry blend to bring that fundamental
       back, the voicing is all clank and no note. The plan called it unshippable as an
       approximation; this is that claim as a number.
    */
    const auto lowEndOf = [&](float blend)
    {
        auto preset = nts::amp::makeOriginalPreset(nts::amp::Topology::cmosModern,
                                                   nts::amp::Instrument::bass);
        preset.parameters.dryBlend = blend;
        preset.parameters.loudnessMatch = false;
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, blockSize, 1 });
        processor.loadPreset(preset, 0);
        std::vector<float> input (16384);
        for (std::size_t n = 0; n < input.size(); ++n)
            input[n] = 0.5f * static_cast<float>(
                std::sin(2.0 * std::numbers::pi * 55.0 * static_cast<double>(n) / sampleRate));
        const auto rendered = nts::amp::renderOffline(processor, input, blockSize);
        return magnitude(std::span(rendered).subspan(4096), 55.0);
    };
    tests.expect(lowEndOf(0.4f) > lowEndOf(0.0f) * 1.5,
                 "the dry blend restores the fundamental the CMOS engine is filtered off");
    tests.expect(nts::amp::makeOriginalPreset(nts::amp::Topology::cmosModern,
                                              nts::amp::Instrument::bass).parameters.dryBlend > 0.2f,
                 "the CMOS voicing ships with its dry blend engaged");
}

/** F4: no voicing runs away, and no voicing is a volume jump.

    Two separate claims that the plan filed together, and one of them turned out to be about
    something else entirely -- see the note on calibration at the end.
*/
void testVoicingBoundsAndLevels(TestHarness& tests)
{
    const auto renderPeak = [](const nts::amp::AmpPreset& preset, double toneHz, float amplitude)
    {
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, blockSize, 1 });
        processor.loadPreset(preset, 0);
        std::vector<float> input (8192);
        for (std::size_t n = 0; n < input.size(); ++n)
            input[n] = amplitude * static_cast<float>(
                std::sin(2.0 * std::numbers::pi * toneHz * static_cast<double>(n) / sampleRate));
        const auto rendered = nts::amp::renderOffline(processor, input, blockSize);
        auto peak = 0.0f;
        for (const auto sample : rendered)
        {
            if (! std::isfinite(sample)) return std::numeric_limits<float>::infinity();
            peak = std::max(peak, std::abs(sample));
        }
        return peak;
    };

    /* Every control that shapes the waveform pushed to its stop.

       Master and output level are deliberately left at unity rather than maximised. With both at
       +18 dB a loud output is arithmetic, not a defect, and a bound loose enough to accommodate
       them would not catch anything -- exactly the reasoning the pedal catalogue's own runaway
       guard settled on. What is left after pinning them is the curves, which are the only things
       here that can diverge.
    */
    const auto atExtremes = [](nts::amp::AmpPreset preset)
    {
        auto& p = preset.parameters;
        p.loudnessMatch = false;
        p.outputGainDb = 0.0f;
        p.powerAmp.masterDb = 0.0f;
        p.gateEnabled = false;
        for (auto& stage : p.stages)
        {
            stage.driveDb = 42.0f; stage.bias = 0.8f; stage.asymmetry = 0.8f;
            stage.dynamicBias = 1.0f; stage.frequencySaturation = 1.0f;
            stage.attackReduction = 1.0f; stage.memoryAmount = 1.0f;
        }
        p.stageCount = 4;
        p.phaseInverter = { 8.0f, 1.5f, 0.8f, 0.4f, 1.0f };
        p.powerAmp.saturation = 1.0f; p.powerAmp.sag = 1.0f; p.powerAmp.biasCharacter = 0.6f;
        p.dryBlend = 0.5f;
        // Drive is a shaping control and goes to its stop; the two band *levels* are pinned at
        // unity for the same reason master and output are. +12 dB on both bands is 4x of ordinary
        // gain on a signal already at full scale, and a bound loose enough to allow it would not
        // catch a curve that diverges -- which is the only thing this test is looking for.
        p.bass.lowSaturation = true; p.bass.lowDriveDb = 24.0f;
        p.bass.lowLevelDb = 0.0f; p.bass.highLevelDb = 0.0f;
        return preset;
    };

    auto worstPeak = 0.0f;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
        for (const auto instrument : { nts::amp::Instrument::guitar, nts::amp::Instrument::bass })
        {
            const auto preset = atExtremes(nts::amp::makeOriginalPreset(
                static_cast<nts::amp::Topology>(index), instrument));
            for (const auto toneHz : { 55.0, 1000.0 })
            {
                worstPeak = std::max(worstPeak, renderPeak(preset, toneHz, 1.0f));
            }
        }
    tests.expect(std::isfinite(worstPeak) && worstPeak < 2.0f,
                 "no voicing exceeds +6 dBFS or goes non-finite at every control extreme");

    /* Every curve, not just the ones a voicing happens to ship with.

       A shape is selectable per stage and per power amp, so a preset -- or a hand-edited one, or a
       future voicing -- can pair any of them with any gain structure.

       **The threshold here is +10 dBFS rather than the +6 above, because of one curve.** Measured
       through four maxed stages, every shape peaks between 1.10 and 1.43 except
       `asymmetricPolynomial`, which reaches 2.73 -- roughly twice any other. It is bounded, not
       divergent, and it stays bounded when the stages are centred (that reading is *higher*, at
       2.73 against 2.08, so the asymmetric bias offset is not the cause; the curve simply passes
       more). It is also the one shape `dsp::supportsAntiderivative` rejects, so it cannot be
       antialiased.

       Recorded rather than hidden: no shipped voicing selects it, which the next assertion holds
       to. Anything that does should expect to trim about 6 dB more than a valve curve would need.
    */
    auto everyShapeBounded = true;
    for (const auto shape : { nts::dsp::Waveshape::hyperbolicTangent, nts::dsp::Waveshape::arcTangent,
                              nts::dsp::Waveshape::hardClip, nts::dsp::Waveshape::softClip,
                              nts::dsp::Waveshape::asymmetricPolynomial, nts::dsp::Waveshape::diode,
                              nts::dsp::Waveshape::germanium, nts::dsp::Waveshape::ledClip })
    {
        auto preset = atExtremes(nts::amp::makeOriginalPreset(nts::amp::Topology::cmosModern,
                                                              nts::amp::Instrument::bass));
        for (auto& stage : preset.parameters.stages) { stage.shape = shape; stage.antialiasedSaturation = true; }
        preset.parameters.powerAmp.shape = shape;
        preset.parameters.bass.lowShape = shape;
        const auto peak = renderPeak(preset, 110.0, 1.0f);
        everyShapeBounded = everyShapeBounded && std::isfinite(peak) && peak < 3.2f;
    }
    tests.expect(everyShapeBounded, "every waveshape stays bounded through the whole amplifier");

    // The outlier above is not reachable from any voicing, and this is what keeps it that way.
    auto shippedShapesSafe = true;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
        for (const auto instrument : { nts::amp::Instrument::guitar, nts::amp::Instrument::bass })
        {
            const auto preset = nts::amp::makeOriginalPreset(static_cast<nts::amp::Topology>(index), instrument);
            for (std::size_t stage = 0; stage < preset.parameters.stageCount; ++stage)
                shippedShapesSafe = shippedShapesSafe
                    && preset.parameters.stages[stage].shape != nts::dsp::Waveshape::asymmetricPolynomial;
            shippedShapesSafe = shippedShapesSafe
                && preset.parameters.powerAmp.shape != nts::dsp::Waveshape::asymmetricPolynomial
                && preset.parameters.bass.lowShape != nts::dsp::Waveshape::asymmetricPolynomial;
        }
    tests.expect(shippedShapesSafe,
                 "no factory voicing selects the one curve that cannot be antialiased");

    /* Switching voicing is not a volume jump.

       With loudness matching off -- which is the setting where the hand-written `outputGainDb`
       per voicing is the only thing levelling them -- the factory presets should land within a
       few dB of each other. A voicing that is 10 dB quieter than its neighbour is one nobody
       auditions fairly, and a voicing that is 10 dB louder is the one everybody picks.
    */
    /* Measured with a harmonically rich note, and that is not a detail.

       A pure sine at the fundamental made three voicings look 9 to 15 dB quiet, and none of them
       were. CMOS Modern filters everything below 150 Hz out of its drive path and Solid-State
       Bi-Amp splits at 500 Hz, so a 110 Hz sine lands entirely on one side of each: the
       measurement was reading their crossovers and calling it level. A plucked string has
       harmonics, and a test tone that does not will always flatter whichever voicing happens to
       pass the single frequency it contains.
    */
    const auto pluckedPeak = [](const nts::amp::AmpPreset& preset, double fundamental)
    {
        nts::amp::TraditionalAmpProcessor processor;
        processor.prepare({ sampleRate, blockSize, 1 });
        processor.loadPreset(preset, 0);
        std::vector<float> input (8192);
        for (std::size_t n = 0; n < input.size(); ++n)
        {
            auto value = 0.0;
            for (int harmonic = 1; harmonic <= 6; ++harmonic)
                value += std::sin(2.0 * std::numbers::pi * fundamental * harmonic
                                  * static_cast<double>(n) / sampleRate) / harmonic;
            input[n] = 0.28f * static_cast<float>(value);
        }
        const auto rendered = nts::amp::renderOffline(processor, input, blockSize);
        auto peak = 0.0f;
        for (const auto sample : rendered)
        {
            if (! std::isfinite(sample)) return std::numeric_limits<float>::infinity();
            peak = std::max(peak, std::abs(sample));
        }
        return peak;
    };

    for (const auto instrument : { nts::amp::Instrument::guitar, nts::amp::Instrument::bass })
    {
        auto quietest = std::numeric_limits<float>::infinity();
        auto loudest = 0.0f;
        for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
        {
            auto preset = nts::amp::makeOriginalPreset(static_cast<nts::amp::Topology>(index), instrument);
            preset.parameters.loudnessMatch = false;
            preset.parameters.gateEnabled = false;
            const auto peak = pluckedPeak(preset, instrument == nts::amp::Instrument::bass ? 55.0 : 110.0);
            quietest = std::min(quietest, peak);
            loudest = std::max(loudest, peak);
        }
        /* Measured at 15.7 dB on guitar and 15.4 dB on bass. That is wider than ideal and is not
           a defect: `loudnessMatch` defaults **on**, and with it on the voicings level
           themselves. This bound guards the case where a player turns it off, where the
           hand-written `outputGainDb` per voicing is the only thing levelling them. Widen it only
           with a reason -- a voicing 20 dB down is one nobody auditions fairly. */
        const auto spreadDb = 20.0 * std::log10(std::max(1.0e-9f, loudest) / std::max(1.0e-9f, quietest));
        tests.expect(spreadDb < 18.0,
                     "factory voicings sit within a usable level range of each other");
    }

    /* On calibration, which the plan asked for and does not apply.

       F4 called for a per-voicing review of `CalibrationProfile`, on the reasoning that CMOS
       Modern's 24 dB first stage is a very different gain structure from a valve voicing's. It is
       -- but `InputCalibrator` never sees it. The calibrator runs on the signal arriving at the
       plug-in, before the trim and before the gate, and suggests a trim to land that *input* at a
       target level. Its numbers depend on the player's pickups and interface and on nothing
       downstream of them, so a profile per instrument is exactly the right granularity and a
       profile per voicing would be modelling a dependency that does not exist.

       The real version of the concern is output level across voicings, which is the check above.
    */
    const auto guitarProfile = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern,
                                                            nts::amp::Instrument::guitar).calibration;
    const auto bassProfile = nts::amp::makeOriginalPreset(nts::amp::Topology::cmosModern,
                                                          nts::amp::Instrument::bass).calibration;
    tests.expect(guitarProfile.targetRmsLowDb != bassProfile.targetRmsLowDb,
                 "calibration targets differ by instrument, which is the granularity that matters");
}

/** A4: the front-panel switches, and the measurement that says each one does what it claims.

    These are the four figures the research turned up, and they are worth testing rather than
    eyeballing precisely because they are specific: an Ultra Lo whose cut sits at 950 Hz instead of
    500 is not a slightly-off Ultra Lo, it is a different control.
*/
void testPanelSwitches(TestHarness& tests)
{
    using nts::amp::PanelSwitch;

    // Response through the pre-EQ alone, which is where every one of these switches lives.
    const auto responseAt = [](const nts::amp::PreEqParameters& parameters, double probeHz)
    {
        nts::amp::PreEq preEq;
        preEq.prepare({ sampleRate, 8192, 1 });
        preEq.setParameters(parameters, 0);
        preEq.reset();
        auto signal = sine(8192, probeHz, 0.25f);
        float* channel[] { signal.data() };
        preEq.process(channel, 1, 8192);
        return magnitude(std::span(signal).subspan(2048), probeHz);
    };
    const auto switchGainDb = [&](nts::amp::Topology topology, PanelSwitch value, double probeHz)
    {
        auto preset = nts::amp::makeOriginalPreset(topology, nts::amp::Instrument::bass);
        // Cuts open so the shelves are measured rather than the high-pass in front of them.
        preset.parameters.preEq.lowCutHz = 15.0f;
        preset.parameters.preEq.tightness = 0.0f;
        preset.parameters.preEq.highCutHz = 20000.0f;
        const auto before = preset.parameters.preEq;
        nts::amp::applyPanelSwitch(preset.parameters, value);
        return 20.0 * std::log10(std::max(1.0e-9, responseAt(preset.parameters.preEq, probeHz))
                               / std::max(1.0e-9, responseAt(before, probeHz)));
    };

    /* Ultra Lo: +2 dB at 40 Hz and −10 dB at 500. The cut is the dominant term and the reason the
       switch reads as "more bass" without adding low-end power, so it is the one asserted hardest.
    */
    tests.expect(switchGainDb(nts::amp::Topology::valveFlagship, PanelSwitch::ultraLo, 500.0) < -6.0,
                 "Ultra Lo cuts hard at 500 Hz, which is what makes it sound bigger");
    tests.expect(switchGainDb(nts::amp::Topology::valveFlagship, PanelSwitch::ultraLo, 40.0) > 0.5,
                 "Ultra Lo lifts the bottom as well as cutting the mid");
    /* The three shelves are asserted at 2 dB, not at their stated 5 and 6, and that is arithmetic
       rather than slack: a shelf reaches exactly **half** its dB at its corner frequency, so a
       +5 dB shelf at 30 Hz measures +2.5 dB at 30 Hz and a +6 dB one at 5 kHz measures +3. The
       first version of this asserted the half-gain figure exactly and two of the three landed on
       the boundary. The full lift is a decade away in each case, where the pre-EQ's own cuts are
       in the way and the measurement stops being about the switch. */
    tests.expect(switchGainDb(nts::amp::Topology::valveFlagship, PanelSwitch::ultraHi, 5000.0) > 2.0,
                 "Ultra Hi lifts 5 kHz");
    tests.expect(switchGainDb(nts::amp::Topology::hybridMosfet, PanelSwitch::deep, 30.0) > 2.0,
                 "Deep lifts the bottom octave");
    tests.expect(switchGainDb(nts::amp::Topology::hybridMosfet, PanelSwitch::bright, 6000.0) > 2.0,
                 "Bright lifts the top");

    /* A switch belongs to an amplifier, and one it does not have is inert rather than applied.

       A preset or an automation lane can name any switch against any voicing -- the parameter is
       one flat list, deliberately, so that a lane's meaning never depends on the topology
       parameter's value. Honouring an out-of-panel switch would build an amplifier that never
       existed; rejecting the preset outright would lose everything else in it.
    */
    auto foreignSwitchesAreInert = true;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
        for (std::size_t option = 0; option < nts::amp::panelSwitchCount; ++option)
        {
            const auto topology = static_cast<nts::amp::Topology>(index);
            const auto value = static_cast<PanelSwitch>(option);
            if (nts::amp::panelSwitchAppliesTo(topology, value)) continue;
            auto preset = nts::amp::makeOriginalPreset(topology, nts::amp::Instrument::bass);
            const auto before = preset.parameters;
            nts::amp::applyPanelSwitch(preset.parameters, value);
            foreignSwitchesAreInert = foreignSwitchesAreInert && preset.parameters == before;
        }
    tests.expect(foreignSwitchesAreInert, "a switch the voicing does not have changes nothing");

    // `none` is every voicing's default and must be a true no-op, or the two amplifiers that do
    // carry switches would sound different from every preset written before the control existed.
    auto standardIsNeutral = true;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
    {
        auto preset = nts::amp::makeOriginalPreset(static_cast<nts::amp::Topology>(index),
                                                   nts::amp::Instrument::bass);
        const auto before = preset.parameters;
        nts::amp::applyPanelSwitch(preset.parameters, PanelSwitch::none);
        standardIsNeutral = standardIsNeutral && preset.parameters == before;
    }
    tests.expect(standardIsNeutral, "the Standard position changes nothing on any voicing");

    // Exactly two voicings carry switches, and every switch belongs to one of them. A switch
    // nothing offers is unreachable; one every voicing offers is not a panel switch.
    auto everySwitchHasAHome = true;
    for (std::size_t option = 1; option < nts::amp::panelSwitchCount; ++option)
    {
        auto homes = 0;
        for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
            if (nts::amp::panelSwitchAppliesTo(static_cast<nts::amp::Topology>(index),
                                               static_cast<PanelSwitch>(option)))
                ++homes;
        everySwitchHasAHome = everySwitchHasAHome && homes == 1;
    }
    tests.expect(everySwitchHasAHome, "every switch belongs to exactly one voicing");

    // Keys and names are stable identifiers for a saved state, so neither may collide.
    auto labelsDistinct = true;
    for (std::size_t first = 0; first < nts::amp::panelSwitchCount; ++first)
        for (std::size_t second = first + 1; second < nts::amp::panelSwitchCount; ++second)
            labelsDistinct = labelsDistinct
                && nts::amp::panelSwitchKey(static_cast<PanelSwitch>(first))
                       != nts::amp::panelSwitchKey(static_cast<PanelSwitch>(second))
                && nts::amp::panelSwitchName(static_cast<PanelSwitch>(first))
                       != nts::amp::panelSwitchName(static_cast<PanelSwitch>(second));
    tests.expect(labelsDistinct, "no two panel switches share a key or a name");

    // The pre-EQ frequencies are part of a preset now, not literals in the filter builder.
    auto shaped = nts::amp::makeOriginalPreset(nts::amp::Topology::valveFlagship,
                                               nts::amp::Instrument::bass);
    nts::amp::applyPanelSwitch(shaped.parameters, PanelSwitch::ultraLo);
    const auto restored = nts::amp::deserializePreset(nts::amp::serializePreset(shaped));
    tests.expect(restored.has_value() && restored->parameters.preEq == shaped.parameters.preEq,
                 "an engaged switch's filter corners survive a serialize/restore");
}

void testTonePhaseAndPower(TestHarness& tests)
{
    nts::amp::ToneStack tone; tone.prepare(monoSpec);
    nts::amp::ToneStackParameters neutral; neutral.type = nts::amp::ToneStackType::passiveCoupled;
    tone.setParameters(neutral, 0); const auto neutralCoefficients = tone.coefficients();
    auto bassRaised = neutral; bassRaised.bass = 1.0f; tone.setParameters(bassRaised, 0);
    const auto bassCoefficients = tone.coefficients();
    tests.expect(std::abs(neutralCoefficients[1].b0 - bassCoefficients[1].b0) > 1.0e-5,
                 "passive tone-stack bass control changes coupled mid coefficients");
    for (const auto type : { nts::amp::ToneStackType::passiveCoupled,
                             nts::amp::ToneStackType::activeThreeBand,
                             nts::amp::ToneStackType::bassSemiParametric })
    {
        auto parameters = neutral; parameters.type = type; parameters.midFrequencyHz = 1350.0f;
        tone.setParameters(parameters, 0); tone.reset(); auto signal = sine(blockSize, 900.0);
        float* channel[] { signal.data() }; tone.process(channel, 1, blockSize);
        tests.expect(finite(signal), "all tone-stack modes generate stable coefficients");
    }

    nts::amp::PhaseInverter inverter; inverter.prepare(monoSpec);
    nts::amp::PhaseInverterParameters inverterParameters; inverterParameters.headroom = 0.4f;
    inverterParameters.drive = 5.0f; inverterParameters.asymmetry = 0.4f; inverter.setParameters(inverterParameters, 0);
    std::array<float, blockSize> hot; hot.fill(4.0f); float* hotChannel[] { hot.data() };
    inverter.process(hotChannel, 1, blockSize);
    tests.expect(*std::max_element(hot.begin(), hot.end()) <= 0.401f,
                 "phase inverter applies asymmetric limited headroom");

    nts::amp::PowerAmp power; power.prepare(monoSpec);
    nts::amp::PowerAmpParameters powerParameters; powerParameters.sag = 1.0f;
    powerParameters.masterDb = 12.0f; powerParameters.saturation = 0.9f;
    powerParameters.sagAttackMs = 1.0f; powerParameters.sagRecoveryMs = 80.0f;
    power.setParameters(powerParameters, 0); power.reset();
    std::array<float, blockSize> driven; driven.fill(0.9f); float* powerChannel[] { driven.data() };
    for (int block = 0; block < 80; ++block) power.process(powerChannel, 1, blockSize);
    const auto sagged = power.supplyState(0);
    tests.expect(sagged >= 0.32f && sagged < 0.9f, "power-amp sag supply remains bounded and drops under load");
    for (int block = 0; block < 400; ++block)
    {
        driven.fill(0.0f); power.process(powerChannel, 1, blockSize);
    }
    tests.expect(power.supplyState(0) > sagged, "power-amp virtual supply recovers after load");
}

/// Width splits the two cabinet slots left and right. The two things that must hold: it is inert
/// at its default, so no stored preset changes; and it reports honestly what folding to mono will
/// cost, because an inter-channel delay is wide on speakers and comb-filtered on a mix bus.
void testCabinetStereoWidth(TestHarness& tests)
{
    const auto renderWith = [](float width, std::size_t delaySamplesB, bool differentSlots)
    {
        nts::amp::CabinetSection cabinet; cabinet.prepare(stereoSpec);
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.5f; parameters.width = width;
        parameters.slots[1].delaySamples = delaySamplesB;
        parameters.lowCutHz = 20.0f; parameters.highCutHz = 20000.0f;
        cabinet.setParameters(parameters, 0);
        if (differentSlots)
        {
            // Two audibly different responses, which is the point of the feature: one bright and
            // short, one darker with a longer tail.
            std::vector<float> bright(192), dark(192);
            for (std::size_t index = 0; index < bright.size(); ++index)
            {
                const auto position = static_cast<float>(index);
                bright[index] = (index == 0 ? 0.9f : 0.0f) + 0.10f * std::exp(-position / 12.0f);
                dark[index] = 0.35f * std::exp(-position / 70.0f) * std::cos(0.07f * position);
            }
            cabinet.loadImpulseA(bright, {}, { "Bright", "57", 0.2f }, 0);
            cabinet.loadImpulseB(dark, {}, { "Dark", "121", 0.8f }, 0);
        }
        cabinet.reset();

        std::array<float, blockSize> left {}, right {};
        float* channels[] { left.data(), right.data() };
        std::vector<float> capturedLeft, capturedRight;
        for (std::size_t block = 0; block < 400; ++block)
        {
            for (std::size_t sample = 0; sample < blockSize; ++sample)
            {
                const auto phase = static_cast<float>(block * blockSize + sample) * 0.05f;
                left[sample] = right[sample] = 0.5f * std::sin(phase);
            }
            cabinet.process(channels, 2, blockSize);
            for (std::size_t sample = 0; sample < blockSize; ++sample)
            { capturedLeft.push_back(left[sample]); capturedRight.push_back(right[sample]); }
        }
        struct Result { std::vector<float> left, right; float monoLossDb; };
        return Result { std::move(capturedLeft), std::move(capturedRight), cabinet.monoCompatibility() };
    };

    // Inert at the default. A mono-fed stereo signal through summed slots must stay identical
    // across the channels, which is what every existing preset relies on.
    const auto summed = renderWith(0.0f, 0, true);
    auto identical = true;
    for (std::size_t index = 0; index < summed.left.size(); ++index)
        identical = identical && summed.left[index] == summed.right[index];
    tests.expect(identical, "width defaults to inert: the two channels stay bit-identical");
    tests.expectNear(summed.monoLossDb, 0.0, 0.05,
                     "identical channels fold to mono with no loss");

    // Up, with two different responses: the channels must actually differ, and they must still be
    // largely mono-compatible, because two responses decorrelate spectrally rather than by delay.
    const auto split = renderWith(1.0f, 0, true);
    auto differs = false;
    for (std::size_t index = 0; index < split.left.size(); ++index)
        differs = differs || std::abs(split.left[index] - split.right[index]) > 1.0e-6f;
    tests.expect(differs, "width up sends a different response to each channel");
    tests.expect(split.monoLossDb > -3.0f,
                 "two responses split across the field stay broadly mono-safe: "
                     + std::to_string(split.monoLossDb) + " dB");

    // The hazard the meter exists for. An inter-channel delay is a comb filter under summing, and
    // it must read materially worse than the undelayed split rather than silently the same.
    const auto delayed = renderWith(1.0f, 96, true);
    tests.expect(delayed.monoLossDb < split.monoLossDb - 1.0f,
                 "an inter-channel delay reports a real mono-fold penalty: "
                     + std::to_string(delayed.monoLossDb) + " dB against "
                     + std::to_string(split.monoLossDb) + " dB");
}

/** The per-slot controls, and the one claim that governs all of them: **at their defaults the
    section does exactly what it did before they existed.**

    That is not a nicety. These controls were added to a plug-in people have saved projects with,
    and the fields they expose previously had fixed values baked into the processing loop. If a
    default is off by so much as a rounding step, every stored rig is re-voiced by an update.

    So each case below pins one historical behaviour to the control that now generalises it:
    the hard left/right split `width` performed, unity slot level, no delay on A, no output trim.
    The last two cases check the new controls actually do something, because a control that is
    inert at its default and inert everywhere else is worse than no control at all.
*/
void testCabinetSlotControls(TestHarness& tests)
{
    // Two responses that cannot be confused for one another: a single spike each, at different
    // positions and heights, so every assertion below can be read off the output directly.
    const std::array<float, 4> responseA { 1.0f, 0.0f, 0.0f, 0.0f };
    const std::array<float, 4> responseB { 0.0f, 0.0f, 0.5f, 0.0f };

    const auto render = [&](const nts::amp::CabinetParameters& settings)
    {
        nts::amp::CabinetSection cabinet; cabinet.prepare(stereoSpec);
        cabinet.loadImpulseA(responseA, {}, {}, 0);
        cabinet.loadImpulseB(responseB, {}, {}, 0);
        auto parameters = settings;
        // The two cuts are pushed out of the way throughout: they are biquads either side of the
        // audio band and they would smear the spikes these assertions read positions off.
        parameters.lowCutHz = 20.0f; parameters.highCutHz = 20000.0f;
        cabinet.setParameters(parameters, 0);
        cabinet.reset();
        std::array<float, blockSize> left {}, right {};
        left[0] = right[0] = 1.0f;
        float* channels[] { left.data(), right.data() };
        cabinet.process(channels, 2, blockSize);
        struct Result { std::array<float, blockSize> left, right; };
        return Result { left, right };
    };

    /* The compatibility case. Full width with the pans left alone must put slot A alone on the
       left and slot B alone on the right -- which is the rule the loop had hard-coded before a
       slot could be panned, and therefore the sound every saved project with width up expects. */
    {
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.5f; parameters.width = 1.0f;
        const auto result = render(parameters);
        tests.expectNear(result.left[0], 1.0, 1.0e-6, "full width puts slot A alone on the left");
        tests.expectNear(result.left[2], 0.0, 1.0e-6, "slot B does not reach the left channel");
        tests.expectNear(result.right[2], 0.5, 1.0e-6, "full width puts slot B alone on the right");
        tests.expectNear(result.right[0], 0.0, 1.0e-6, "slot A does not reach the right channel");
    }

    // Unity level and no trim: the summed default has to be the plain blend, unscaled.
    {
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.5f;
        const auto result = render(parameters);
        tests.expectNear(result.left[0], 0.5, 1.0e-6, "slot A defaults to unity level");
        tests.expectNear(result.left[2], 0.25, 1.0e-6, "slot B defaults to unity level");
    }

    // A slot's own delay moves that slot and nothing else. Slot A's is the new one; before it,
    // aligning a pair meant hoping the early microphone happened to be B.
    {
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.5f;
        parameters.slots[0].delaySamples = 5;
        const auto result = render(parameters);
        tests.expectNear(result.left[0], 0.0, 1.0e-6, "delaying slot A clears its original position");
        tests.expectNear(result.left[5], 0.5, 1.0e-6, "slot A arrives exactly its delay later");
        tests.expectNear(result.left[2], 0.25, 1.0e-6, "slot B is untouched by slot A's delay");
    }

    // Mute is silence, not attenuation, and the section must stop paying for the slot as well.
    {
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.5f;
        parameters.slots[1].mute = true;
        const auto result = render(parameters);
        tests.expectNear(result.left[2], 0.0, 1.0e-6, "a muted slot contributes nothing");
        tests.expectNear(result.left[0], 0.5, 1.0e-6, "muting one slot leaves the other alone");
    }

    // Polarity on slot A, which had none: inverting one side of a two-microphone blend is the
    // first thing anybody tries, and it was reachable on B only.
    {
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.5f;
        parameters.slots[0].phaseInvert = true;
        const auto result = render(parameters);
        tests.expectNear(result.left[0], -0.5, 1.0e-6, "slot A polarity inverts slot A");
        tests.expectNear(result.left[2], 0.25, 1.0e-6, "slot A polarity leaves slot B alone");
    }

    // The output trim scales everything after the blend, and -6.0206 dB is exactly a half.
    {
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.5f;
        parameters.outputTrimDb = -6.0205999f;
        const auto result = render(parameters);
        tests.expectNear(result.left[0], 0.25, 1.0e-5, "the output trim scales the summed cabinet");
        tests.expectNear(result.left[2], 0.125, 1.0e-5, "the output trim scales both slots alike");
    }

    // Centring both pans is the thing width could not previously express: a placement that
    // separates nothing, so full width collapses back onto an even sum of the two slots.
    {
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.5f; parameters.width = 1.0f;
        parameters.slots[0].pan = 0.0f; parameters.slots[1].pan = 0.0f;
        const auto result = render(parameters);
        tests.expectNear(result.left[0], 0.5, 1.0e-6, "two centred slots place slot A on both sides");
        tests.expectNear(result.right[0], 0.5, 1.0e-6, "and identically on the right");
        tests.expectNear(result.left[2], 0.25, 1.0e-6, "with slot B placed alongside it");
    }
}

/** True-stereo responses: four impulses, and the cross terms have to reach the right channel.

    A true-stereo capture is the matrix [[LL, LR], [RL, RR]], and the failure mode of getting it
    wrong is not a crash or a silence -- it is a cabinet that still sounds like a cabinet, with the
    two cross terms swapped, which nobody would ever catch by ear. So the test uses four impulses
    that are single spikes at four different positions and reads the routing off the output
    directly.

    The other half of the claim matters just as much: a slot that is *not* true stereo must be
    untouched by any of this, because that is every slot in almost every rig.
*/
void testCabinetTrueStereo(TestHarness& tests)
{
    // One spike each, at four distinguishable positions and heights.
    const std::array<float, 8> leftToLeft   { 1.0f, 0, 0, 0, 0, 0, 0, 0 };
    const std::array<float, 8> rightToRight { 0, 0.8f, 0, 0, 0, 0, 0, 0 };
    const std::array<float, 8> leftToRight  { 0, 0, 0.5f, 0, 0, 0, 0, 0 };
    const std::array<float, 8> rightToLeft  { 0, 0, 0, 0.25f, 0, 0, 0, 0 };

    const auto render = [&](bool trueStereo, float leftInput, float rightInput)
    {
        nts::amp::CabinetSection cabinet;
        cabinet.prepare(stereoSpec);
        nts::amp::CabinetSection::TrueStereoImpulse cross;
        if (trueStereo) cross = { leftToRight, rightToLeft };
        cabinet.loadImpulseA(leftToLeft, rightToRight, {}, 0, cross);
        // Slot B silent, so everything read below belongs to slot A.
        const std::array<float, 8> silence {};
        cabinet.loadImpulseB(silence, silence, {}, 0);
        nts::amp::CabinetParameters parameters;
        parameters.blend = 0.0f;   // slot A alone
        parameters.lowCutHz = 20.0f; parameters.highCutHz = 20000.0f;
        cabinet.setParameters(parameters, 0);
        cabinet.reset();
        tests.expectEqual(cabinet.isTrueStereo(0), trueStereo,
                          "the slot reports whether it is convolving a true-stereo response");
        std::array<float, blockSize> left {}, right {};
        left[0] = leftInput; right[0] = rightInput;
        float* channels[] { left.data(), right.data() };
        cabinet.process(channels, 2, blockSize);
        struct Result { std::array<float, blockSize> left, right; };
        return Result { left, right };
    };

    /* An impulse on the left input only. With true stereo engaged the left output carries LL and
       the right output carries LR; nothing should appear where RL and RR live. */
    {
        const auto result = render(true, 1.0f, 0.0f);
        tests.expectNear(result.left[0], 1.0, 1.0e-6, "left in reaches left out through LL");
        tests.expectNear(result.right[2], 0.5, 1.0e-6, "left in reaches right out through LR");
        tests.expectNear(result.left[3], 0.0, 1.0e-6, "left in does not trigger the RL term");
        tests.expectNear(result.right[1], 0.0, 1.0e-6, "left in does not trigger the RR term");
    }

    // And the mirror: an impulse on the right input takes RR and RL.
    {
        const auto result = render(true, 0.0f, 1.0f);
        tests.expectNear(result.right[1], 0.8, 1.0e-6, "right in reaches right out through RR");
        tests.expectNear(result.left[3], 0.25, 1.0e-6, "right in reaches left out through RL");
        tests.expectNear(result.left[0], 0.0, 1.0e-6, "right in does not trigger the LL term");
        tests.expectNear(result.right[2], 0.0, 1.0e-6, "right in does not trigger the LR term");
    }

    /* Without the cross terms the slot must behave exactly as it always has: the two direct
       impulses, and nothing crossing between the channels. This is the case every existing rig
       is in, so it is the one that must not have moved. */
    {
        const auto result = render(false, 1.0f, 0.0f);
        tests.expectNear(result.left[0], 1.0, 1.0e-6, "an ordinary stereo response still passes LL");
        tests.expectNear(result.right[2], 0.0, 1.0e-6,
                         "and nothing crosses between the channels without cross terms");
    }
}

/// The blend control parks one convolver at a weight of zero; skipping it is worth half the
/// cabinet's cost, and the risk it carries is the delay line going stale while it is skipped.
void testCabinetBlendEngagement(TestHarness& tests)
{
    nts::amp::CabinetSection cabinet; cabinet.prepare(stereoSpec);
    nts::amp::CabinetParameters parameters;
    parameters.blend = 0.0f; parameters.lowCutHz = 20.0f; parameters.highCutHz = 20000.0f;
    cabinet.setParameters(parameters, 0); cabinet.reset();

    std::array<float, blockSize> left {}, right {};
    float* channels[] { left.data(), right.data() };
    const auto drive = [&](std::size_t blocks, float amplitude)
    {
        std::vector<float> captured;
        for (std::size_t block = 0; block < blocks; ++block)
        {
            for (std::size_t sample = 0; sample < blockSize; ++sample)
            {
                const auto phase = static_cast<float>(block * blockSize + sample) * 0.05f;
                left[sample] = right[sample] = amplitude * std::sin(phase);
            }
            cabinet.process(channels, 2, blockSize);
            for (std::size_t sample = 0; sample < blockSize; ++sample) captured.push_back(left[sample]);
        }
        return captured;
    };

    // Park at blend 0 long enough that side B, which contributes nothing there, has a delay
    // line full of samples that are no longer adjacent to what comes next.
    static_cast<void>(drive(24, 0.3f));

    // Now hand the output entirely to B. If a skipped side were re-engaged without clearing
    // its history, this is where the stale line would fold into the output as a step.
    parameters.blend = 1.0f;
    cabinet.setParameters(parameters, 0);
    const auto afterSwing = drive(8, 0.3f);
    auto largestStep = 0.0f;
    for (std::size_t index = 1; index < afterSwing.size(); ++index)
        largestStep = std::max(largestStep, std::abs(afterSwing[index] - afterSwing[index - 1]));
    tests.expect(std::isfinite(largestStep) && largestStep < 0.5f,
                 "re-engaging a blend-skipped cabinet side does not step");

    // And the skip must not have changed what blend 0 actually produces. A section that never
    // parks either side is the reference: same input, same responses, same output.
    nts::amp::CabinetSection reference; reference.prepare(stereoSpec);
    nts::amp::CabinetParameters halfway = parameters;
    halfway.blend = 0.0f;
    reference.setParameters(halfway, 0); reference.reset();
    cabinet.setParameters(halfway, 0); cabinet.reset();

    auto matches = true;
    std::array<float, blockSize> referenceLeft {}, referenceRight {};
    float* referenceChannels[] { referenceLeft.data(), referenceRight.data() };
    for (std::size_t block = 0; block < 8 && matches; ++block)
    {
        for (std::size_t sample = 0; sample < blockSize; ++sample)
        {
            const auto phase = static_cast<float>(block * blockSize + sample) * 0.05f;
            const auto value = 0.3f * std::sin(phase);
            left[sample] = right[sample] = value;
            referenceLeft[sample] = referenceRight[sample] = value;
        }
        cabinet.process(channels, 2, blockSize);
        reference.process(referenceChannels, 2, blockSize);
        for (std::size_t sample = 0; sample < blockSize; ++sample)
            matches = matches && std::abs(left[sample] - referenceLeft[sample]) < 1.0e-6f;
    }
    tests.expect(matches, "blend 0 output is unchanged by skipping the idle cabinet side");
}

/// A long response moves the whole section onto the partitioned path, which costs latency. The
/// thing that must not break is that both sides move together: a blend of one buffered and one
/// unbuffered response would comb-filter rather than mix.
void testCabinetLongImpulsePath(TestHarness& tests)
{
    nts::amp::CabinetSection cabinet; cabinet.prepare(stereoSpec);
    nts::amp::CabinetParameters parameters;
    parameters.blend = 0.5f; parameters.lowCutHz = 20.0f; parameters.highCutHz = 20000.0f;
    cabinet.setParameters(parameters, 0); cabinet.reset();

    tests.expectEqual(cabinet.latencySamples(), std::size_t { 0 },
                      "the built-in responses stay on the zero-latency direct path");

    const auto makeResponse = [](std::size_t length, float decay)
    {
        std::vector<float> response(length);
        for (std::size_t index = 0; index < length; ++index)
            response[index] = (index == 0 ? 0.8f : 0.0f)
                            + 0.2f * std::exp(-static_cast<float>(index) / decay)
                            * std::sin(0.23f * static_cast<float>(index));
        return response;
    };

    // Long enough to cross the threshold, which must move *both* slots.
    const auto longResponse = makeResponse(2048, 260.0f);
    tests.expect(cabinet.loadImpulseA(longResponse, {}, { "Long", "57", 0.5f }, 64),
                 "cabinet accepts a response long enough to want the partitioned path");
    tests.expect(cabinet.latencySamples() > 0,
                 "a long response puts the section on the buffered path and reports its latency");

    std::array<float, blockSize> left {}, right {};
    float* channels[] { left.data(), right.data() };
    auto largestStep = 0.0f;
    auto previous = 0.0f;
    auto finite = true;
    for (int block = 0; block < 64; ++block)
    {
        for (std::size_t sample = 0; sample < blockSize; ++sample)
        {
            const auto phase = static_cast<float>(block * static_cast<int>(blockSize) + static_cast<int>(sample));
            left[sample] = right[sample] = 0.3f * std::sin(0.04f * phase);
        }
        cabinet.process(channels, 2, blockSize);
        for (const auto value : left)
        {
            finite = finite && std::isfinite(value);
            largestStep = std::max(largestStep, std::abs(value - previous));
            previous = value;
        }
    }
    tests.expect(finite, "the buffered cabinet path stays finite");
    tests.expect(largestStep < 0.5f, "switching to the buffered path does not step the output");

    // Back to a short response: the section must return to the direct path and give the latency
    // back, or a user who tries a long impulse and undoes it is left paying for it forever.
    const auto shortResponse = makeResponse(256, 60.0f);
    tests.expect(cabinet.loadImpulseA(shortResponse, {}, { "Short", "57", 0.5f }, 64),
                 "cabinet accepts a short response again");
    tests.expectEqual(cabinet.latencySamples(), std::size_t { 0 },
                      "returning to short responses returns to the zero-latency path");
}

/// Replacing a cabinet response while audio is running -- what user IR loading needs.
void testCabinetImpulseSwapping(TestHarness& tests)
{
    // A response can only be replaced safely if it is staged into an inactive buffer and
    // faded across on the audio thread. Overwriting coefficients in place, which is what the
    // section used to do, races the audio thread and produces garbage.
    nts::amp::CabinetSection cabinet; cabinet.prepare(stereoSpec);
    nts::amp::CabinetParameters parameters;
    parameters.blend = 0.0f; parameters.lowCutHz = 20.0f; parameters.highCutHz = 20000.0f;
    cabinet.setParameters(parameters, 0); cabinet.reset();

    const auto drive = [&](std::size_t blocks, std::size_t samplesPerBlock)
    {
        std::vector<float> captured;
        std::array<float, blockSize> left {}, right {};
        float* channels[] { left.data(), right.data() };
        for (std::size_t block = 0; block < blocks; ++block)
        {
            left.fill(0.0f); right.fill(0.0f);
            for (std::size_t sample = 0; sample < samplesPerBlock; ++sample)
                left[sample] = right[sample] = 0.25f;
            cabinet.process(channels, 2, samplesPerBlock);
            for (std::size_t sample = 0; sample < samplesPerBlock; ++sample) captured.push_back(left[sample]);
        }
        return captured;
    };

    static_cast<void>(drive(4, blockSize));
    // Longer than the synthesized response in the other slot, so the tail assertion below is
    // actually reading the replacement rather than whatever slot B still holds.
    std::vector<float> replacement(512);
    for (std::size_t index = 0; index < replacement.size(); ++index)
        replacement[index] = (index == 0 ? 0.9f : 0.0f)
                           + 0.2f * std::exp(-static_cast<float>(index) / 90.0f)
                           * std::sin(0.27f * static_cast<float>(index));
    tests.expect(cabinet.loadImpulseA(replacement, {}, { "Replacement", "57", 0.5f }, 256),
                 "cabinet accepts a replacement response while running");

    const auto during = drive(16, blockSize);
    auto largestStep = 0.0f;
    for (std::size_t index = 1; index < during.size(); ++index)
        largestStep = std::max(largestStep, std::abs(during[index] - during[index - 1]));
    tests.expect(std::isfinite(largestStep) && largestStep < 0.5f,
                 "swapping the cabinet response fades rather than steps");
    tests.expectEqual(cabinet.tailSamples(), replacement.size(),
                      "tail follows the newly staged response");

    // Short blocks are the case that rules out FFT partitioning here: a partitioned convolver
    // advances its overlap by a whole block whatever it is handed, so anything less than a
    // full block corrupts its state. Direct convolution is invariant to how the input is cut up.
    nts::amp::CabinetSection uniform; uniform.prepare(stereoSpec);
    nts::amp::CabinetSection chopped; chopped.prepare(stereoSpec);
    uniform.setParameters(parameters, 0); chopped.setParameters(parameters, 0);
    uniform.reset(); chopped.reset();
    std::array<float, blockSize> a {}, b {}, c {}, d {};
    for (std::size_t index = 0; index < blockSize; ++index)
        a[index] = b[index] = c[index] = d[index] = 0.3f * std::sin(0.07f * static_cast<float>(index));
    float* wholeChannels[] { a.data(), b.data() };
    float* choppedChannels[] { c.data(), d.data() };
    uniform.process(wholeChannels, 2, blockSize);
    float* firstHalf[] { c.data(), d.data() };
    chopped.process(firstHalf, 2, blockSize / 4);
    float* secondPart[] { c.data() + blockSize / 4, d.data() + blockSize / 4 };
    chopped.process(secondPart, 2, blockSize - blockSize / 4);
    auto matches = true;
    for (std::size_t index = 0; index < blockSize; ++index)
        matches = matches && std::abs(a[index] - c[index]) < 1.0e-6f;
    tests.expect(matches, "cabinet output does not depend on how the block is subdivided");

    // The case the whole change exists for: a loader thread replacing responses while the
    // audio thread keeps convolving. Worth running under the sanitizer leg, which is where a
    // torn read would actually be caught rather than merely producing odd numbers.
    nts::amp::CabinetSection shared; shared.prepare(stereoSpec);
    shared.setParameters(parameters, 0); shared.reset();
    std::atomic<bool> running { true };
    std::atomic<bool> finite { true };
    std::thread loader([&shared, &running]
    {
        std::vector<float> response(256);
        for (int generation = 0; running.load(std::memory_order_relaxed); ++generation)
        {
            for (std::size_t index = 0; index < response.size(); ++index)
                response[index] = 0.4f * std::sin(0.01f * static_cast<float>(index * (generation % 7 + 1)));
            static_cast<void>(shared.loadImpulseA(response, {}, { "Live", "57", 0.5f }, 128));
            std::this_thread::yield();
        }
    });
    std::array<float, blockSize> sl {}, sr {};
    float* sharedChannels[] { sl.data(), sr.data() };
    for (int block = 0; block < 4000; ++block)
    {
        for (std::size_t index = 0; index < blockSize; ++index)
            sl[index] = sr[index] = 0.2f * std::sin(0.05f * static_cast<float>(block * blockSize + index));
        shared.process(sharedChannels, 2, blockSize);
        for (std::size_t index = 0; index < blockSize; ++index)
            finite = finite && std::isfinite(sl[index]) && std::abs(sl[index]) < 12.0f;
    }
    running.store(false, std::memory_order_relaxed);
    loader.join();
    tests.expect(finite.load(), "cabinet stays finite and bounded while responses are replaced concurrently");
}

/** The topology table: names, round-tripping, and that no two voicings are the same amplifier.

    Worth its own test because a voicing is a block of literals. A copy-paste that left two
    entries identical would still build, still serialize, still load, and still sound wrong --
    the only thing that catches it is asking whether the seven are actually seven.
*/
void testTopologyTable(TestHarness& tests)
{
    using nts::amp::Topology;

    auto namesDistinct = true;
    auto namesPresent = true;
    for (std::size_t first = 0; first < nts::amp::topologyCount; ++first)
    {
        const auto a = static_cast<Topology>(first);
        namesPresent = namesPresent && ! nts::amp::topologyKey(a).empty()
                                    && ! nts::amp::topologyName(a).empty();
        for (std::size_t second = first + 1; second < nts::amp::topologyCount; ++second)
        {
            const auto b = static_cast<Topology>(second);
            namesDistinct = namesDistinct && nts::amp::topologyKey(a) != nts::amp::topologyKey(b)
                                          && nts::amp::topologyName(a) != nts::amp::topologyName(b);
        }
    }
    tests.expect(namesPresent, "every topology has a preset key and a display name");
    tests.expect(namesDistinct, "no two topologies share a key or a name");

    auto keysRoundTrip = true;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
    {
        const auto topology = static_cast<Topology>(index);
        keysRoundTrip = keysRoundTrip
            && nts::amp::topologyFromKey(nts::amp::topologyKey(topology)) == topology;
    }
    tests.expect(keysRoundTrip, "a topology key parses back to the topology that wrote it");

    // The forward-compatibility case: a preset naming a voicing this build has never heard of
    // loads as the default rather than failing, so the rest of its values survive.
    tests.expect(! nts::amp::topologyFromKey("someFutureVoicing").has_value(),
                 "an unknown topology key does not parse to a guess");

    auto presetsRoundTrip = true;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
    {
        const auto topology = static_cast<Topology>(index);
        for (const auto instrument : { nts::amp::Instrument::guitar, nts::amp::Instrument::bass })
        {
            const auto preset = nts::amp::makeOriginalPreset(topology, instrument);
            const auto restored = nts::amp::deserializePreset(nts::amp::serializePreset(preset));
            presetsRoundTrip = presetsRoundTrip && restored.has_value()
                            && restored->parameters.topology == topology
                            && restored->parameters.instrument == instrument
                            && restored->parameters == preset.parameters;
        }
    }
    tests.expect(presetsRoundTrip, "every topology's factory preset survives a serialize/restore");

    auto voicingsDiffer = true;
    for (std::size_t first = 0; first < nts::amp::topologyCount; ++first)
        for (std::size_t second = first + 1; second < nts::amp::topologyCount; ++second)
        {
            auto a = nts::amp::makeOriginalPreset(static_cast<Topology>(first),
                                                  nts::amp::Instrument::guitar).parameters;
            auto b = nts::amp::makeOriginalPreset(static_cast<Topology>(second),
                                                  nts::amp::Instrument::guitar).parameters;
            // Cleared, because topology is stored on the parameters and would make every pair
            // differ trivially. What is being asked is whether the *amplifier* differs.
            a.topology = b.topology = Topology::tightModern;
            voicingsDiffer = voicingsDiffer && ! (a == b);
        }
    tests.expect(voicingsDiffer, "no two topologies produce the same amplifier settings");
}

void testCabinetAndPresets(TestHarness& tests)
{
    nts::amp::CabinetSection cabinet; cabinet.prepare(stereoSpec);
    const std::array<float, 3> impulseA { 1.0f, 0.3f, -0.1f };
    const std::array<float, 3> impulseB { 0.5f, -0.2f, 0.1f };
    tests.expect(cabinet.loadImpulseA(impulseA, {}, { "A", "57", 0.2f })
                 && cabinet.loadImpulseB(impulseB, {}, { "B", "121", 0.8f }),
                 "cabinet accepts two IRs with mic metadata");
    nts::amp::CabinetParameters parameters; parameters.blend = 0.5f; parameters.slots[1].delaySamples = 3;
    parameters.slots[1].phaseInvert = true; parameters.lowCutHz = 20.0f; parameters.highCutHz = 20000.0f;
    cabinet.setParameters(parameters, 0); cabinet.reset();
    std::array<float, blockSize> left {}; left[0] = 1.0f; auto right = left; float* channels[] { left.data(), right.data() };
    cabinet.process(channels, 2, blockSize);
    tests.expect(cabinet.metadataA().name == "A" && cabinet.metadataB().microphone == "121"
                 && finite(left), "dual cabinet blend, phase, alignment, and metadata are active");
    // Tail has to cover the longer of the two IRs, and B is read through the alignment delay.
    tests.expectEqual(cabinet.tailSamples(), impulseB.size() + 3,
                      "cabinet tail covers the aligned second impulse");
    parameters.bypass = true; cabinet.setParameters(parameters, 0); left.fill(0.25f); right = left;
    cabinet.process(channels, 2, blockSize); tests.expectNear(left.front(), 0.25, 1.0e-7, "cabinet bypass is transparent");
    tests.expectEqual(cabinet.tailSamples(), std::size_t {}, "bypassed cabinet reports no tail");
    parameters.bypass = false; cabinet.setParameters(parameters, 0);

    testCabinetImpulseSwapping(tests);
    testCabinetBlendEngagement(tests);
    testCabinetStereoWidth(tests);
    testCabinetSlotControls(tests);
    testCabinetTrueStereo(tests);
    testCabinetLongImpulsePath(tests);

    auto original = nts::amp::makeOriginalPreset(nts::amp::Topology::vintageBloom, nts::amp::Instrument::bass);
    original.parameters.stages[0].memoryAmount = 0.731f;
    original.parameters.stages[0].frequencySaturation = 0.417f;
    original.parameters.phaseInverter.headroom = 0.643f;
    original.parameters.cabinet.slots[1].phaseInvert = true;
    original.parameters.cabinet.slots[1].delaySamples = 17;
    original.parameters.bass.lowMono = 0.37f;
    original.parameters.postHighDb = -2.25f;
    original.parameters.gateEnabled = false;
    original.parameters.gateThresholdDb = -47.5f;
    original.parameters.gateDepthDb = -18.25f;
    original.parameters.gateAttackMs = 4.5f;
    original.parameters.gateHoldMs = 120.0f;
    original.parameters.gateReleaseMs = 640.0f;
    const auto json = nts::amp::serializePreset(original);
    const auto restored = nts::amp::deserializePreset(json);
    tests.expect(restored.has_value() && restored->name == original.name
                 && restored->parameters.instrument == nts::amp::Instrument::bass
                 && restored->parameters.topology == nts::amp::Topology::vintageBloom
                 && restored->parameters.stageCount == original.parameters.stageCount
                 && std::abs(restored->parameters.stages[0].memoryAmount - 0.731f) < 1.0e-5f
                 && std::abs(restored->parameters.stages[0].frequencySaturation - 0.417f) < 1.0e-5f
                 && std::abs(restored->parameters.phaseInverter.headroom - 0.643f) < 1.0e-5f
                 && restored->parameters.cabinet.slots[1].phaseInvert
                 && restored->parameters.cabinet.slots[1].delaySamples == 17
                 && std::abs(restored->parameters.bass.lowMono - 0.37f) < 1.0e-5f
                 && std::abs(restored->parameters.postHighDb + 2.25f) < 1.0e-5f
                 && ! restored->parameters.gateEnabled
                 && std::abs(restored->parameters.gateThresholdDb + 47.5f) < 1.0e-5f
                 && std::abs(restored->parameters.gateDepthDb + 18.25f) < 1.0e-5f
                 && std::abs(restored->parameters.gateAttackMs - 4.5f) < 1.0e-5f
                 && std::abs(restored->parameters.gateHoldMs - 120.0f) < 1.0e-5f
                 && std::abs(restored->parameters.gateReleaseMs - 640.0f) < 1.0e-5f,
                 "amp preset schema serializes and restores every expert subsystem");

    {
        // The gate sits ahead of the preamp, so a quiet part that survives it is
        // then amplified by everything downstream. These check the two things the
        // user actually asked for: that it can be switched off entirely, and that
        // depth controls how much is removed rather than always muting.
        const auto tailLevelWith = [&](bool enabled, float depthDb, float releaseMs)
        {
            auto preset = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern,
                                                        nts::amp::Instrument::guitar);
            preset.parameters.gateEnabled = enabled;
            preset.parameters.gateThresholdDb = -30.0f;
            preset.parameters.gateDepthDb = depthDb;
            preset.parameters.gateAttackMs = 2.0f;
            preset.parameters.gateHoldMs = 10.0f;
            preset.parameters.gateReleaseMs = releaseMs;

            nts::amp::TraditionalAmpProcessor amp;
            amp.prepare({ sampleRate, 512, 1 });
            amp.loadPreset(preset, 0);

            // A loud note that opens the gate, then a long quiet tail below the
            // threshold, which is exactly the material a gate destroys. The tail
            // is measured well after the loud section so the cabinet's own
            // convolution ring-out has decayed and is not counted as signal.
            const auto loudSamples = static_cast<std::size_t>(sampleRate * 0.25);
            const auto tailSamples = static_cast<std::size_t>(sampleRate * 1.5);
            auto signal = sine(loudSamples + tailSamples, 220.0, 1.0f);
            for (std::size_t index = loudSamples; index < signal.size(); ++index)
                signal[index] *= 0.004f;
            const auto rendered = nts::amp::renderOffline(amp, signal, 256);
            const auto measureFrom = loudSamples + static_cast<std::size_t>(sampleRate * 1.0);
            return magnitude(std::span(rendered).subspan(measureFrom), 220.0);
        };

        // Measured: the full-depth gate leaves roughly 1/1800th of the tail that
        // an open gate does, about 65 dB, which is what "killing the tone" means
        // in numbers. A shallow gate sits between the two rather than muting.
        const auto gatedTail = tailLevelWith(true, -80.0f, 40.0f);
        const auto openTail = tailLevelWith(false, -80.0f, 40.0f);
        const auto shallowTail = tailLevelWith(true, -12.0f, 40.0f);
        tests.expect(openTail > gatedTail * 20.0,
                     "disabling the gate leaves the quiet tail the gate removed");
        tests.expect(shallowTail > gatedTail * 5.0,
                     "reducing gate depth removes less of the quiet tail");
        tests.expect(shallowTail < openTail,
                     "gate depth is a continuum between muting and bypass");
    }
}

void testCompleteGraphs(TestHarness& tests)
{
    auto guitarPreset = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern, nts::amp::Instrument::guitar);
    auto bassPreset = nts::amp::makeOriginalPreset(nts::amp::Topology::vintageBloom, nts::amp::Instrument::bass);
    bassPreset.parameters.bass.cleanBlend = 0.9f; bassPreset.parameters.bass.highDriveDb = 24.0f;

    nts::amp::TraditionalAmpProcessor guitar, bass;
    guitar.prepare({ sampleRate, blockSize, 1 }); bass.prepare({ sampleRate, blockSize, 1 });
    auto minimumLatency = guitarPreset.parameters;
    for (auto& stage : minimumLatency.stages) stage.oversamplingFactor = 1;
    guitar.setParametersImmediately(minimumLatency);
    tests.expectEqual(guitar.latencySamples(), std::size_t { 0 },
                      "immediate 1x real-time configuration has zero algorithmic latency");
    guitar.setParametersImmediately(guitarPreset.parameters);
    guitar.loadPreset(guitarPreset, 1); bass.loadPreset(bassPreset, 1);
    auto guitarInput = nts::dsp::StimulusGenerator::guitarDi(8192, sampleRate);
    auto bassInput = sine(8192, 55.0, 0.35f); auto guitarOutput = guitarInput; auto bassOutput = bassInput;
    for (std::size_t offset = 0; offset < guitarOutput.size(); offset += blockSize)
    {
        float* guitarChannel[] { guitarOutput.data() + offset }; guitar.process(guitarChannel, 1, blockSize);
        float* bassChannel[] { bassOutput.data() + offset }; bass.process(bassChannel, 1, blockSize);
    }
    tests.expect(finite(guitarOutput) && finite(bassOutput), "complete guitar and bass graphs process finite audio");
    tests.expect(nts::dsp::hashAudio(guitarOutput) != nts::dsp::hashAudio(bassOutput),
                 "guitar and bass modes have clearly different deterministic behavior");
    tests.expect(magnitude(std::span(bassOutput).subspan(1024), 55.0) > 0.015,
                 "bass clean blend preserves the low fundamental under heavy high-path drive");
    tests.expect(guitar.latencySamples() >= 16 && bass.latencySamples() >= 16,
                 "complete graph reports cascaded nonlinear-stage latency");

    auto switchSignal = sine(blockSize * 16, 220.0, 0.12f);
    nts::amp::TraditionalAmpProcessor switching; switching.prepare({ sampleRate, blockSize, 1 });
    switching.loadPreset(guitarPreset, blockSize * 4);
    float maximumStep {};
    for (std::size_t offset = 0; offset < switchSignal.size(); offset += blockSize)
    {
        if (offset == blockSize * 6) switching.loadPreset(bassPreset, blockSize * 4);
        float* channel[] { switchSignal.data() + offset }; switching.process(channel, 1, blockSize);
        if (offset > 0) maximumStep = std::max(maximumStep,
            std::abs(switchSignal[offset] - switchSignal[offset - 1]));
    }
    tests.expect(maximumStep < 0.5f, "preset changes crossfade without a click-sized discontinuity");
}

void testAudioRegressionAndRealtime(TestHarness& tests)
{
    const std::array<std::vector<float>, 6> stimuli {
        sine(4096, 997.0, 0.4f),
        nts::dsp::StimulusGenerator::multiTone(4096, sampleRate, std::array<double, 2> { 700.0, 1100.0 }),
        nts::dsp::StimulusGenerator::palmMute(4096, sampleRate),
        nts::dsp::StimulusGenerator::guitarDi(4096, sampleRate),
        nts::dsp::StimulusGenerator::bassDi(4096, sampleRate),
        nts::dsp::StimulusGenerator::transient(4096, sampleRate)
    };
    const auto preset = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern, nts::amp::Instrument::guitar);
    for (const auto& stimulus : stimuli)
    {
        nts::amp::TraditionalAmpProcessor first, second;
        first.prepare({ sampleRate, blockSize, 1 }); second.prepare({ sampleRate, blockSize, 1 });
        first.loadPreset(preset, 1); second.loadPreset(preset, 1);
        const auto firstRender = nts::amp::renderOffline(first, stimulus, blockSize);
        const auto secondRender = nts::amp::renderOffline(second, stimulus, blockSize);
        tests.expect(nts::dsp::hashAudio(firstRender) == nts::dsp::hashAudio(secondRender) && finite(firstRender),
                     "sine, intermodulation, palm, chord, bass, and slap/transient renders are deterministic");
    }

    nts::amp::TraditionalAmpProcessor realtime; realtime.prepare(stereoSpec); realtime.loadPreset(preset, 1);
    std::array<float, blockSize> left; left.fill(0.1f); auto right = left; float* channels[] { left.data(), right.data() };
    realtime.process(channels, 2, blockSize);
    allocationCount.store(0); countAllocations = true;
    realtime.process(channels, 2, blockSize);
    countAllocations = false;
    tests.expectEqual(allocationCount.load(), std::size_t { 0 }, "complete amp graph allocates nothing while processing");

    constexpr int iterations = 200;
    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) realtime.process(channels, 2, blockSize);
    const auto averageUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - started).count() / iterations;
    const auto callbackBudgetUs = 1.0e6 * blockSize / sampleRate;
    tests.expect(averageUs < callbackBudgetUs * 0.5,
                 "stereo traditional amp remains below 50% callback budget at 4x stage oversampling");

    /* Track B3: what the solid-state voicings will cost.

       They ask for 8x on their preamp stages rather than the 4x every valve voicing runs, because
       a hard clipper needs it -- see the fold-back measurements in `testWaveshapeSelection`. This
       is the bill for that, measured rather than assumed, and it is a *guard* as much as a figure:
       the two voicings blocked on Tracks C and D will land in this graph, and the budget they land
       in has to have been checked before they get there rather than after.

       Held to the same 50% of the callback the valve configuration is held to. Doubling the
       oversampling factor does not double the whole amplifier's cost -- the cabinet convolution,
       the tone stack and the power section are all unchanged -- so the headroom is there, and if
       it ever is not, this fails before a user finds it.
    */
    auto solidState = preset;
    solidState.parameters.stageCount = 2;
    for (auto& stage : solidState.parameters.stages)
    {
        stage.oversamplingFactor = 8;
        stage.shape = nts::dsp::Waveshape::hardClip;
    }
    solidState.parameters.powerAmp.shape = nts::dsp::Waveshape::hardClip;
    nts::amp::TraditionalAmpProcessor hardClipped;
    hardClipped.prepare(stereoSpec); hardClipped.loadPreset(solidState, 1);
    hardClipped.process(channels, 2, blockSize);
    const auto hardStarted = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) hardClipped.process(channels, 2, blockSize);
    const auto hardAverageUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - hardStarted).count() / iterations;
    tests.expect(hardAverageUs < callbackBudgetUs * 0.5,
                 "stereo amp stays below 50% callback budget at 8x with hard-clip stages");
    tests.expect(finite(std::vector<float>(left.begin(), left.end())),
                 "an 8x hard-clip configuration produces finite output");
}
} // namespace

/** The seven original voicings still render exactly the audio they always did.

    Every other test in this file asks whether the amplifier is *reasonable*. This one asks the
    only question that cannot be recovered from later: whether a project somebody saved recalls
    the sound it was saved with. The seven voicings that predate the bass-native block are a
    compatibility surface, and the hashes below were taken from the build immediately before that
    block was added -- so they are a record of the shipped sound, not of the current code's
    opinion of itself.

    **These numbers are never to be regenerated to make a failing build pass.** A diff here means
    either a real change of sound in a voicing that is not allowed to change one, or a deliberate
    decision that has to be made and written down. Regenerating them turns the test into a
    tautology, which is exactly the failure it exists to prevent -- and the plan that added the
    bass voicings leans on it as the precondition for touching the shared saturator, the bass path
    and the dry blend at all.

    A hash rather than stored audio because the failure is binary. There is no useful notion of
    "nearly the right amplifier" here, and 14 renders of stored float would be half a megabyte in
    the repository to say the same thing.
*/
void testOriginalVoicingRegression(TestHarness& tests)
{
    struct Golden { std::string_view name; std::uint64_t hash; };
    // Captured from the pre-bass-topology build. See the note above before touching them.
    constexpr std::array<Golden, 14> goldens { {
        { "tightModern/guitar", 13736008448498962631ull },
        { "tightModern/bass", 6352694481359901093ull },
        { "vintageBloom/guitar", 17866495747281218636ull },
        { "vintageBloom/bass", 9071400359841382050ull },
        { "americanClean/guitar", 8115985061413987186ull },
        { "americanClean/bass", 17103613497921938587ull },
        { "britishCrunch/guitar", 15760522335369434937ull },
        { "britishCrunch/bass", 2360016342175368192ull },
        { "classAChime/guitar", 1197813929854403933ull },
        { "classAChime/bass", 14523401880314385465ull },
        { "saggingRectifier/guitar", 13373013645590942393ull },
        { "saggingRectifier/bass", 7667099778163236981ull },
        { "studioDirect/guitar", 18071372094858142022ull },
        { "studioDirect/bass", 3514709347172934587ull }
    } };

    // An A at 110 Hz: low enough that the bass path's crossover and the supply sag both engage,
    // and loud enough at 0.5 that every voicing is doing something non-linear to it.
    std::vector<float> input (4096);
    for (std::size_t n = 0; n < input.size(); ++n)
        input[n] = 0.5f * static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 110.0 * static_cast<double>(n) / sampleRate));

    std::size_t golden {};
    auto allMatch = true;
    for (std::size_t index = 0; index < 7; ++index)
        for (const auto instrument : { nts::amp::Instrument::guitar, nts::amp::Instrument::bass })
        {
            const auto topology = static_cast<nts::amp::Topology>(index);
            nts::amp::TraditionalAmpProcessor processor;
            processor.prepare({ sampleRate, blockSize, 1 });
            processor.loadPreset(nts::amp::makeOriginalPreset(topology, instrument), 0);
            const auto rendered = nts::amp::renderOffline(processor, input, blockSize);

            std::uint64_t hash = 1469598103934665603ull;
            for (const auto sample : rendered)
            {
                std::uint32_t bits {};
                std::memcpy(&bits, &sample, sizeof bits);
                hash = (hash ^ bits) * 1099511628211ull;
            }
            if (hash != goldens[golden].hash)
            {
                allMatch = false;
                tests.expect(false, std::string { "voicing changed sound: " }
                                        + std::string { goldens[golden].name });
            }
            ++golden;
        }
    tests.expect(allMatch, "the seven original voicings render bit-identical audio on both instruments");

    // The bass-native voicings answer for guitar too. A host can write any (instrument, topology)
    // pair, and an uninitialised preset coming back is the failure this catches.
    auto guitarReadingsValid = true;
    for (std::size_t index = 7; index < nts::amp::topologyCount; ++index)
    {
        const auto preset = nts::amp::makeOriginalPreset(static_cast<nts::amp::Topology>(index),
                                                         nts::amp::Instrument::guitar);
        guitarReadingsValid = guitarReadingsValid
            && ! preset.name.empty() && preset.parameters.stageCount >= 2
            && preset.parameters.preEq.lowCutHz > 0.0f;
    }
    tests.expect(guitarReadingsValid, "every bass-native voicing returns a real preset for guitar");

    // The trap that motivated moving the bass block ahead of the switch: a voicing whose `p.bass`
    // was overwritten on the way out would compile, run, and sound plausible.
    auto bassPathsDiffer = true;
    for (std::size_t first = 7; first < nts::amp::topologyCount; ++first)
        for (std::size_t second = first + 1; second < nts::amp::topologyCount; ++second)
            bassPathsDiffer = bassPathsDiffer
                && ! (nts::amp::makeOriginalPreset(static_cast<nts::amp::Topology>(first),
                                                   nts::amp::Instrument::bass).parameters.bass
                   == nts::amp::makeOriginalPreset(static_cast<nts::amp::Topology>(second),
                                                   nts::amp::Instrument::bass).parameters.bass);
    tests.expect(bassPathsDiffer, "each bass-native voicing keeps its own bass path");

    // Affinity is what the picker filters on. Getting the boundary wrong hides real voicings or
    // shows guitarists an amplifier with no guitar reading worth having.
    auto affinityCorrect = true;
    for (std::size_t index = 0; index < nts::amp::topologyCount; ++index)
        affinityCorrect = affinityCorrect
            && (nts::amp::topologyAffinity(static_cast<nts::amp::Topology>(index))
                    == (index < 7 ? nts::amp::TopologyAffinity::either
                                  : nts::amp::TopologyAffinity::bass));
    tests.expect(affinityCorrect, "topology affinity splits at the first bass-native voicing");
}

int main()
{
    TestHarness tests;
    testCalibrationAndPreEq(tests); testPreampStages(tests); testWaveshapeSelection(tests);
    testBiAmpPath(tests); testDryBlendAlignment(tests); testVoicingBoundsAndLevels(tests);
    testPanelSwitches(tests); testTonePhaseAndPower(tests);
    return tests.result();
}
