#include "TestHarness.h"
#include "fixtures/RecordedDiFixtures.h"

#include <nts/dsp/Analysis.h>
#include <nts/dsp/BassSplit.h>
#include <nts/dsp/Common.h>
#include <nts/dsp/Convolution.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Effects.h>
#include <nts/dsp/Filters.h>
#include <nts/dsp/Metering.h>
#include <nts/dsp/DelayLine.h>
#include <nts/dsp/ModeCrossfader.h>
#include <nts/dsp/Nonlinear.h>
#include <nts/dsp/Oversampling.h>
#include <nts/dsp/PitchDetector.h>
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
#include <random>
#include <string>
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

    // Round-trip through the resampler with nothing in between. Everything above checks the
    // designed coefficients or the gross aliasing behaviour, and neither would notice if the
    // polyphase up and down paths had drifted out of alignment with each other -- a phase
    // error, an off-by-one in the tap window, or a sub-filter assigned to the wrong phase all
    // leave the coefficients correct and the aliasing rejected. Passing a signal well inside
    // the passband through unchanged and landing it exactly on the reported latency is what
    // pins those down.
    for (const auto factor : { nts::dsp::OversamplingFactor::x2, nts::dsp::OversamplingFactor::x4,
                               nts::dsp::OversamplingFactor::x8 })
    {
        constexpr std::size_t length = 2048;
        auto reference = sine(length, 500.0, 0.5f);
        auto roundTrip = reference;
        nts::dsp::Oversampler unity;
        unity.prepare({ sampleRate, length, 1 }, factor);
        float* channel[] { roundTrip.data() };
        unity.process(channel, 1, length, [](float value) noexcept { return value; });

        const auto latency = unity.latencySamples();
        // Skip the filter's start-up transient before comparing.
        auto worst = 0.0f;
        for (std::size_t index = latency + 128; index < length; ++index)
            worst = std::max(worst, std::abs(roundTrip[index] - reference[index - latency]));
        tests.expect(worst < 0.02f,
                     "oversampling round trip reproduces a passband signal at the reported latency");
    }
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

    // Both dispatch paths, driven on whatever machine this is running on. Without forcing, a
    // developer machine new enough to select AVX2 never executes the SSE2 kernel again -- and the
    // SSE2 kernel is the one that has to keep working on the hardware this engine exists to
    // support. So the interesting assertion is not that AVX2 is fast, it is that the two agree.
    {
        constexpr std::size_t length = 1000;
        std::vector<float> left(length), right(length);
        std::uint32_t seed = 20260803u;
        const auto next = [&seed]
        {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
        };
        for (std::size_t index = 0; index < length; ++index) { left[index] = next(); right[index] = next(); }

        const auto restore = nts::dsp::activeSimdPath();
        // Every length from zero through the widest unrolled step, so the tails of both kernels
        // are exercised as well as their main loops. An off-by-one in a tail is exactly the kind
        // of thing that only shows up on the path the developer's machine does not take.
        auto worst = 0.0f;
        auto scalarWorst = 0.0f;
        for (std::size_t count = 0; count <= 40; ++count)
        {
            nts::dsp::setSimdPath(nts::dsp::SimdPath::sse2);
            const auto sse2 = nts::dsp::dotProductWide(left.data(), right.data(), count);
            nts::dsp::setSimdPath(nts::dsp::SimdPath::avx2);
            const auto avx2 = nts::dsp::dotProductWide(left.data(), right.data(), count);
            const auto reference = nts::dsp::dotProductScalar(left.data(), right.data(), count);
            worst = std::max(worst, std::abs(sse2 - avx2));
            scalarWorst = std::max({ scalarWorst, std::abs(sse2 - reference), std::abs(avx2 - reference) });
        }
        nts::dsp::setSimdPath(nts::dsp::SimdPath::sse2);
        const auto longSse2 = nts::dsp::dotProductWide(left.data(), right.data(), length);
        nts::dsp::setSimdPath(nts::dsp::SimdPath::avx2);
        const auto longAvx2 = nts::dsp::dotProductWide(left.data(), right.data(), length);
        worst = std::max(worst, std::abs(longSse2 - longAvx2));
        nts::dsp::setSimdPath(restore);

        // Not bit-equality: the AVX2 kernel fuses its multiply and add, which keeps more
        // precision than the SSE2 kernel's separate rounding, and the two reduce their partial
        // sums in a different order. Agreement to within float epsilon of the magnitude is the
        // honest requirement.
        tests.expect(worst < 1.0e-4f, "the SSE2 and AVX2 dot products agree across every length");
        tests.expect(scalarWorst < 1.0e-4f, "both vector dot products agree with a scalar reference");

        // The forcing hook must refuse a path the machine cannot run, or a test that forces AVX2
        // on an older CPU would be an illegal instruction rather than a failure.
        const auto forced = nts::dsp::setSimdPath(nts::dsp::SimdPath::avx2);
        tests.expect(nts::dsp::avx2Available() ? forced == nts::dsp::SimdPath::avx2
                                               : forced == nts::dsp::SimdPath::sse2,
                     "forcing AVX2 is refused on a machine that does not support it");
        nts::dsp::setSimdPath(nts::dsp::SimdPath::automatic);
        tests.expect(nts::dsp::activeSimdPath() == nts::dsp::SimdPath::automatic,
                     "the dispatch returns to automatic selection");
    }
}

