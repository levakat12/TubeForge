#include "TestHarness.h"
#include "fixtures/RecordedDiFixtures.h"

#include <nts/dsp/Analysis.h>
#include <nts/dsp/BassSplit.h>
#include <nts/dsp/Common.h>
#include <nts/dsp/Convolution.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Filters.h>
#include <nts/dsp/Metering.h>
#include <nts/dsp/ModeCrossfader.h>
#include <nts/dsp/Nonlinear.h>
#include <nts/dsp/Oversampling.h>
#include <nts/dsp/Regression.h>
#include <nts/dsp/Smoothing.h>
#include <nts/dsp/Simd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <memory>
#include <new>
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
constexpr std::size_t blockSize = 64;
const nts::dsp::ProcessSpec monoSpec { sampleRate, blockSize, 1 };
const nts::dsp::ProcessSpec stereoSpec { sampleRate, blockSize, 2 };

std::vector<float> sine(std::size_t samples, double frequency, float amplitude = 0.5f)
{
    std::vector<float> result(samples);
    for (std::size_t index = 0; index < samples; ++index)
        result[index] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * index / sampleRate));
    return result;
}

double rms(std::span<const float> samples)
{
    double energy {};
    for (const auto sample : samples) energy += sample * sample;
    return std::sqrt(energy / std::max<std::size_t>(1, samples.size()));
}

double toneMagnitude(std::span<const float> samples, double frequency)
{
    std::complex<double> sum {};
    for (std::size_t index = 0; index < samples.size(); ++index)
        sum += static_cast<double>(samples[index]) * std::polar(1.0, -2.0 * std::numbers::pi * frequency * index / sampleRate);
    return 2.0 * std::abs(sum) / samples.size();
}

std::vector<float> referenceBiquadImpulse(const nts::dsp::BiquadCoefficients& coefficients,
                                          std::size_t samples)
{
    std::vector<float> result(samples);
    double x1 {}, x2 {}, y1 {}, y2 {};
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto input = index == 0 ? 1.0 : 0.0;
        const auto output = coefficients.b0 * input + coefficients.b1 * x1 + coefficients.b2 * x2
                          - coefficients.a1 * y1 - coefficients.a2 * y2;
        x2 = x1; x1 = input; y2 = y1; y1 = output;
        result[index] = static_cast<float>(output);
    }
    return result;
}

std::vector<float> referenceSvfImpulse(nts::dsp::StateVariableOutput outputType,
                                       double frequency, double q, std::size_t samples)
{
    std::vector<float> result(samples);
    const auto g = std::tan(std::numbers::pi * frequency / sampleRate);
    const auto k = 1.0 / q;
    double integrator1 {}, integrator2 {};
    for (std::size_t index = 0; index < samples; ++index)
    {
        const auto input = index == 0 ? 1.0 : 0.0;
        const auto band = (g * (input - integrator2) + integrator1) / (1.0 + g * (g + k));
        const auto low = integrator2 + g * band;
        integrator1 = 2.0 * band - integrator1;
        integrator2 = 2.0 * low - integrator2;
        const auto high = input - k * band - low;
        switch (outputType)
        {
            case nts::dsp::StateVariableOutput::lowPass: result[index] = static_cast<float>(low); break;
            case nts::dsp::StateVariableOutput::bandPass: result[index] = static_cast<float>(band); break;
            case nts::dsp::StateVariableOutput::highPass: result[index] = static_cast<float>(high); break;
            case nts::dsp::StateVariableOutput::notch: result[index] = static_cast<float>(high + low); break;
        }
    }
    return result;
}

void testGainAndSmoothing(TestHarness& tests)
{
    tests.expectNear(nts::dsp::dbToLinear(6.0205999f), 2.0, 1.0e-5, "dB converts to linear gain");
    tests.expectNear(nts::dsp::linearToDb(0.5f), -6.0205999, 1.0e-4, "linear gain converts to dB");
    nts::dsp::SmoothedGain gain; gain.prepare(1000.0); gain.setRampMilliseconds(10.0); gain.resetDb(0.0f); gain.setTargetDb(-6.0205999f);
    std::array<float, 10> samples; samples.fill(1.0f); gain.process(samples.data(), samples.size());
    tests.expect(samples.front() < 1.0f && samples.front() > 0.5f, "gain automation starts without a step");
    tests.expectNear(samples.back(), 0.5, 1.0e-5, "smoothed gain reaches target");
    nts::dsp::SmoothedParameter frequency; frequency.prepare(1000.0, 10.0, nts::dsp::SmoothingMode::logarithmic);
    frequency.reset(100.0f); frequency.setTarget(10000.0f);
    for (int index = 0; index < 5; ++index) static_cast<void>(frequency.next());
    tests.expectNear(frequency.value(), 1000.0, 2.0, "frequency smoothing interpolates logarithmically");
}

