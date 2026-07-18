#include <nts/dsp/Analysis.h>
#include <nts/dsp/BassSplit.h>
#include <nts/dsp/Convolution.h>
#include <nts/dsp/Dynamics.h>
#include <nts/dsp/Filters.h>
#include <nts/dsp/Metering.h>
#include <nts/dsp/ModeCrossfader.h>
#include <nts/dsp/Nonlinear.h>
#include <nts/dsp/Oversampling.h>
#include <nts/dsp/Simd.h>
#include <nts/dsp/Smoothing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace
{
struct Configuration { double sampleRate; std::size_t blockSize; };
struct Statistics { double averageUs {}; double p99Us {}; };

template <typename Callback>
Statistics benchmark(Callback&& callback)
{
    constexpr std::size_t warmups = 40;
    constexpr std::size_t iterations = 400;
    for (std::size_t iteration = 0; iteration < warmups; ++iteration)
        callback();
    std::vector<double> timings; timings.reserve(iterations);
    for (std::size_t iteration = 0; iteration < iterations; ++iteration)
    {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto started = std::chrono::steady_clock::now();
        callback();
        const auto finished = std::chrono::steady_clock::now();
        std::atomic_signal_fence(std::memory_order_seq_cst);
        timings.push_back(std::chrono::duration<double, std::micro>(finished - started).count());
    }
    const auto sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    std::sort(timings.begin(), timings.end());
    return { sum / static_cast<double>(timings.size()),
             timings[static_cast<std::size_t>(timings.size() * 0.99)] };
}

void emit(const std::string& processor, const Configuration& configuration,
          std::size_t channels, std::size_t memoryBytes, std::size_t latency,
          Statistics statistics, double simdSpeedup = 1.0)
{
    const auto callbackBudgetUs = 1.0e6 * static_cast<double>(configuration.blockSize)
                                / configuration.sampleRate;
    std::cout << processor << ',' << configuration.sampleRate << ',' << configuration.blockSize << ','
              << channels << ',' << std::fixed << std::setprecision(3) << statistics.averageUs << ','
              << statistics.p99Us << ',' << memoryBytes << ',' << latency << ',' << simdSpeedup << ','
              << statistics.averageUs * 100.0 / callbackBudgetUs << '\n';
}

template <typename Callback>
Statistics measureAndEmit(const std::string& processor, const Configuration& configuration,
                          std::size_t channels, std::size_t memoryBytes, std::size_t latency,
                          Callback&& callback, double simdSpeedup = 1.0)
{
    const auto statistics = benchmark(std::forward<Callback>(callback));
    emit(processor, configuration, channels, memoryBytes, latency, statistics, simdSpeedup);
    return statistics;
}
} // namespace