void testConvolutionAndIr(TestHarness& tests)
{
    const std::array<float, 4> ir { 1.0f, 0.5f, -0.25f, 0.125f };
    nts::dsp::DirectConvolver direct; direct.prepare(64, 2); tests.expect(direct.loadImpulse(ir), "direct convolver accepts mono IR");
    std::array<float, 16> left {}; std::array<float, 16> right {}; left[0] = right[0] = 1.0f; float* channels[] { left.data(), right.data() };
    direct.process(channels, 2, left.size());
    for (std::size_t index = 0; index < ir.size(); ++index) tests.expectNear(left[index], ir[index], 1.0e-6, "direct convolution matches reference impulse");

    // An independent reference, because everything else here checks DirectConvolver against
    // itself or against a single impulse at position zero. Neither would catch a delay-line
    // wraparound, an off-by-one in the tap window, or a reversed-impulse misalignment -- and
    // the doubled-buffer layout this class uses is exactly the kind of rewrite that fails that
    // way. Irregular block sizes are the point: they walk the write position through every
    // phase of the ring rather than landing on the same offsets each call.
    {
        constexpr std::size_t irLength = 100, signalLength = 1000;
        std::vector<float> randomIr(irLength);
        std::vector<float> signal(signalLength);
        std::uint32_t seed = 987654321u;
        const auto nextRandom = [&seed]
        {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
        };
        for (auto& value : randomIr) value = nextRandom();
        for (auto& value : signal) value = nextRandom();

        // Naive convolution in double, computed straight from the definition.
        std::vector<float> reference(signalLength);
        for (std::size_t n = 0; n < signalLength; ++n)
        {
            double sum {};
            for (std::size_t k = 0; k < irLength && k <= n; ++k)
                sum += static_cast<double>(randomIr[k]) * static_cast<double>(signal[n - k]);
            reference[n] = static_cast<float>(sum);
        }

        auto produced = signal;
        nts::dsp::DirectConvolver irregular;
        irregular.prepare(irLength, 1);
        tests.expect(irregular.loadImpulse(randomIr), "direct convolver accepts the random reference IR");
        constexpr std::array<std::size_t, 8> blockPattern { 7, 13, 1, 64, 200, 3, 128, 33 };
        std::size_t offset {}, patternIndex {};
        while (offset < signalLength)
        {
            const auto count = std::min(blockPattern[patternIndex++ % blockPattern.size()],
                                        signalLength - offset);
            float* block[] { produced.data() + offset };
            irregular.process(block, 1, count);
            offset += count;
        }
        auto worstError = 0.0f;
        for (std::size_t index = 0; index < signalLength; ++index)
            worstError = std::max(worstError, std::abs(produced[index] - reference[index]));
        tests.expect(worstError < 1.0e-5f,
                     "direct convolution matches a naive reference across irregular block sizes");
    }

    // The buffered partitioned path, driven the way a host actually drives one. This is the test
    // the fixed-block trap made impossible, and it is the entire point of the wrapper: partitioned
    // convolution is asymptotically cheaper, and it was unreachable because nothing could promise
    // it a constant block size.
    {
        constexpr std::size_t irLength = 300, signalLength = 4096, partition = 128;
        auto bufferedIr = nts::dsp::StimulusGenerator::transient(irLength, sampleRate);
        auto signal = nts::dsp::StimulusGenerator::whiteNoise(signalLength, 4242);

        // Reference: the direct convolver, which is invariant to how the input is cut up.
        auto reference = signal;
        nts::dsp::DirectConvolver referenceConvolver;
        referenceConvolver.prepare(bufferedIr.size(), 1);
        referenceConvolver.loadImpulse(bufferedIr);
        float* referenceChannel[] { reference.data() };
        referenceConvolver.process(referenceChannel, 1, reference.size());

        auto buffered = signal;
        nts::dsp::BufferedCrossfadingConvolver wrapper;
        wrapper.prepare(partition, bufferedIr.size(), 1);
        tests.expect(wrapper.loadInactiveImpulse(bufferedIr), "buffered convolver accepts an IR");
        wrapper.requestSwap(1);
        tests.expectEqual(wrapper.latencySamples(), partition,
                          "the buffered convolver reports exactly one partition of latency");

        // Deliberately pathological: none of these is the partition size, several are shorter,
        // and one is longer, so the wrapper has to cross partition boundaries mid-call.
        constexpr std::array<std::size_t, 7> pattern { 63, 1, 512, 127, 3, 200, 33 };
        std::size_t offset {}, patternIndex {};
        while (offset < signalLength)
        {
            const auto count = std::min(pattern[patternIndex++ % pattern.size()], signalLength - offset);
            float* block[] { buffered.data() + offset };
            wrapper.process(block, 1, count);
            offset += count;
        }

        // Offset by the reported latency, and skipping the leading partition of priming silence.
        auto worst = 0.0f;
        for (std::size_t index = partition + 64; index < signalLength; ++index)
            worst = std::max(worst, std::abs(buffered[index] - reference[index - partition]));
        tests.expect(worst < 2.0e-4f,
                     "buffered partitioned convolution matches direct convolution at its "
                     "reported latency across irregular block sizes");

        auto leadingSilence = true;
        for (std::size_t index = 0; index < partition; ++index)
            leadingSilence = leadingSilence && std::abs(buffered[index]) < 1.0e-6f;
        tests.expect(leadingSilence, "the buffered convolver's latency is silence, not stale data");
    }

    auto longIr = nts::dsp::StimulusGenerator::transient(300, sampleRate);
    auto input = nts::dsp::StimulusGenerator::whiteNoise(1024, 1234); auto expected = input; auto actual = input;
    nts::dsp::DirectConvolver reference; reference.prepare(longIr.size(), 1); tests.expect(reference.loadImpulse(longIr), "direct reference accepts long IR"); float* expectedChannel[] { expected.data() }; reference.process(expectedChannel, 1, expected.size());
    nts::dsp::PartitionedConvolver partitioned; partitioned.prepare(blockSize, longIr.size(), 1); partitioned.loadImpulse(longIr);
    for (std::size_t offset = 0; offset < actual.size(); offset += blockSize) { float* block[] { actual.data() + offset }; partitioned.process(block, 1, blockSize); }
    const auto metrics = nts::dsp::compareAudio(expected, actual);
    tests.expect(metrics.maximumAbsoluteError < 2.0e-4, "partitioned convolution matches direct reference");

    // Fixed block size is a contract, not a suggestion: a short call would desynchronise the
    // spectrum history and overlap. It is refused, so the failure is silence rather than
    // plausible-looking wrong audio.
    nts::dsp::PartitionedConvolver strict; strict.prepare(64, 128, 1);
    std::array<float, 3> shortImpulse { 1.0f, 0.4f, -0.2f };
    tests.expect(strict.loadImpulse(shortImpulse), "partitioned convolver accepts an impulse");
    tests.expectEqual(strict.requiredBlockSize(), std::size_t { 64 },
                      "partitioned convolver reports the block size it requires");
    std::array<float, 64> shortBlock {}; shortBlock[0] = 1.0f;
    float* shortChannel[] { shortBlock.data() };
    strict.process(shortChannel, 1, 32);   // deliberately half a block
    auto untouched = true;
    for (std::size_t index = 1; index < shortBlock.size(); ++index)
        untouched = untouched && shortBlock[index] == 0.0f;
    tests.expect(untouched && shortBlock[0] == 1.0f,
                 "a mismatched block is refused rather than corrupting the convolver");

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

void testDelayLine(TestHarness& tests)
{
    nts::dsp::DelayLine delay; delay.prepare(64, 2);
    delay.setDelay(5);
    std::array<float, blockSize> left {}; left[0] = 1.0f; auto right = left;
    const float* input[] { left.data(), right.data() };
    float* output[] { left.data(), right.data() };
    // Aliasing input and output is the way it is used in the processor, so test it that way.
    delay.process(input, output, 2, blockSize);
    tests.expectNear(left[5], 1.0, 1.0e-7, "delay line places the impulse at the requested offset");
    tests.expectNear(left[0], 0.0, 1.0e-7, "delay line clears the samples ahead of the impulse");

    // Zero delay has to be a straight pass-through, which is the case the bypass mix hits
    // whenever the processing latency happens to be zero.
    nts::dsp::DelayLine passthrough; passthrough.prepare(64, 2); passthrough.setDelay(0);
    std::array<float, blockSize> flat; flat.fill(0.25f); auto flatRight = flat;
    const float* flatIn[] { flat.data(), flatRight.data() };
    float* flatOut[] { flat.data(), flatRight.data() };
    passthrough.process(flatIn, flatOut, 2, blockSize);
    tests.expectNear(flat[0], 0.25, 1.0e-7, "zero delay passes the input through unchanged");

    // Continuity across block boundaries: an impulse late in one block must emerge in the next.
    nts::dsp::DelayLine spanning; spanning.prepare(64, 1); spanning.setDelay(8);
    std::array<float, 4> first { 1.0f, 0.0f, 0.0f, 0.0f }; std::array<float, 4> rest {};
    const float* firstIn[] { first.data() }; float* firstOut[] { first.data() };
    spanning.process(firstIn, firstOut, 1, first.size());
    auto found = false;
    for (int block = 0; block < 3; ++block)
    {
        rest.fill(0.0f);
        const float* restIn[] { rest.data() }; float* restOut[] { rest.data() };
        spanning.process(restIn, restOut, 1, rest.size());
        for (std::size_t sample = 0; sample < rest.size(); ++sample)
            if (std::abs(rest[sample] - 1.0f) < 1.0e-6f)
                found = block == 1 && sample == 0;
    }
    tests.expect(found, "delay line carries a delayed impulse across block boundaries");

    nts::dsp::DelayLine clamped; clamped.prepare(16, 1); clamped.setDelay(1000);
    tests.expect(clamped.delay() <= 16, "delay line clamps a request beyond its capacity");
}

void testPitchDetection(TestHarness& tests)
{
    // Every open string a tuner has to handle, including the low B of a 5-string bass, which
    // is the case that decides whether the analysis window is long enough.
    struct Reference { const char* name; double frequency; int midi; };
    constexpr std::array<Reference, 8> strings { {
        { "B0 (5-string bass)", 30.868, 23 }, { "E1 (bass)", 41.203, 28 },
        { "A1 (bass)", 55.000, 33 },          { "E2 (guitar)", 82.407, 40 },
        { "A2", 110.000, 45 },                { "D3", 146.832, 50 },
        { "G3", 195.998, 55 },                { "E4", 329.628, 64 } } };

    nts::dsp::PitchDetector detector;
    const auto frameSamples = std::size_t { 8192 };
    detector.prepare(sampleRate, frameSamples, 28.0, 1400.0);

    auto allWithinACent = true;
    auto allNamedCorrectly = true;
    for (const auto& reference : strings)
    {
        // A plucked string is not a sine: harmonics are what make a naive autocorrelation
        // report an octave error, so the fixture has to contain them.
        std::vector<float> frame(frameSamples);
        for (std::size_t index = 0; index < frame.size(); ++index)
        {
            const auto t = static_cast<double>(index) / sampleRate;
            const auto phase = 2.0 * std::numbers::pi * reference.frequency * t;
            frame[index] = static_cast<float>(0.5 * std::sin(phase)
                                              + 0.25 * std::sin(2.0 * phase + 0.4)
                                              + 0.12 * std::sin(3.0 * phase + 1.1)
                                              + 0.06 * std::sin(4.0 * phase + 2.0));
        }
        const auto reading = detector.analyse(frame);
        if (! reading.voiced) { allWithinACent = false; allNamedCorrectly = false; continue; }

        const auto errorCents = 1200.0 * std::log2(reading.frequencyHz / reference.frequency);
        allWithinACent = allWithinACent && std::abs(errorCents) < 1.0;
        const auto note = nts::dsp::nearestNote(reading.frequencyHz);
        allNamedCorrectly = allNamedCorrectly && note.midiNote == reference.midi;
    }
    tests.expect(allWithinACent, "pitch detection is within one cent across the guitar and bass range");
    tests.expect(allNamedCorrectly, "pitch detection names the right note, without octave errors");

    // A tuner has to show which way to turn the peg, so the sign of the offset matters.
    std::vector<float> sharp(frameSamples);
    const auto detuned = 440.0 * std::pow(2.0, 15.0 / 1200.0);
    for (std::size_t index = 0; index < sharp.size(); ++index)
        sharp[index] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * detuned
                                                         * static_cast<double>(index) / sampleRate));
    const auto sharpReading = detector.analyse(sharp);
    const auto sharpNote = nts::dsp::nearestNote(sharpReading.frequencyHz);
    tests.expect(sharpNote.midiNote == 69, "a sharp A4 still reads as A4");
    tests.expectNear(sharpNote.cents, 15.0, 1.5, "cent offset reports how far sharp the note is");

    {
        // An adjustable concert A is the whole point of the reference control on the tuner page:
        // 440 Hz has to read as in tune at A=440 and audibly sharp against a lower reference, and
        // the note name must not move when only the reference does.
        const auto at440 = nts::dsp::nearestNote(440.0f, 440.0f);
        const auto at432 = nts::dsp::nearestNote(440.0f, 432.0f);
        tests.expect(at440.midiNote == 69 && at432.midiNote == 69,
                     "changing the reference renames nothing: 440 Hz is A4 either way");
        tests.expectNear(at440.cents, 0.0, 0.01, "440 Hz is in tune at A=440");
        // 1200 * log2(440/432) is 31.8 cents.
        tests.expectNear(at432.cents, 31.8, 0.3, "440 Hz reads sharp against a 432 Hz reference");
        // And the reference scales the whole grid, not just the octave it names: the low E of a
        // guitar has to move by the same ratio, or every string but the A would be tuned wrongly.
        const auto lowE432 = nts::dsp::nearestNote(82.41f * 432.0f / 440.0f, 432.0f);
        tests.expect(lowE432.midiNote == 40, "a reference-scaled low E is still E2");
        tests.expectNear(lowE432.cents, 0.0, 1.0,
                         "the reference scales every note, not only the A it is named for");
    }

    // Silence and noise must not produce a confident reading, or the display will chatter.
    std::vector<float> silence(frameSamples, 0.0f);
    tests.expect(! detector.analyse(silence).voiced, "silence produces no pitch reading");

    std::vector<float> noise(frameSamples);
    std::mt19937 generator(1234);
    std::uniform_real_distribution<float> spread(-0.3f, 0.3f);
    for (auto& sample : noise) sample = spread(generator);
    const auto noiseReading = detector.analyse(noise);
    tests.expect(! noiseReading.voiced || noiseReading.confidence < 0.6f,
                 "broadband noise does not produce a confident pitch reading");

    tests.expect(std::string(nts::dsp::noteName(40)) == "E2"
                 && std::string(nts::dsp::noteName(69)) == "A4"
                 && std::string(nts::dsp::noteName(23)) == "B0",
                 "note names match their MIDI numbers");
    tests.expect(std::string(nts::dsp::noteName(-1)) == "--", "an invalid note has a safe name");
}