void testFilters(TestHarness& tests)
{
    using nts::dsp::BiquadCoefficients; using nts::dsp::FilterType;
    const auto low = BiquadCoefficients::make(FilterType::lowPass, sampleRate, 1000.0);
    const auto high = BiquadCoefficients::make(FilterType::highPass, sampleRate, 1000.0);
    tests.expect(low.magnitude(100.0, sampleRate) > low.magnitude(10000.0, sampleRate), "Butterworth low-pass response slopes down");
    tests.expect(high.magnitude(100.0, sampleRate) < high.magnitude(10000.0, sampleRate), "Butterworth high-pass response slopes up");
    tests.expectNear(BiquadCoefficients::make(FilterType::notch, sampleRate, 2000.0, 2.0).magnitude(2000.0, sampleRate), 0.0, 1.0e-5, "notch rejects center frequency");
    tests.expectNear(BiquadCoefficients::make(FilterType::allPass, sampleRate, 2000.0).magnitude(7000.0, sampleRate), 1.0, 1.0e-5, "all-pass has unity magnitude");
    tests.expect(BiquadCoefficients::make(FilterType::peaking, sampleRate, 1000.0, 1.0, 6.0).magnitude(1000.0, sampleRate) > 1.9, "peaking EQ applies boost");
    tests.expect(BiquadCoefficients::make(FilterType::lowShelf, sampleRate, 500.0, 0.707, 6.0).magnitude(30.0, sampleRate) > 1.8, "low shelf boosts lows");
    tests.expect(BiquadCoefficients::make(FilterType::highShelf, sampleRate, 5000.0, 0.707, 6.0).magnitude(18000.0, sampleRate) > 1.8, "high shelf boosts highs");

    for (const auto type : { FilterType::lowPass, FilterType::highPass, FilterType::peaking,
                             FilterType::lowShelf, FilterType::highShelf, FilterType::notch, FilterType::allPass })
    {
        const auto referenceCoefficients = BiquadCoefficients::make(type, sampleRate, 1800.0, 0.83, 5.0);
        const auto reference = referenceBiquadImpulse(referenceCoefficients, 128);
        std::array<float, 128> measured {}; measured[0] = 1.0f; float* measuredChannel[] { measured.data() };
        nts::dsp::Biquad referenceFilter; referenceFilter.prepare({ sampleRate, measured.size(), 1 });
        referenceFilter.setCoefficients(referenceCoefficients); referenceFilter.process(measuredChannel, 1, measured.size());
        for (std::size_t sample = 0; sample < measured.size(); ++sample)
            tests.expectNear(measured[sample], reference[sample], 2.0e-6,
                             "filter impulse matches independent direct-form reference");

        nts::dsp::Biquad filter; filter.prepare(stereoSpec);
        filter.setCoefficients(BiquadCoefficients::make(type, sampleRate, 1.0, 0.05, 18.0));
        std::array<float, blockSize> left {}; std::array<float, blockSize> right {};
        left[0] = right[0] = 1.0f; float* channels[] { left.data(), right.data() };
        for (int block = 0; block < 200; ++block)
        {
            filter.setCoefficients(BiquadCoefficients::make(type, sampleRate, block % 2 == 0 ? 1.0 : 23900.0, block % 3 == 0 ? 0.05 : 50.0, 18.0), blockSize);
            filter.process(channels, 2, blockSize);
            for (const auto sample : left) tests.expect(std::isfinite(sample) && std::abs(sample) < 1000.0f, "filter remains stable under extreme modulation");
            left.fill(0.0f); right.fill(0.0f);
        }
    }

    for (const auto type : { nts::dsp::OnePoleFilter::Type::lowPass, nts::dsp::OnePoleFilter::Type::highPass })
    {
        nts::dsp::OnePoleFilter onePole; onePole.prepare(monoSpec); onePole.setCutoff(type, 500.0);
        std::array<float, blockSize> onePoleData {}; onePoleData[0] = 1.0f;
        float* onePoleChannels[] { onePoleData.data() }; onePole.process(onePoleChannels, 1, blockSize);
        const auto coefficient = 1.0 - std::exp(-2.0 * std::numbers::pi * 500.0 / sampleRate);
        double lowState {};
        for (std::size_t sample = 0; sample < onePoleData.size(); ++sample)
        {
            const auto input = sample == 0 ? 1.0 : 0.0;
            lowState += coefficient * (input - lowState);
            const auto reference = type == nts::dsp::OnePoleFilter::Type::lowPass ? lowState : input - lowState;
            tests.expectNear(onePoleData[sample], reference, 1.0e-6,
                             "first-order filter impulse matches independent recurrence");
        }
    }

    for (const auto output : { nts::dsp::StateVariableOutput::lowPass, nts::dsp::StateVariableOutput::bandPass,
                               nts::dsp::StateVariableOutput::highPass, nts::dsp::StateVariableOutput::notch })
    {
        nts::dsp::StateVariableFilter svf; svf.prepare(stereoSpec); svf.setParameters(1000.0, 0.707, output);
        std::array<float, blockSize> left {}; std::array<float, blockSize> right {}; left[0] = right[0] = 1.0f;
        float* channels[] { left.data(), right.data() }; svf.process(channels, 2, blockSize);
        tests.expect(std::all_of(left.begin(), left.end(), [](float value) { return std::isfinite(value); }), "state-variable filter output is finite");
        const auto reference = referenceSvfImpulse(output, 1000.0, 0.707, blockSize);
        for (std::size_t sample = 0; sample < blockSize; ++sample)
            tests.expectNear(left[sample], reference[sample], 2.0e-6,
                             "state-variable filter impulse matches independent reference");
    }
}