int main()
{
    std::cout << "processor,sampleRate,blockSize,channels,averageUs,p99Us,memoryBytes,latencySamples,simdSpeedup,callbackBudgetPercent\n";
    const Configuration configurations[] { { 44100.0, 64 }, { 48000.0, 128 }, { 96000.0, 64 } };
    bool meteringWithinBudget = true;
    bool simdBenefitObserved = nts::dsp::simdAvailable();
    for (const auto configuration : configurations)
        for (const auto channels : { std::size_t { 1 }, std::size_t { 2 } })
        {
            using namespace nts::dsp;
            const ProcessSpec spec { configuration.sampleRate, configuration.blockSize, channels };
            std::vector<float> left(configuration.blockSize, 0.1f), right(configuration.blockSize, -0.1f);
            std::vector<float> sourceLeft(configuration.blockSize, 0.1f), sourceRight(configuration.blockSize, -0.1f);
            float* pointers[] { left.data(), right.data() };
            const float* constPointers[] { left.data(), right.data() };
            const float* inputPointers[] { sourceLeft.data(), sourceRight.data() };

            std::array<SmoothedGain, maximumChannels> gains;
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                gains[channel].prepare(configuration.sampleRate);
                gains[channel].resetDb(0.0f); gains[channel].setTargetDb(-3.0f);
            }
            measureAndEmit("smoothed_gain", configuration, channels, sizeof(gains), 0, [&]
            {
                for (std::size_t channel = 0; channel < channels; ++channel)
                    gains[channel].process(pointers[channel], configuration.blockSize);
            });

            OnePoleFilter onePole; onePole.prepare(spec); onePole.setCutoff(OnePoleFilter::Type::highPass, 70.0);
            measureAndEmit("one_pole", configuration, channels, sizeof(onePole), 0,
                           [&] { onePole.process(pointers, channels, configuration.blockSize); });

            Biquad biquad; biquad.prepare(spec); biquad.setCoefficients(
                BiquadCoefficients::make(FilterType::peaking, configuration.sampleRate, 1200.0, 0.8, 4.0));
            measureAndEmit("biquad", configuration, channels, sizeof(biquad), 0,
                           [&] { biquad.process(pointers, channels, configuration.blockSize); });

            StateVariableFilter svf; svf.prepare(spec); svf.setParameters(1800.0, 0.8, StateVariableOutput::lowPass);
            measureAndEmit("state_variable_filter", configuration, channels, sizeof(svf), 0,
                           [&] { svf.process(pointers, channels, configuration.blockSize); });

            LinkwitzRileyCrossover crossover; crossover.prepare(spec); crossover.setFrequency(240.0);
            std::vector<float> low(configuration.blockSize * channels), high(configuration.blockSize * channels);
            std::array<float*, maximumChannels> lowPointers {}, highPointers {};
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                lowPointers[channel] = low.data() + channel * configuration.blockSize;
                highPointers[channel] = high.data() + channel * configuration.blockSize;
            }
            measureAndEmit("linkwitz_riley", configuration, channels,
                           sizeof(crossover) + (low.size() + high.size()) * sizeof(float), 0, [&]
            {
                crossover.process(inputPointers, lowPointers.data(), highPointers.data(), channels,
                                  configuration.blockSize);
            });

            BiasableWaveshaper waveshaper; waveshaper.setDrive(4.0f); waveshaper.setBias(0.1f);
            measureAndEmit("waveshaper", configuration, channels, sizeof(waveshaper), 0, [&]
            {
                for (std::size_t channel = 0; channel < channels; ++channel)
                    waveshaper.process(pointers[channel], configuration.blockSize);
            });

            DynamicWaveshaper dynamicWaveshaper; dynamicWaveshaper.prepare(spec);
            dynamicWaveshaper.setParameters(3.0f, 0.5f, 2.0, 80.0);
            measureAndEmit("dynamic_waveshaper", configuration, channels, sizeof(dynamicWaveshaper), 0,
                           [&] { dynamicWaveshaper.process(pointers, channels, configuration.blockSize); });

            NoiseGate gate; gate.prepare(spec);
            measureAndEmit("noise_gate", configuration, channels, sizeof(gate), 0,
                           [&] { gate.process(pointers, channels, configuration.blockSize); });

            Compressor compressor; compressor.prepare(spec);
            measureAndEmit("compressor", configuration, channels, sizeof(compressor), 0,
                           [&] { compressor.process(pointers, channels, configuration.blockSize); });

            PeakLimiter limiter; limiter.prepare(spec);
            measureAndEmit("peak_limiter", configuration, channels,
                           sizeof(limiter) + (static_cast<std::size_t>(configuration.sampleRate * 0.02) + 1)
                                           * maximumChannels * sizeof(float), limiter.latencySamples(),
                           [&] { limiter.process(pointers, channels, configuration.blockSize); });

            for (const auto factor : { OversamplingFactor::x1, OversamplingFactor::x2,
                                       OversamplingFactor::x4, OversamplingFactor::x8 })
            {
                Oversampler oversampler; oversampler.prepare(spec, factor);
                const auto numericFactor = static_cast<std::size_t>(factor);
                const auto name = std::string("oversampler") + std::to_string(numericFactor) + "x";
                const auto taps = numericFactor == 1 ? std::size_t {} : 8 * numericFactor + 1;
                const auto memory = sizeof(oversampler) + taps * sizeof(float)
                    + channels * (configuration.blockSize * numericFactor + taps + 9) * sizeof(float);
                measureAndEmit(name, configuration, channels, memory, oversampler.latencySamples(), [&]
                {
                    oversampler.process(pointers, channels, configuration.blockSize,
                                        [](float value) noexcept { return std::tanh(3.0f * value); });
                });
            }

            std::vector<float> shortIr(256);
            for (std::size_t index = 0; index < shortIr.size(); ++index)
                shortIr[index] = static_cast<float>(std::exp(-0.025 * static_cast<double>(index)));
            DirectConvolver direct; direct.prepare(shortIr.size(), channels); direct.loadImpulse(shortIr);
            measureAndEmit("direct_convolver", configuration, channels,
                           sizeof(direct) + shortIr.size() * channels * 2 * sizeof(float), 0,
                           [&] { direct.process(pointers, channels, configuration.blockSize); });

            std::vector<float> longIr(2048);
            for (std::size_t index = 0; index < longIr.size(); ++index)
                longIr[index] = static_cast<float>(std::exp(-0.004 * static_cast<double>(index)));
            PartitionedConvolver partitioned; partitioned.prepare(configuration.blockSize, longIr.size(), channels);
            partitioned.loadImpulse(longIr);
            measureAndEmit("partitioned_convolver", configuration, channels,
                           sizeof(partitioned) + longIr.size() * channels * 8 * sizeof(float), 0,
                           [&] { partitioned.process(pointers, channels, configuration.blockSize); });

            CrossfadingConvolver convolverSwitch; convolverSwitch.prepare(configuration.blockSize, 256, channels);
            convolverSwitch.loadInactiveImpulse(shortIr); convolverSwitch.requestSwap(configuration.blockSize);
            convolverSwitch.process(pointers, channels, configuration.blockSize);
            measureAndEmit("crossfading_convolver", configuration, channels,
                           sizeof(convolverSwitch) + configuration.blockSize * channels * 2 * sizeof(float), 0,
                           [&] { convolverSwitch.process(pointers, channels, configuration.blockSize); });

            BassSplitProcessor bassSplit; bassSplit.prepare(spec); bassSplit.setCrossoverFrequency(250.0);
            CompressorParameters bassCompression; bassCompression.ratio = 3.0f;
            bassSplit.setLowBandCompression(true, bassCompression);
            bassSplit.setHighBandWaveshaping(true, Waveshape::hyperbolicTangent, 2.5f);
            measureAndEmit("bass_split", configuration, channels,
                           sizeof(bassSplit) + configuration.blockSize * channels * 2 * sizeof(float), 0,
                           [&] { bassSplit.process(pointers, channels, configuration.blockSize); });

            MeterBank meters; meters.prepare(spec);
            const auto meterMemory = sizeof(meters)
                + static_cast<std::size_t>(configuration.sampleRate * 3.4) * maximumChannels * sizeof(double);
            const auto baseline = measureAndEmit("metering_baseline", configuration, channels, 0, 0, [&]
            {
                std::copy(sourceLeft.begin(), sourceLeft.end(), left.begin());
                if (channels == 2) std::copy(sourceRight.begin(), sourceRight.end(), right.begin());
            });
            const auto meterStatistics = measureAndEmit("meter_bank", configuration, channels, meterMemory, 0,
                [&] { meters.process(constPointers, constPointers, channels, configuration.blockSize); });
            const Statistics meteringOverhead {
                std::max(0.0, meterStatistics.averageUs - baseline.averageUs),
                std::max(0.0, meterStatistics.p99Us - baseline.p99Us) };
            emit("metering_overhead", configuration, channels, meterMemory, 0, meteringOverhead);
            const auto callbackBudgetUs = 1.0e6 * static_cast<double>(configuration.blockSize)
                                        / configuration.sampleRate;
            meteringWithinBudget = meteringWithinBudget
                && meteringOverhead.averageUs / callbackBudgetUs < 0.01;

            SpectrumAnalyzer spectrum; spectrum.prepare(1024, configuration.blockSize);
            measureAndEmit("spectrum_analyzer", configuration, 1,
                           sizeof(spectrum) + 1024 * (sizeof(float) * 2 + sizeof(std::complex<float>)), 0,
                           [&] { spectrum.process(std::span<const float>(left.data(), configuration.blockSize)); });

            WaveformHistory waveform; waveform.reset();
            measureAndEmit("waveform_history", configuration, 1, sizeof(waveform), 0,
                           [&] { waveform.push(std::span<const float>(left.data(), configuration.blockSize), 4); });

            ModeCrossfader modeSwitch; modeSwitch.prepare(spec, 2); std::size_t nextMode { 1 };
            measureAndEmit("mode_crossfader", configuration, channels,
                           sizeof(modeSwitch) + configuration.blockSize * channels * 2 * sizeof(float), 0, [&]
            {
                modeSwitch.requestMode(nextMode, configuration.blockSize); nextMode = 1 - nextMode;
                modeSwitch.process(pointers, channels, configuration.blockSize,
                    [](std::size_t mode, float* const* buffers, std::size_t count, std::size_t samples) noexcept
                    {
                        const auto gain = mode == 0 ? 0.9f : 1.1f;
                        for (std::size_t channel = 0; channel < count; ++channel)
                            multiplyGainSimd(buffers[channel], samples, gain);
                    });
            });

            const auto scalarStatistics = benchmark([&]
            {
                for (std::size_t channel = 0; channel < channels; ++channel)
                    multiplyGainScalar(pointers[channel], configuration.blockSize, 1.000001f);
            });
            const auto simdStatistics = benchmark([&]
            {
                for (std::size_t channel = 0; channel < channels; ++channel)
                    multiplyGainSimd(pointers[channel], configuration.blockSize, 1.000001f);
            });
            emit("gain_scalar", configuration, channels, 0, 0, scalarStatistics);
            const auto simdSpeedup = scalarStatistics.averageUs / std::max(1.0e-9, simdStatistics.averageUs);
            emit("gain_simd", configuration, channels, 0, 0, simdStatistics, simdSpeedup);
            simdBenefitObserved = simdBenefitObserved && simdSpeedup > 1.05;
        }
    return meteringWithinBudget && simdBenefitObserved ? 0 : 1;
}
