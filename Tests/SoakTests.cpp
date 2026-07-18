#include "TestHarness.h"

#include <nts/audio/CoreEngine.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argumentCount, char** arguments)
{
    const auto requestedSeconds = argumentCount > 1 ? std::stoi(arguments[1]) : 3600;
    const auto duration = std::chrono::seconds(std::max(1, requestedSeconds));
    constexpr std::size_t blockSize = 32;
    constexpr auto sampleRate = 96000.0;

    nts::audio::RuntimeParameters parameters;
    nts::audio::MeterState meters;
    nts::audio::CoreEngine engine(parameters, meters);
    engine.prepare(sampleRate, blockSize, 2, 2);

    std::array<float, blockSize> left {};
    std::array<float, blockSize> right {};
    std::array<float, blockSize> outputLeft {};
    std::array<float, blockSize> outputRight {};
    std::array<const float*, 2> inputs { left.data(), right.data() };
    std::array<float*, 2> outputs { outputLeft.data(), outputRight.data() };

    for (std::size_t sample = 0; sample < blockSize; ++sample)
    {
        left[sample] = 0.1f * std::sin(static_cast<float>(sample) * 0.17f);
        right[sample] = -left[sample];
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + duration;
    std::uint64_t blocksProcessed {};
    auto finite = true;

    while (std::chrono::steady_clock::now() < deadline)
    {
        parameters.inputGainDb.store(blocksProcessed % 2 == 0 ? -12.0f : 12.0f,
                                     std::memory_order_relaxed);
        parameters.outputGainDb.store(blocksProcessed % 3 == 0 ? -6.0f : 0.0f,
                                      std::memory_order_relaxed);
        parameters.bypass.store(blocksProcessed % 11 == 0, std::memory_order_relaxed);

        nts::audio::AudioProcessContext context {
            inputs, outputs, blockSize, sampleRate, blocksProcessed * blockSize
        };
        engine.process(context);
        finite = finite && std::isfinite(outputLeft[0]) && std::isfinite(outputRight[0]);
        ++blocksProcessed;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    TestHarness tests;
    tests.expect(finite, "continuous processing remains finite");
    tests.expect(elapsed >= duration, "soak test runs for the requested wall-clock duration");
    tests.expect(blocksProcessed > 0, "soak test processes audio blocks continuously");
    std::cout << "Processed " << blocksProcessed << " blocks in " << elapsed.count() << " seconds\n";
    return tests.result();
}