void testCrossover(TestHarness& tests)
{
    {
        constexpr std::size_t impulseSamples = 128;
        nts::dsp::LinkwitzRileyCrossover crossover; crossover.prepare({ sampleRate, impulseSamples, 1 });
        crossover.setFrequency(800.0);
        std::array<float, impulseSamples> input {}; input[0] = 1.0f;
        std::array<float, impulseSamples> low {}, high {};
        const float* in[] { input.data() }; float* lo[] { low.data() }; float* hi[] { high.data() };
        crossover.process(in, lo, hi, 1, impulseSamples);
        const auto lowSection = referenceBiquadImpulse(
            nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::lowPass, sampleRate, 800.0), impulseSamples);
        const auto highSection = referenceBiquadImpulse(
            nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::highPass, sampleRate, 800.0), impulseSamples);
        for (std::size_t sample = 0; sample < impulseSamples; ++sample)
        {
            double lowReference {}, highReference {};
            for (std::size_t tap = 0; tap <= sample; ++tap)
            {
                lowReference += lowSection[tap] * lowSection[sample - tap];
                highReference += highSection[tap] * highSection[sample - tap];
            }
            tests.expectNear(low[sample], lowReference, 3.0e-6,
                             "Linkwitz-Riley low impulse matches independent cascade reference");
            tests.expectNear(high[sample], highReference, 3.0e-6,
                             "Linkwitz-Riley high impulse matches independent cascade reference");
        }
    }
    for (const auto frequency : { 80.0, 1000.0, 10000.0 })
    {
        nts::dsp::LinkwitzRileyCrossover crossover; crossover.prepare(monoSpec); crossover.setFrequency(800.0);
        auto input = sine(4096, frequency); std::vector<float> low(input.size()), high(input.size());
        for (std::size_t offset = 0; offset < input.size(); offset += blockSize)
        {
            const float* in[] { input.data() + offset }; float* lo[] { low.data() + offset }; float* hi[] { high.data() + offset };
            crossover.process(in, lo, hi, 1, blockSize);
        }
        std::vector<float> sum(input.size()); for (std::size_t index = 0; index < sum.size(); ++index) sum[index] = low[index] + high[index];
        tests.expect(std::abs(rms(std::span(sum).subspan(1024)) / rms(std::span(input).subspan(1024)) - 1.0) < 0.015,
                     "Linkwitz-Riley low and high bands sum flat");
    }
    nts::dsp::BassSplitProcessor bass; bass.prepare(stereoSpec); bass.setCrossoverFrequency(250.0);
    auto left = sine(blockSize, 100.0); auto right = left; float* channels[] { left.data(), right.data() };
    bass.process(channels, 2, blockSize); tests.expect(std::all_of(left.begin(), left.end(), [](float value) { return std::isfinite(value); }), "bass split stereo recombination is finite");
}

