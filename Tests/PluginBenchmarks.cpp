// The figure that actually decides whether the plug-in runs on a given machine: what fraction of
// the callback deadline one instance consumes, driving the real processBlock.
//
// The component benchmarks in DspBenchmarks and MlRuntimeBenchmarks measure kernels in isolation,
// and AmpBenchmarks measures the amplifier alone. None of them include the shared front end, the
// pedal board, the gate, the effects, the metering, or the display feeds -- which between them are
// most of what a block costs once the engine itself has been optimised.
//
// Emits the same CSV shape as the other benchmark tools so the four can sit side by side.

#include "PluginProcessor.h"

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
struct Statistics { double averageUs {}; double p99Us {}; };

template <typename Callback>
Statistics benchmark(Callback&& callback)
{
    constexpr std::size_t warmups = 30, iterations = 250, batches = 3;
    for (std::size_t iteration = 0; iteration < warmups; ++iteration) callback();
    std::array<Statistics, batches> batchStatistics {};
    for (std::size_t batch = 0; batch < batches; ++batch)
    {
        std::vector<double> timings; timings.reserve(iterations);
        for (std::size_t iteration = 0; iteration < iterations; ++iteration)
        {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const auto started = std::chrono::steady_clock::now(); callback();
            const auto finished = std::chrono::steady_clock::now();
            std::atomic_signal_fence(std::memory_order_seq_cst);
            timings.push_back(std::chrono::duration<double, std::micro>(finished - started).count());
        }
        const auto sum = std::accumulate(timings.begin(), timings.end(), 0.0);
        std::sort(timings.begin(), timings.end());
        batchStatistics[batch] = { sum / static_cast<double>(timings.size()),
                                   timings[static_cast<std::size_t>(timings.size() * 0.99)] };
    }
    // Median batch, so one scheduler hiccup does not become the reported figure.
    std::sort(batchStatistics.begin(), batchStatistics.end(),
              [](const auto& left, const auto& right) { return left.averageUs < right.averageUs; });
    return batchStatistics[batches / 2];
}

void setParameter(TubeForgeAudioProcessor& processor, const char* id, float value)
{
    if (auto* parameter = processor.getParameters().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

struct Scenario
{
    const char* name;
    int engineMode;      // 0 traditional, 1 neural, 2 physical circuit
    int oversampling;    // index into the oversampling parameter
    bool editorOpen;
    bool effects;
    int tier { 1 };      // 0 Eco, 1 Standard, 2 Studio
};

void run(const Scenario& scenario, int blockSize, double sampleRate)
{
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);
    processor.setEditorActive(scenario.editorOpen);

    setParameter(processor, "performanceTier", static_cast<float>(scenario.tier));
    setParameter(processor, "engineMode", static_cast<float>(scenario.engineMode));
    setParameter(processor, "oversampling", static_cast<float>(scenario.oversampling));
    // Sends either engaged or parked, which is what decides whether they cost anything at all.
    setParameter(processor, "delayMix", scenario.effects ? 30.0f : 0.0f);
    setParameter(processor, "reverbMix", scenario.effects ? 30.0f : 0.0f);
    // Both cabinets contributing, so the tier's single-cabinet rule has something to remove.
    setParameter(processor, "cabinetBlend", 50.0f);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    int phase {};

    // The engine-mode change is a fade, so let it settle before timing anything.
    for (int block = 0; block < 64; ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }

    const auto statistics = benchmark([&]
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = 0.25f * std::sin(0.07f * static_cast<float>(phase + sample))
                             + 0.08f * std::sin(0.31f * static_cast<float>(phase + sample));
            buffer.setSample(0, sample, value);
            buffer.setSample(1, sample, value);
        }
        phase += blockSize;
        processor.processBlock(buffer, midi);
    });

    const auto budgetUs = 1.0e6 * static_cast<double>(blockSize) / sampleRate;
    std::cout << scenario.name << ',' << sampleRate << ',' << blockSize << ",2,"
              << std::fixed << std::setprecision(3) << statistics.averageUs << ','
              << statistics.p99Us << ",0," << processor.getLatencySamples() << ",1.000,"
              << statistics.averageUs * 100.0 / budgetUs << '\n';
}
} // namespace

int main()
{
    // JUCE parameter and message machinery needs an initialised event loop even headless.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    std::cout << "processor,sampleRate,blockSize,channels,averageUs,p99Us,memoryBytes,"
                 "latencySamples,simdSpeedup,callbackBudgetPercent\n";

    const std::array scenarios {
        // Oversampling index 2 is 4x, the default the auto setting lands on when driven.
        Scenario { "traditional_os4_editor_fx",     0, 2, true,  true },
        Scenario { "traditional_os4_editor_nofx",   0, 2, true,  false },
        Scenario { "traditional_os4_closed_nofx",   0, 2, false, false },
        Scenario { "traditional_os1_closed_nofx",   0, 0, false, false },
        Scenario { "traditional_os8_closed_nofx",   0, 3, false, false },
        // The neural and circuit engines with no model or graph loaded still cost their
        // surrounding chain, which is what these two isolate.
        Scenario { "neural_closed_nofx",            1, 2, false, false },
        Scenario { "circuit_closed_nofx",           2, 2, false, false },
        // The tier comparison: the same request -- 8x oversampling, both cabinets, effects up,
        // editor open -- served at each of the three settings.
        Scenario { "tier_eco_worstcase",            0, 3, true,  true,  0 },
        Scenario { "tier_standard_worstcase",       0, 3, true,  true,  1 },
        Scenario { "tier_studio_worstcase",         0, 3, true,  true,  2 },
    };

    for (const auto blockSize : { 64, 128, 256, 512 })
        for (const auto& scenario : scenarios)
            run(scenario, blockSize, 48000.0);
    return 0;
}
