#include "PackedFixtures.h"
#include <nts/ml/NeuralAmpProcessor.h>
#include <nts/ml/PackedWaveNetModel.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
void appendU32(std::vector<std::byte>& bytes, std::uint32_t value)
{ const auto offset = bytes.size(); bytes.resize(offset + sizeof(value)); std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
void appendFloat(std::vector<std::byte>& bytes, float value)
{ const auto offset = bytes.size(); bytes.resize(offset + sizeof(value)); std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
std::vector<std::byte> recurrentFixture(std::size_t hidden)
{
    std::vector<std::byte> bytes { std::byte{'N'}, std::byte{'T'}, std::byte{'S'}, std::byte{'M'} };
    for (const auto value : { 1u, 1u, 48000u, 1u, static_cast<std::uint32_t>(hidden), 1u }) appendU32(bytes, value);
    for (std::size_t row = 0; row < hidden; ++row) appendFloat(bytes, 0.02f);
    for (std::size_t row = 0; row < hidden; ++row)
        for (std::size_t column = 0; column < hidden; ++column) appendFloat(bytes, row == column ? 0.15f : 0.0f);
    for (std::size_t row = 0; row < hidden; ++row) appendFloat(bytes, 0.0f);
    for (std::size_t row = 0; row < hidden; ++row) appendFloat(bytes, 0.01f);
    appendFloat(bytes, 0.0f); return bytes;
}
}

int main()
{
#if defined(_WIN32)
    SYSTEM_INFO systemInfo {};
    GetSystemInfo(&systemInfo);
    const auto processorIndex = std::min<DWORD>(systemInfo.dwNumberOfProcessors > 0
        ? systemInfo.dwNumberOfProcessors - 1 : 0, static_cast<DWORD>(sizeof(DWORD_PTR) * 8 - 1));
    (void) SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    (void) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR { 1 } << processorIndex);
#endif
    constexpr std::size_t blockSize = 64, iterations = 2000, batches = 5;
    const auto bytes = recurrentFixture(64);
    nts::ml::PackedTanhModel reference; std::string error;
    if (! reference.load(bytes, error)) { std::cerr << error << '\n'; return 1; }
    std::array<float, blockSize> testInput {}, expected {};
    for (std::size_t index = 0; index < blockSize; ++index) testInput[index] = 0.08f * std::sin(static_cast<float>(index) * 0.17f);
    reference.process(testInput, expected);
    for (const auto channels : { std::size_t { 1 }, std::size_t { 2 } })
    {
        nts::ml::NeuralAmpProcessor processor; processor.prepare(48000.0, blockSize, channels);
        if (! processor.stageModel(bytes, testInput, expected, 1.0e-6f, -21.0f, error)) { std::cerr << error << '\n'; return 2; }
        std::array<std::array<float, blockSize>, 2> blocks {};
        std::array<float*, 2> pointers { blocks[0].data(), blocks[1].data() };
        for (std::size_t iteration = 0; iteration < 256; ++iteration)
        {
            for (std::size_t channel = 0; channel < channels; ++channel) blocks[channel] = testInput;
            processor.process(pointers.data(), channels, blockSize);
        }
        std::array<double, batches> averages {}, percentiles {};
        for (std::size_t batch = 0; batch < batches; ++batch)
        {
            std::vector<double> timings; timings.reserve(iterations);
            for (std::size_t iteration = 0; iteration < iterations; ++iteration)
            {
                for (std::size_t channel = 0; channel < channels; ++channel) blocks[channel] = testInput;
                const auto start = std::chrono::steady_clock::now(); processor.process(pointers.data(), channels, blockSize);
                const auto end = std::chrono::steady_clock::now();
                timings.push_back(std::chrono::duration<double, std::micro>(end - start).count());
            }
            std::sort(timings.begin(), timings.end());
            averages[batch] = std::accumulate(timings.begin(), timings.end(), 0.0) / timings.size();
            percentiles[batch] = timings[static_cast<std::size_t>(0.99 * static_cast<double>(timings.size() - 1))];
        }
        std::sort(averages.begin(), averages.end()); std::sort(percentiles.begin(), percentiles.end());
        const auto average = averages[batches / 2], p99 = percentiles[batches / 2];
        const auto budgetUs = 1.0e6 * static_cast<double>(blockSize) / 48000.0;
        std::cout << "recurrent64,48000,64," << channels << ',' << average << ',' << p99 << ','
                  << processor.modelMemoryBytes() << ',' << p99 / budgetUs * 100.0 << "\n";
        const auto targetFraction = channels == 1 ? 0.5 : 0.75;
        if (p99 >= budgetUs * targetFraction) return 3;
    }

    // The NAM corpus geometry: 23 layers, kernels of 6 with two of 15, dilations cycling to 239.
    // The lite tier (3 channels) is the one held to a budget here because it is the tier a machine
    // is expected to run several of; the standard tier is reported for reference. Measured on the
    // development machine at 17.1% (standard) and 5.5% (lite) of one core, scalar and unoptimised.
    for (const auto channels : { std::size_t { 3 }, std::size_t { 8 } })
    {
        const auto wavenetBytes = nts::test::wavenetFixture(static_cast<std::uint32_t>(channels), 16u,
                                                            nts::test::namCorpusLayers());
        nts::ml::PackedWaveNetModel wavenet;
        if (! wavenet.load(wavenetBytes, error)) { std::cerr << error << '\n'; return 4; }
        std::array<float, blockSize> wavenetInput {}, wavenetOutput {};
        for (std::size_t index = 0; index < blockSize; ++index)
            wavenetInput[index] = 0.08f * std::sin(static_cast<float>(index) * 0.17f);
        wavenet.reset();
        for (std::size_t iteration = 0; iteration < 256; ++iteration) wavenet.process(wavenetInput, wavenetOutput);
        std::array<double, batches> averages {}, percentiles {};
        for (std::size_t batch = 0; batch < batches; ++batch)
        {
            std::vector<double> timings; timings.reserve(iterations);
            for (std::size_t iteration = 0; iteration < iterations; ++iteration)
            {
                const auto start = std::chrono::steady_clock::now();
                wavenet.process(wavenetInput, wavenetOutput);
                const auto end = std::chrono::steady_clock::now();
                timings.push_back(std::chrono::duration<double, std::micro>(end - start).count());
            }
            std::sort(timings.begin(), timings.end());
            averages[batch] = std::accumulate(timings.begin(), timings.end(), 0.0) / timings.size();
            percentiles[batch] = timings[static_cast<std::size_t>(0.99 * static_cast<double>(timings.size() - 1))];
        }
        std::sort(averages.begin(), averages.end()); std::sort(percentiles.begin(), percentiles.end());
        const auto average = averages[batches / 2], p99 = percentiles[batches / 2];
        const auto budgetUs = 1.0e6 * static_cast<double>(blockSize) / 48000.0;
        std::cout << "wavenet" << channels << ",48000,64,1," << average << ',' << p99 << ','
                  << wavenet.memoryBytes() << ',' << p99 / budgetUs * 100.0 << "\n";
        if (channels == 3 && p99 >= budgetUs * 0.25) return 5;
    }
    return 0;
}