void testNonlinearAndOversampling(TestHarness& tests)
{
    for (const auto shape : { nts::dsp::Waveshape::hyperbolicTangent, nts::dsp::Waveshape::arcTangent,
                              nts::dsp::Waveshape::hardClip, nts::dsp::Waveshape::softClip,
                              nts::dsp::Waveshape::asymmetricPolynomial, nts::dsp::Waveshape::diode })
        for (const auto input : { -10.0f, -1.0f, 0.0f, 1.0f, 10.0f })
            tests.expect(std::isfinite(nts::dsp::shapeSample(input, shape, 3.0f)), "baseline waveshaper remains finite");
    nts::dsp::BiasableWaveshaper biased; biased.setBias(0.2f); biased.setDrive(4.0f);
    tests.expect(std::abs(biased.processSample(0.5f) + biased.processSample(-0.5f)) > 0.01f, "bias creates asymmetric transfer");
    nts::dsp::DynamicWaveshaper dynamic; dynamic.prepare(stereoSpec); dynamic.setParameters(2.0f, 1.0f, 1.0, 50.0);
    std::array<float, blockSize> dynamicLeft; dynamicLeft.fill(0.5f); std::array<float, blockSize> dynamicRight = dynamicLeft;
    float* dynamicChannels[] { dynamicLeft.data(), dynamicRight.data() }; dynamic.process(dynamicChannels, 2, blockSize);
    tests.expect(dynamicLeft.back() != dynamicLeft.front(), "dynamic waveshaper responds to its envelope");

    for (const auto factor : { nts::dsp::OversamplingFactor::x1, nts::dsp::OversamplingFactor::x2,
                               nts::dsp::OversamplingFactor::x4, nts::dsp::OversamplingFactor::x8 })
    {
        nts::dsp::Oversampler oversampler; oversampler.prepare(stereoSpec, factor);
        auto left = sine(blockSize, 1000.0); auto right = left; float* channels[] { left.data(), right.data() };
        oversampler.process(channels, 2, blockSize, [](float value) noexcept { return std::tanh(4.0f * value); });
        tests.expect(std::all_of(left.begin(), left.end(), [](float value) { return std::isfinite(value); }), "oversampling factor processes stereo safely");
        tests.expectEqual(oversampler.latencySamples(), factor == nts::dsp::OversamplingFactor::x1 ? std::size_t { 0 } : std::size_t { 8 }, "oversampling reports exact designed latency");
        if (factor != nts::dsp::OversamplingFactor::x1)
        {
            const auto numericFactor = static_cast<double>(factor);
            tests.expect(std::abs(oversampler.filterMagnitude(0.1 / numericFactor) - 1.0) < 0.02,
                         "oversampling anti-alias filter has low passband ripple");
            tests.expect(oversampler.filterMagnitude(0.9 / numericFactor) < 0.12,
                         "oversampling anti-alias filter attenuates the stopband");
            tests.expect(oversampler.hasLinearPhaseCoefficients(),
                         "oversampling FIR coefficients have linear phase");
        }
    }
    auto base = sine(8192, 10000.0, 0.9f); auto highQuality = base;
    nts::dsp::Oversampler x1; x1.prepare({ sampleRate, base.size(), 1 }, nts::dsp::OversamplingFactor::x1);
    nts::dsp::Oversampler x8; x8.prepare({ sampleRate, base.size(), 1 }, nts::dsp::OversamplingFactor::x8);
    float* baseChannel[] { base.data() }; float* highChannel[] { highQuality.data() };
    const auto transfer = [](float value) noexcept { return std::tanh(5.0f * value); };
    x1.process(baseChannel, 1, base.size(), transfer); x8.process(highChannel, 1, highQuality.size(), transfer);
    const auto x1Alias = toneMagnitude(std::span(base).subspan(256), 18000.0);
    const auto x8Alias = toneMagnitude(std::span(highQuality).subspan(256), 18000.0);
    tests.expect(x8Alias < x1Alias, "8x oversampling measurably rejects nonlinear aliasing");
}

