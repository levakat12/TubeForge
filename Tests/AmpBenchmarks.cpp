// End-to-end cost of the traditional amplifier, which the per-component numbers in
// DspBenchmarks cannot give: the chain's cost is dominated by how often its parameters are
// reconfigured and by which oversampling factor and cabinet length are in force, none of which
// a single filter or convolver in isolation shows.
//
// Emits the same CSV shape as DspBenchmarks so the two can sit side by side.

#include <nts/amp/TraditionalAmp.h>

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
    constexpr std::size_t warmups = 20, iterations = 200, batches = 3;
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
    // Median batch, so a scheduler hiccup in one batch does not become the reported figure.
    std::sort(batchStatistics.begin(), batchStatistics.end(),
              [](const auto& left, const auto& right) { return left.averageUs < right.averageUs; });
    return batchStatistics[batches / 2];
}

/// A cabinet response of the requested length, shaped like the built-in ones.
std::vector<float> makeImpulse(std::size_t length)
{
    std::vector<float> impulse(length);
    for (std::size_t index = 0; index < length; ++index)
    {
        const auto time = static_cast<float>(index);
        impulse[index] = (index == 0 ? 0.72f : 0.0f)
                       + 0.16f * std::exp(-time / 68.0f) * std::sin(0.31f * time);
    }
    return impulse;
}

void run(double sampleRate, std::size_t blockSize, int oversampling, std::size_t irLength,
         bool retuneEveryBlock)
{
    const nts::dsp::ProcessSpec spec { sampleRate, blockSize, 2 };
    nts::amp::TraditionalAmpProcessor amp;
    amp.prepare(spec);

    auto preset = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern,
                                               nts::amp::Instrument::guitar);
    for (auto& stage : preset.parameters.stages) stage.oversamplingFactor = oversampling;
    amp.setParametersImmediately(preset.parameters);
    const auto impulse = makeImpulse(irLength);
    amp.loadCabinetImpulse(0, impulse, {}, {}, 0);

    std::vector<float> left(blockSize), right(blockSize);
    float* channels[] { left.data(), right.data() };
    std::size_t phase {};

    auto parameters = preset.parameters;
    const auto statistics = benchmark([&]
    {
        for (std::size_t sample = 0; sample < blockSize; ++sample)
        {
            const auto value = 0.25f * std::sin(0.07f * static_cast<float>(phase + sample));
            left[sample] = right[sample] = value;
        }
        phase += blockSize;
        // The plug-in hands the amplifier a freshly built parameter struct every block. When
        // nothing has moved that struct is identical, which is the case the dirty check exists
        // for; the "retune" variant perturbs a control each block to measure the other side.
        if (retuneEveryBlock)
            parameters.toneStack.mid = 0.5f + 0.0001f * static_cast<float>(phase % 100);
        amp.setParameters(parameters);
        amp.process(channels, 2, blockSize);
    });

    const auto budgetUs = 1.0e6 * static_cast<double>(blockSize) / sampleRate;
    std::cout << "amp_os" << oversampling << "_ir" << irLength
              << (retuneEveryBlock ? "_retuning" : "_settled") << ','
              << sampleRate << ',' << blockSize << ",2," << std::fixed << std::setprecision(3)
              << statistics.averageUs << ',' << statistics.p99Us << ",0,"
              << amp.latencySamples() << ",1.000,"
              << statistics.averageUs * 100.0 / budgetUs << '\n';
}
/** The cabinet on its own, which is the number the shared stage made everybody's business.

    While the cabinet lived inside `AmpVoice`, only the traditional engine paid for it. It is a
    stage after all three engines now, so a neural capture and the physical circuit are buying a
    convolution they did not buy before -- and there is no honest way to talk about that trade
    without measuring it. Reported per slot arrangement, because the whole point of the idle-slot
    skip is that one engaged slot costs about half of two, and of the mute that a slot switched
    off costs nothing at all.
*/
void runCabinet(double sampleRate, std::size_t blockSize, std::size_t irLength,
                bool bothSlots, bool muted)
{
    const nts::dsp::ProcessSpec spec { sampleRate, blockSize, 2 };
    nts::amp::CabinetSection cabinet;
    cabinet.prepare(spec);
    const auto impulse = makeImpulse(irLength);
    cabinet.loadImpulseA(impulse, {}, {}, 0);
    cabinet.loadImpulseB(impulse, {}, {}, 0);

    nts::amp::CabinetParameters parameters;
    // Blend at the centre engages both convolvers; at the rail the idle one is skipped, which is
    // the saving being measured. Mute is the third case: a slot switched off must cost nothing
    // even while the blend is still asking for it.
    parameters.blend = bothSlots ? 0.5f : 0.0f;
    parameters.slots[1].mute = muted;
    cabinet.setParameters(parameters, 0);
    cabinet.reset();

    std::vector<float> left(blockSize), right(blockSize);
    float* channels[] { left.data(), right.data() };
    std::size_t phase {};
    const auto statistics = benchmark([&]
    {
        for (std::size_t sample = 0; sample < blockSize; ++sample)
        {
            const auto value = 0.25f * std::sin(0.07f * static_cast<float>(phase + sample));
            left[sample] = right[sample] = value;
        }
        phase += blockSize;
        cabinet.process(channels, 2, blockSize);
    });

    const auto budgetUs = 1.0e6 * static_cast<double>(blockSize) / sampleRate;
    std::cout << "cabinet_ir" << irLength
              << (muted ? "_bmuted" : bothSlots ? "_both" : "_single") << ','
              << sampleRate << ',' << blockSize << ",2," << std::fixed << std::setprecision(3)
              << statistics.averageUs << ',' << statistics.p99Us << ",0,"
              << cabinet.latencySamples() << ",1.000,"
              << statistics.averageUs * 100.0 / budgetUs << '\n';
}
} // namespace

int main()
{
    std::cout << "processor,sampleRate,blockSize,channels,averageUs,p99Us,memoryBytes,"
                 "latencySamples,simdSpeedup,callbackBudgetPercent\n";
    for (const auto blockSize : { std::size_t { 128 }, std::size_t { 256 } })
        for (const auto oversampling : { 1, 2, 4, 8 })
            for (const auto irLength : { std::size_t { 384 }, std::size_t { 4096 } })
                for (const auto retuning : { false, true })
                    run(48000.0, blockSize, oversampling, irLength, retuning);
    // The cabinet stage alone, at the three lengths that matter: the built-in model's 512 taps,
    // the standard tier's 1024 ceiling, and the 4096 a long user response reaches.
    for (const auto blockSize : { std::size_t { 128 }, std::size_t { 256 } })
        for (const auto irLength : { std::size_t { 512 }, std::size_t { 1024 }, std::size_t { 4096 } })
        {
            runCabinet(48000.0, blockSize, irLength, false, false);
            runCabinet(48000.0, blockSize, irLength, true, false);
            runCabinet(48000.0, blockSize, irLength, true, true);
        }
    return 0;
}