void testTimeBasedEffects(TestHarness& tests)
{
    constexpr std::size_t frame = 8192;
    const nts::dsp::ProcessSpec fxSpec { sampleRate, frame, 2 };

    // --- Delay -----------------------------------------------------------------------
    nts::dsp::Delay delay;
    delay.prepare(fxSpec);
    nts::dsp::DelayParameters delayParameters;
    delayParameters.timeMs = 100.0f;      // 4800 samples at 48 kHz
    delayParameters.feedback = 0.0f;
    delayParameters.mix = 1.0f;
    delayParameters.dampingHz = 20000.0f; // out of the way, so the tap keeps its level
    delay.setParameters(delayParameters);
    delay.reset();

    std::vector<float> left(frame, 0.0f), right(frame, 0.0f);
    left[0] = 1.0f; right[0] = 1.0f;
    float* channels[] { left.data(), right.data() };
    delay.process(channels, 2, frame);

    auto peakIndex = std::size_t {};
    auto peak = 0.0f;
    for (std::size_t index = 1; index < frame; ++index)
        if (std::abs(left[index]) > peak) { peak = std::abs(left[index]); peakIndex = index; }
    tests.expect(peakIndex >= 4780 && peakIndex <= 4820,
                 "delay places its repeat at the requested time");
    tests.expectNear(left[0], 1.0, 1.0e-6, "delay leaves the dry signal at unity");

    // Feedback has to decay. A line that grows is the failure that destroys speakers.
    nts::dsp::Delay runaway;
    runaway.prepare(fxSpec);
    delayParameters.feedback = 0.95f;     // the maximum the clamp allows
    delayParameters.timeMs = 20.0f;
    runaway.setParameters(delayParameters);
    runaway.reset();
    auto largest = 0.0f;
    std::vector<float> burst(frame), burstRight(frame);
    for (int block = 0; block < 40; ++block)
    {
        std::fill(burst.begin(), burst.end(), 0.0f);
        std::fill(burstRight.begin(), burstRight.end(), 0.0f);
        if (block == 0) { burst[0] = 1.0f; burstRight[0] = 1.0f; }
        float* burstChannels[] { burst.data(), burstRight.data() };
        runaway.process(burstChannels, 2, frame);
        for (const auto value : burst) largest = std::max(largest, std::abs(value));
    }
    tests.expect(std::isfinite(largest) && largest < 25.0f,
                 "delay feedback decays rather than running away");

    // An out-of-range feedback request must be clamped, not trusted.
    delayParameters.feedback = 5.0f;
    runaway.setParameters(delayParameters);
    runaway.reset();
    largest = 0.0f;
    for (int block = 0; block < 40; ++block)
    {
        std::fill(burst.begin(), burst.end(), 0.0f);
        std::fill(burstRight.begin(), burstRight.end(), 0.0f);
        if (block == 0) { burst[0] = 1.0f; burstRight[0] = 1.0f; }
        float* burstChannels[] { burst.data(), burstRight.data() };
        runaway.process(burstChannels, 2, frame);
        for (const auto value : burst) largest = std::max(largest, std::abs(value));
    }
    tests.expect(std::isfinite(largest) && largest < 25.0f,
                 "delay clamps a feedback request above unity");

    // --- Reverb ----------------------------------------------------------------------
    nts::dsp::Reverb reverb;
    reverb.prepare(fxSpec);
    nts::dsp::ReverbParameters reverbParameters;
    reverbParameters.size = 0.8f; reverbParameters.damping = 0.3f;
    reverbParameters.mix = 0.5f; reverbParameters.lowCutHz = 100.0f;
    reverb.setParameters(reverbParameters);
    reverb.reset();

    std::vector<float> revLeft(frame, 0.0f), revRight(frame, 0.0f);
    revLeft[0] = 1.0f; revRight[0] = 1.0f;
    float* revChannels[] { revLeft.data(), revRight.data() };
    reverb.process(revChannels, 2, frame);

    // Energy well after the impulse is the tail; without one there is no reverb.
    double late {};
    for (std::size_t index = frame / 2; index < frame; ++index) late += revLeft[index] * revLeft[index];
    tests.expect(late > 1.0e-9, "reverb produces a tail after the impulse has passed");

    auto finiteTail = true;
    for (const auto value : revLeft) finiteTail = finiteTail && std::isfinite(value) && std::abs(value) < 10.0f;
    tests.expect(finiteTail, "reverb output stays finite and bounded");

    // The two sides must differ, or it is mono reverb in a stereo wrapper.
    auto identical = true;
    for (std::size_t index = 0; index < frame; ++index)
        identical = identical && std::abs(revLeft[index] - revRight[index]) < 1.0e-9f;
    tests.expect(! identical, "reverb decorrelates the two channels");

    // Zero mix has to be a true bypass: no tail, no filtering of the dry path.
    nts::dsp::Reverb silent;
    silent.prepare(fxSpec);
    reverbParameters.mix = 0.0f;
    silent.setParameters(reverbParameters);
    silent.reset();
    std::vector<float> flat(frame, 0.3f), flatRight(frame, 0.3f);
    float* flatChannels[] { flat.data(), flatRight.data() };
    silent.process(flatChannels, 2, frame);
    auto untouched = true;
    for (const auto value : flat) untouched = untouched && std::abs(value - 0.3f) < 1.0e-7f;
    tests.expect(untouched, "a reverb at zero mix leaves the signal completely alone");

    // Bringing a send back up after it has been parked. Skipping the wet path while it is
    // inaudible is worth the whole cost of the effect, but it leaves the delay lines holding
    // whatever was in them, and a send that replays seconds of stale audio on re-engagement is
    // worse than one that costs CPU. Both effects clear on the way back in; these assert it.
    for (auto* effect : { &silent })
    {
        // Drive it hard while parked at zero mix, so anything retained would be loud.
        reverbParameters.mix = 0.0f;
        effect->setParameters(reverbParameters);
        std::vector<float> loudLeft(frame, 0.0f), loudRight(frame, 0.0f);
        float* loudChannels[] { loudLeft.data(), loudRight.data() };
        for (std::size_t index = 0; index < frame; ++index)
            loudLeft[index] = loudRight[index] = 0.9f * std::sin(0.07f * static_cast<float>(index));
        effect->process(loudChannels, 2, frame);

        // Now bring it up with silence going in. A cleared reverb produces nothing.
        reverbParameters.mix = 1.0f;
        effect->setParameters(reverbParameters);
        std::vector<float> quietLeft(frame, 0.0f), quietRight(frame, 0.0f);
        float* quietChannels[] { quietLeft.data(), quietRight.data() };
        effect->process(quietChannels, 2, frame);
        auto loudest = 0.0f;
        for (const auto value : quietLeft) loudest = std::max(loudest, std::abs(value));
        tests.expect(loudest < 1.0e-6f,
                     "a reverb brought back up does not replay what it held while parked");
    }

    nts::dsp::Delay parked;
    parked.prepare(fxSpec);
    delayParameters.timeMs = 100.0f; delayParameters.feedback = 0.4f;
    delayParameters.mix = 0.0f; delayParameters.dampingHz = 20000.0f;
    parked.setParameters(delayParameters);
    parked.reset();
    std::vector<float> parkLeft(frame, 0.0f), parkRight(frame, 0.0f);
    float* parkChannels[] { parkLeft.data(), parkRight.data() };
    for (std::size_t index = 0; index < frame; ++index)
        parkLeft[index] = parkRight[index] = 0.9f * std::sin(0.05f * static_cast<float>(index));
    parked.process(parkChannels, 2, frame);
    auto dryUntouched = true;
    for (std::size_t index = 0; index < frame; ++index)
        dryUntouched = dryUntouched
            && std::abs(parkLeft[index] - 0.9f * std::sin(0.05f * static_cast<float>(index))) < 1.0e-6f;
    tests.expect(dryUntouched, "a delay at zero mix leaves the signal completely alone");

    delayParameters.mix = 1.0f;
    parked.setParameters(delayParameters);
    std::vector<float> silentLeft(frame, 0.0f), silentRight(frame, 0.0f);
    float* silentChannels[] { silentLeft.data(), silentRight.data() };
    parked.process(silentChannels, 2, frame);
    auto loudestRepeat = 0.0f;
    for (const auto value : silentLeft) loudestRepeat = std::max(loudestRepeat, std::abs(value));
    tests.expect(loudestRepeat < 1.0e-6f,
                 "a delay brought back up does not replay what it held while parked");

    // A long decay must still settle rather than sustaining forever.
    tests.expect(reverb.tailSamples() > 0 && delay.tailSamples() > 0,
                 "both effects report a tail for the host");
}