void testDynamics(TestHarness& tests)
{
    nts::dsp::NoiseGate gate; gate.prepare(stereoSpec); nts::dsp::NoiseGateParameters gateParameters; gateParameters.thresholdDb = -30.0f; gate.setParameters(gateParameters);
    std::array<float, blockSize> quiet; quiet.fill(0.001f); std::array<float, blockSize> quietRight = quiet; float* gateChannels[] { quiet.data(), quietRight.data() };
    for (int block = 0; block < 100; ++block) gate.process(gateChannels, 2, blockSize);
    tests.expect(rms(quiet) < 0.0001, "noise gate closes below threshold");
    std::array<float, blockSize> loud; loud.fill(0.5f); std::array<float, blockSize> loudRight = loud; float* loudChannels[] { loud.data(), loudRight.data() };
    for (int block = 0; block < 20; ++block) gate.process(loudChannels, 2, blockSize);
    tests.expect(gate.state() == nts::dsp::GateState::open || gate.state() == nts::dsp::GateState::opening, "noise gate opens above threshold");

    nts::dsp::Compressor compressor; compressor.prepare(stereoSpec); nts::dsp::CompressorParameters compressorParameters;
    compressorParameters.thresholdDb = -20.0f; compressorParameters.ratio = 10.0f; compressorParameters.attackMs = 0.1; compressor.setParameters(compressorParameters);
    std::array<float, blockSize> compressed; compressed.fill(0.8f); std::array<float, blockSize> compressedRight = compressed; float* compressorChannels[] { compressed.data(), compressedRight.data() };
    for (int block = 0; block < 50; ++block) compressor.process(compressorChannels, 2, blockSize);
    tests.expect(compressor.gainReductionDb() > 5.0f && compressed.back() < 0.4f, "feed-forward compressor applies logarithmic gain reduction");

    nts::dsp::PeakLimiter limiter; limiter.prepare(stereoSpec); nts::dsp::LimiterParameters limiterParameters; limiterParameters.ceilingDb = -6.0f; limiterParameters.lookaheadMs = 2.0; limiter.setParameters(limiterParameters);
    std::array<float, blockSize> limited; limited.fill(2.0f); std::array<float, blockSize> limitedRight = limited; float* limiterChannels[] { limited.data(), limitedRight.data() };
    for (int block = 0; block < 4; ++block) limiter.process(limiterChannels, 2, blockSize);
    tests.expect(*std::max_element(limited.begin(), limited.end()) <= nts::dsp::dbToLinear(-6.0f) + 1.0e-5f, "peak limiter enforces ceiling");
    tests.expectEqual(limiter.latencySamples(), std::size_t { 96 }, "lookahead limiter reports latency");

    nts::dsp::Compressor automated; automated.prepare(monoSpec);
    nts::dsp::CompressorParameters automatedParameters; automatedParameters.thresholdDb = 12.0f;
    automatedParameters.ratio = 1.0f; automated.setParameters(automatedParameters); automated.reset();
    automatedParameters.makeupDb = 12.0f; automated.setParameters(automatedParameters);
    std::array<float, blockSize> automatedSignal; automatedSignal.fill(0.25f);
    float* automatedChannel[] { automatedSignal.data() }; automated.process(automatedChannel, 1, blockSize);
    tests.expect(automatedSignal.front() < 0.255f && automatedSignal.back() > automatedSignal.front(),
                 "dynamics parameter automation is internally smoothed");
}

void testModeSwitchAndSimd(TestHarness& tests)
{
    nts::dsp::ModeCrossfader crossfader; crossfader.prepare(monoSpec, 2);
    std::array<float, blockSize> signal; signal.fill(0.5f); float* channels[] { signal.data() };
    crossfader.requestMode(1, blockSize);
    crossfader.process(channels, 1, blockSize,
        [](std::size_t mode, float* const* buffers, std::size_t channelCount,
           std::size_t samples) noexcept
        {
            const auto gain = mode == 0 ? 1.0f : -1.0f;
            for (std::size_t channel = 0; channel < channelCount; ++channel)
                for (std::size_t sample = 0; sample < samples; ++sample)
                    buffers[channel][sample] *= gain;
        });
    float maximumStep {};
    for (std::size_t sample = 1; sample < signal.size(); ++sample)
        maximumStep = std::max(maximumStep, std::abs(signal[sample] - signal[sample - 1]));
    tests.expect(crossfader.mode() == 1 && ! crossfader.isCrossfading() && maximumStep < 0.02f,
                 "generic processor mode switch crossfades without a discontinuity");

    std::array<float, 259> scalar {};
    for (std::size_t index = 0; index < scalar.size(); ++index)
        scalar[index] = static_cast<float>(index) * 0.001f - 0.1f;
    auto simd = scalar;
    nts::dsp::multiplyGainScalar(scalar.data(), scalar.size(), 0.37f);
    nts::dsp::multiplyGainSimd(simd.data(), simd.size(), 0.37f);
    for (std::size_t index = 0; index < scalar.size(); ++index)
        tests.expectNear(simd[index], scalar[index], 1.0e-7, "SIMD gain kernel matches scalar reference");
    tests.expect(nts::dsp::simdAvailable(), "SSE2 SIMD kernel is available on the supported Windows target");
}

