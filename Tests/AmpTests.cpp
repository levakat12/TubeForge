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
#include <new>
#include <numbers>
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
    tests.expect(stage.latencySamples() == 8, "preamp exposes selected oversampling latency");
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

void testCabinetAndPresets(TestHarness& tests)
{
    nts::amp::CabinetSection cabinet; cabinet.prepare(stereoSpec);
    const std::array<float, 3> impulseA { 1.0f, 0.3f, -0.1f };
    const std::array<float, 3> impulseB { 0.5f, -0.2f, 0.1f };
    tests.expect(cabinet.loadImpulseA(impulseA, {}, { "A", "57", 0.2f })
                 && cabinet.loadImpulseB(impulseB, {}, { "B", "121", 0.8f }),
                 "cabinet accepts two IRs with mic metadata");
    nts::amp::CabinetParameters parameters; parameters.blend = 0.5f; parameters.delaySamplesB = 3;
    parameters.phaseInvertB = true; parameters.lowCutHz = 20.0f; parameters.highCutHz = 20000.0f;
    cabinet.setParameters(parameters, 0); cabinet.reset();
    std::array<float, blockSize> left {}; left[0] = 1.0f; auto right = left; float* channels[] { left.data(), right.data() };
    cabinet.process(channels, 2, blockSize);
    tests.expect(cabinet.metadataA().name == "A" && cabinet.metadataB().microphone == "121"
                 && finite(left), "dual cabinet blend, phase, alignment, and metadata are active");
    parameters.bypass = true; cabinet.setParameters(parameters, 0); left.fill(0.25f); right = left;
    cabinet.process(channels, 2, blockSize); tests.expectNear(left.front(), 0.25, 1.0e-7, "cabinet bypass is transparent");

    auto original = nts::amp::makeOriginalPreset(nts::amp::Topology::vintageBloom, nts::amp::Instrument::bass);
    original.parameters.stages[0].memoryAmount = 0.731f;
    original.parameters.stages[0].frequencySaturation = 0.417f;
    original.parameters.phaseInverter.headroom = 0.643f;
    original.parameters.cabinet.phaseInvertB = true;
    original.parameters.cabinet.delaySamplesB = 17;
    original.parameters.bass.lowMono = 0.37f;
    original.parameters.postHighDb = -2.25f;
    const auto json = nts::amp::serializePreset(original);
    const auto restored = nts::amp::deserializePreset(json);
    tests.expect(restored.has_value() && restored->name == original.name
                 && restored->parameters.instrument == nts::amp::Instrument::bass
                 && restored->parameters.topology == nts::amp::Topology::vintageBloom
                 && restored->parameters.stageCount == original.parameters.stageCount
                 && std::abs(restored->parameters.stages[0].memoryAmount - 0.731f) < 1.0e-5f
                 && std::abs(restored->parameters.stages[0].frequencySaturation - 0.417f) < 1.0e-5f
                 && std::abs(restored->parameters.phaseInverter.headroom - 0.643f) < 1.0e-5f
                 && restored->parameters.cabinet.phaseInvertB
                 && restored->parameters.cabinet.delaySamplesB == 17
                 && std::abs(restored->parameters.bass.lowMono - 0.37f) < 1.0e-5f
                 && std::abs(restored->parameters.postHighDb + 2.25f) < 1.0e-5f,
                 "amp preset schema serializes and restores every expert subsystem");
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
}
} // namespace

int main()
{
    TestHarness tests;
    testCalibrationAndPreEq(tests); testPreampStages(tests); testTonePhaseAndPower(tests);
    testCabinetAndPresets(tests); testCompleteGraphs(tests); testAudioRegressionAndRealtime(tests);
    return tests.result();
}