/** Integrating the clipping curve folds back measurably less than sampling it.

    The claim ADAA exists to make, tested the way aliasing is actually audible: drive a high
    sine hard enough that the clipper's odd harmonics run past Nyquist, then measure how much
    energy lands at frequencies that are *not* multiples of the input. A clipper's own harmonics
    are integer multiples of the fundamental; everything that folded back is not.

    The fundamental is deliberately *not* a submultiple of the sample rate. At exactly 4 kHz in
    48 kHz every harmonic folds back onto another multiple of 4 kHz -- the aliases land exactly
    on top of the real harmonics and the measurement reads zero however bad the aliasing is.
    4100 Hz has no such relationship, so the folded partials land between the harmonics where
    they can be seen. This is the whole reason the first version of this test passed nothing.

    Both sides run the *same curve at the same gain*, which is the only comparison that means
    anything. A first attempt at this compared a hard-clipped model against a soft-clipped one
    and was measuring the difference between the two curves, not the difference the integration
    makes.
*/
void testAntiderivativeAntiAliasing(TestHarness& tests)
{
    constexpr double sampleRate = 48000.0;
    constexpr double fundamental = 4100.0;
    constexpr std::size_t length = 4096;
    constexpr float drive = 8.0f;
    constexpr auto shape = nts::dsp::Waveshape::hardClip;

    std::vector<float> input(length);
    for (std::size_t n = 0; n < length; ++n)
        input[n] = 0.9f * static_cast<float>(
            std::sin(2.0 * std::numbers::pi * fundamental * static_cast<double>(n) / sampleRate));

    std::vector<float> sampled(length), integrated(length);
    nts::dsp::AntialiasedWaveshaper shaper;
    for (std::size_t n = 0; n < length; ++n)
    {
        sampled[n] = nts::dsp::shapeSample(input[n], shape, drive);
        integrated[n] = shaper.process(input[n], shape, drive, 0);
    }

    // Energy at every 100 Hz bin that is not within 5% of a harmonic of the fundamental.
    const auto aliasEnergy = [&](const std::vector<float>& signal)
    {
        auto total = 0.0;
        for (auto frequency = 300.0; frequency < 20000.0; frequency += 100.0)
        {
            const auto ratio = frequency / fundamental;
            if (std::abs(ratio - std::round(ratio)) < 0.05) continue;
            double real {}, imaginary {};
            for (std::size_t n = 0; n < signal.size(); ++n)
            {
                const auto phase = 2.0 * std::numbers::pi * frequency
                                 * static_cast<double>(n) / sampleRate;
                real += signal[n] * std::cos(phase);
                imaginary += signal[n] * std::sin(phase);
            }
            total += real * real + imaginary * imaginary;
        }
        return total;
    };

    const auto plain = aliasEnergy(sampled);
    const auto adaa = aliasEnergy(integrated);
    tests.expect(plain > 0.0, "the sampled clipper produces measurable foldback to compare against");
    tests.expect(adaa < plain * 0.5,
                 "integrating the clipping curve halves the aliasing at worst");

    // And it is still a clipper: the point is less foldback, not a different effect.
    auto bounded = true;
    for (const auto value : integrated) bounded = bounded && std::isfinite(value) && std::abs(value) <= 1.001f;
    tests.expect(bounded, "the integrated clipper stays bounded by the curve it integrates");

    // The singular case. Held DC has a zero denominator at every sample, and must produce the
    // curve's value rather than a division by zero.
    nts::dsp::AntialiasedWaveshaper held;
    auto finiteOnDc = true;
    for (int n = 0; n < 64; ++n)
        finiteOnDc = finiteOnDc && std::isfinite(held.process(0.3f, shape, drive, 0));
    tests.expect(finiteOnDc, "a held input does not divide by zero");
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
    nts::dsp::DelayLine delay; delay.prepare(2048, 2); delay.setDelay(128);
    nts::dsp::PitchDetector pitch; pitch.prepare(sampleRate, blockSize, 60.0, 1000.0);
    nts::dsp::Delay fxDelay; fxDelay.prepare(stereoSpec); fxDelay.setParameters({});
    nts::dsp::Reverb fxReverb; fxReverb.prepare(stereoSpec); fxReverb.setParameters({});
    std::array<float, blockSize> left; left.fill(0.1f); auto right = left; float* channels[] { left.data(), right.data() };
    allocationCount.store(0); countAllocations = true;
    filter.process(channels, 2, blockSize); compressor.process(channels, 2, blockSize);
    oversampler.process(channels, 2, blockSize, [](float value) noexcept { return std::tanh(value); });
    convolver.process(channels, 2, blockSize); spectrum.process(left);
    const float* constChannels[] { left.data(), right.data() }; meters.process(constChannels, constChannels, 2, blockSize);
    crossfader.requestMode(1, blockSize);
    crossfader.process(channels, 2, blockSize,
        [](std::size_t, float* const*, std::size_t, std::size_t) noexcept {});
    const float* delayIn[] { left.data(), right.data() }; delay.process(delayIn, channels, 2, blockSize);
    static_cast<void>(pitch.analyse(left));
    fxDelay.process(channels, 2, blockSize); fxReverb.process(channels, 2, blockSize);
    countAllocations = false;
    tests.expectEqual(allocationCount.load(), std::size_t { 0 }, "DSP processing performs no runtime allocation");
}
} // namespace

int main()
{
    TestHarness tests;
    testGainAndSmoothing(tests); testFilters(tests); testCrossover(tests);
    testNonlinearAndOversampling(tests); testDynamics(tests); testModeSwitchAndSimd(tests); testConvolutionAndIr(tests);
    testAnalysisAndMetering(tests); testRegressionSystem(tests); testDelayLine(tests); testPitchDetection(tests);
    testTimeBasedEffects(tests);
    testAntiderivativeAntiAliasing(tests);
    testNoRuntimeAllocations(tests);
    return tests.result();
}