void testConvolutionAndIr(TestHarness& tests)
{
    const std::array<float, 4> ir { 1.0f, 0.5f, -0.25f, 0.125f };
    nts::dsp::DirectConvolver direct; direct.prepare(64, 2); tests.expect(direct.loadImpulse(ir), "direct convolver accepts mono IR");
    std::array<float, 16> left {}; std::array<float, 16> right {}; left[0] = right[0] = 1.0f; float* channels[] { left.data(), right.data() };
    direct.process(channels, 2, left.size());
    for (std::size_t index = 0; index < ir.size(); ++index) tests.expectNear(left[index], ir[index], 1.0e-6, "direct convolution matches reference impulse");

    auto longIr = nts::dsp::StimulusGenerator::transient(300, sampleRate);
    auto input = nts::dsp::StimulusGenerator::whiteNoise(1024, 1234); auto expected = input; auto actual = input;
    nts::dsp::DirectConvolver reference; reference.prepare(longIr.size(), 1); tests.expect(reference.loadImpulse(longIr), "direct reference accepts long IR"); float* expectedChannel[] { expected.data() }; reference.process(expectedChannel, 1, expected.size());
    nts::dsp::PartitionedConvolver partitioned; partitioned.prepare(blockSize, longIr.size(), 1); partitioned.loadImpulse(longIr);
    for (std::size_t offset = 0; offset < actual.size(); offset += blockSize) { float* block[] { actual.data() + offset }; partitioned.process(block, 1, blockSize); }
    const auto metrics = nts::dsp::compareAudio(expected, actual);
    tests.expect(metrics.maximumAbsoluteError < 2.0e-4, "partitioned convolution matches direct reference");

    nts::dsp::ImpulseResponse decoded; decoded.sampleRate = 24000.0; decoded.channels = { { 0.1f, 0.1f, 1.1f, 0.1f, 0.1f } };
    nts::dsp::ImpulsePreparationOptions options; options.outputChannels = 2; options.trimThresholdDb = -20.0f;
    const auto prepared = nts::dsp::prepareImpulseResponse(decoded, 48000.0, options);
    tests.expectEqual(prepared.channels.size(), std::size_t { 2 }, "mono IR converts to stereo");
    tests.expect(prepared.channels[0].size() > decoded.channels[0].size(), "IR resamples to engine sample rate");
    const auto peak = *std::max_element(prepared.channels[0].begin(), prepared.channels[0].end());
    tests.expectNear(peak, nts::dsp::dbToLinear(-1.0f), 1.0e-4, "IR normalization reaches requested peak");

    nts::dsp::CrossfadingConvolver swapping; swapping.prepare(blockSize, 128, 1); std::array<float, 1> delta { 1.0f };
    tests.expect(swapping.loadInactiveImpulse(delta), "inactive convolver prepares off audio thread"); swapping.requestSwap(blockSize);
    auto swapSignal = sine(blockSize, 500.0); float* swapChannel[] { swapSignal.data() }; swapping.process(swapChannel, 1, blockSize);
    float maximumStep {};
    for (std::size_t index = 1; index < swapSignal.size(); ++index) maximumStep = std::max(maximumStep, std::abs(swapSignal[index] - swapSignal[index - 1]));
    tests.expect(maximumStep < 0.2f && ! swapping.isCrossfading(), "IR response swap crossfades without a click");
}

void testAnalysisAndMetering(TestHarness& tests)
{
    nts::dsp::Fft fft; fft.prepare(256); std::vector<std::complex<float>> data(256);
    for (std::size_t index = 0; index < data.size(); ++index) data[index] = { static_cast<float>(std::sin(2.0 * std::numbers::pi * 7.0 * index / data.size())), 0.0f };
    const auto original = data; fft.transform(data); fft.transform(data, true);
    double maximumError {}; for (std::size_t index = 0; index < data.size(); ++index) maximumError = std::max(maximumError, static_cast<double>(std::abs(data[index] - original[index])));
    tests.expect(maximumError < 1.0e-5, "FFT inverse reconstructs complex input");

    nts::dsp::Stft stft; stft.prepare(256, 64, nts::dsp::WindowType::hann); auto signal = sine(4096, 440.0);
    const auto reconstructed = stft.reconstructOffline(signal); const auto reconstruction = nts::dsp::compareAudio(std::span(signal).subspan(256, 3500), std::span(reconstructed).subspan(256, 3500));
    tests.expect(reconstruction.maximumAbsoluteError < 1.0e-4, "overlapped STFT reconstructs offline signal");

    nts::dsp::MeterBank meters; meters.prepare(stereoSpec); std::array<float, blockSize> input {}; std::array<float, blockSize> output {};
    const float* inputChannels[] { input.data(), input.data() }; const float* outputChannels[] { output.data(), output.data() };
    std::size_t meterSample {};
    for (int block = 0; block < 3000; ++block)
    {
        for (std::size_t sample = 0; sample < blockSize; ++sample, ++meterSample)
            input[sample] = output[sample] = 0.5f * static_cast<float>(
                std::sin(2.0 * std::numbers::pi * 1000.0 * meterSample / sampleRate));
        meters.process(inputChannels, outputChannels, 2, blockSize, 3.0f);
    }
    const auto reading = meters.reading(0);
    const auto expectedPeak = *std::max_element(output.begin(), output.end());
    const auto expectedRms = rms(output);
    tests.expectNear(reading.samplePeak, expectedPeak, 1.0e-6, "sample peak meter is exact");
    tests.expectNear(reading.rms, expectedRms, 1.0e-6, "RMS meter is exact");
    tests.expectNear(reading.crestFactor, expectedPeak / expectedRms, 1.0e-6, "crest factor is reported");
    tests.expect(reading.shortTermLufs > -11.0f && reading.shortTermLufs < -8.0f
                 && reading.integratedLufs > -11.0f && reading.integratedLufs < -8.0f,
                 "BS.1770 K-weighted short-term and gated integrated loudness are calibrated");
    tests.expectNear(reading.gainReductionDb, 3.0, 1.0e-6, "gain reduction meter is transported");

    meters.reset(); input.fill(0.0f); output.fill(0.0f);
    for (int block = 0; block < 400; ++block)
        meters.process(inputChannels, outputChannels, 2, blockSize);
    tests.expect(meters.reading(0).integratedLufs <= -119.0f,
                 "BS.1770 absolute gate excludes silence from integrated loudness");

    nts::dsp::SpectrumAnalyzer spectrum; spectrum.prepare(1024, 256); auto spectrumSignal = sine(2048, 3000.0); spectrum.process(spectrumSignal);
    std::vector<float> magnitudes(spectrum.bins()); static_cast<void>(spectrum.copyMagnitudes(magnitudes)); const auto peakBin = static_cast<std::size_t>(std::distance(magnitudes.begin(), std::max_element(magnitudes.begin(), magnitudes.end())));
    tests.expect(std::abs(static_cast<double>(peakBin) * sampleRate / 1024.0 - 3000.0) < 60.0, "spectrum analyzer locates tone");
    nts::dsp::WaveformHistory history; history.reset(); history.push(spectrumSignal, 8); std::array<float, 64> waveform {}; tests.expectEqual(history.copyLatest(waveform), waveform.size(), "downsampled waveform history is lock-free readable");
}

void testRegressionSystem(TestHarness& tests)
{
    const auto whiteA = nts::dsp::StimulusGenerator::whiteNoise(1024, 42); const auto whiteB = nts::dsp::StimulusGenerator::whiteNoise(1024, 42);
    tests.expectEqual(nts::dsp::hashAudio(whiteA), nts::dsp::hashAudio(whiteB), "fixed-seed white noise regression hash is deterministic");
    const auto pink = nts::dsp::StimulusGenerator::pinkNoise(1024, 9); const auto guitar = nts::dsp::StimulusGenerator::guitarDi(1024, sampleRate);
    const auto bass = nts::dsp::StimulusGenerator::bassDi(1024, sampleRate); const auto palm = nts::dsp::StimulusGenerator::palmMute(1024, sampleRate);
    const auto transient = nts::dsp::StimulusGenerator::transient(1024, sampleRate); const double frequencies[] { 100.0, 1000.0, 5000.0 };
    const auto multi = nts::dsp::StimulusGenerator::multiTone(1024, sampleRate, frequencies); const auto sweep = nts::dsp::StimulusGenerator::sineSweep(1024, sampleRate, 20.0, 20000.0);
    tests.expect(! pink.empty() && ! guitar.empty() && ! bass.empty() && ! palm.empty() && ! transient.empty() && ! multi.empty() && ! sweep.empty(), "all deterministic regression stimuli are generated");
    std::vector<float> capturedGuitar, capturedBass;
    capturedGuitar.reserve(nts::test::fixtures::guitarDi.size()); capturedBass.reserve(nts::test::fixtures::bassDi.size());
    for (const auto sample : nts::test::fixtures::guitarDi) capturedGuitar.push_back(static_cast<float>(sample) / 32768.0f);
    for (const auto sample : nts::test::fixtures::bassDi) capturedBass.push_back(static_cast<float>(sample) / 32768.0f);
    tests.expectEqual(nts::dsp::hashAudio(capturedGuitar), std::uint64_t { 5341957500361645939ULL },
                      "checked-in guitar DI capture matches its fixed regression hash");
    tests.expectEqual(nts::dsp::hashAudio(capturedBass), std::uint64_t { 17574014129179321294ULL },
                      "checked-in bass DI capture matches its fixed regression hash");
    auto delayed = guitar; delayed.insert(delayed.begin(), 7, 0.0f); delayed.resize(guitar.size()); const auto metrics = nts::dsp::compareAudio(guitar, delayed);
    tests.expectEqual(metrics.latencyOffset, std::ptrdiff_t { 7 }, "audio regression finds latency offset"); tests.expect(metrics.maximumAbsoluteError > 0.0 && metrics.rmsError > 0.0 && metrics.spectralErrorDb > 0.0, "audio regression reports numeric errors");
}

void testNoRuntimeAllocations(TestHarness& tests)
{
    nts::dsp::Biquad filter; filter.prepare(stereoSpec); filter.setCoefficients(nts::dsp::BiquadCoefficients::make(nts::dsp::FilterType::lowPass, sampleRate, 2000.0));
    nts::dsp::Compressor compressor; compressor.prepare(stereoSpec);
    nts::dsp::Oversampler oversampler; oversampler.prepare(stereoSpec, nts::dsp::OversamplingFactor::x4);
    nts::dsp::PartitionedConvolver convolver; convolver.prepare(blockSize, 256, 2); std::array<float, 1> delta { 1.0f }; convolver.loadImpulse(delta);
    nts::dsp::SpectrumAnalyzer spectrum; spectrum.prepare(64, 64);
    nts::dsp::MeterBank meters; meters.prepare(stereoSpec);
    nts::dsp::ModeCrossfader crossfader; crossfader.prepare(stereoSpec, 2);
    std::array<float, blockSize> left; left.fill(0.1f); auto right = left; float* channels[] { left.data(), right.data() };
    allocationCount.store(0); countAllocations = true;
    filter.process(channels, 2, blockSize); compressor.process(channels, 2, blockSize);
    oversampler.process(channels, 2, blockSize, [](float value) noexcept { return std::tanh(value); });
    convolver.process(channels, 2, blockSize); spectrum.process(left);
    const float* constChannels[] { left.data(), right.data() }; meters.process(constChannels, constChannels, 2, blockSize);
    crossfader.requestMode(1, blockSize);
    crossfader.process(channels, 2, blockSize,
        [](std::size_t, float* const*, std::size_t, std::size_t) noexcept {});
    countAllocations = false;
    tests.expectEqual(allocationCount.load(), std::size_t { 0 }, "DSP processing performs no runtime allocation");
}
} // namespace

int main()
{
    TestHarness tests;
    testGainAndSmoothing(tests); testFilters(tests); testCrossover(tests);
    testNonlinearAndOversampling(tests); testDynamics(tests); testModeSwitchAndSimd(tests); testConvolutionAndIr(tests);
    testAnalysisAndMetering(tests); testRegressionSystem(tests); testNoRuntimeAllocations(tests);
    return tests.result();
}
