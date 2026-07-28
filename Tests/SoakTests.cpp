#include "TestHarness.h"

#include <nts/amp/TraditionalAmp.h>
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

    // The engine on its own is only input gain, output gain, and metering, so a
    // soak restricted to it would exercise almost none of the shipped DSP. The
    // amplifier carries the oversampling, waveshaping, tone stack, sag, and
    // convolution work that actually has to stay stable for an hour.
    nts::amp::TraditionalAmpProcessor amp;
    amp.prepare({ sampleRate, blockSize, 2 });
    const auto guitarPreset = nts::amp::makeOriginalPreset(nts::amp::Topology::tightModern,
                                                           nts::amp::Instrument::guitar);
    const auto bassPreset = nts::amp::makeOriginalPreset(nts::amp::Topology::vintageBloom,
                                                         nts::amp::Instrument::bass);
    amp.loadPreset(guitarPreset);

    std::array<float, blockSize> left {};
    std::array<float, blockSize> right {};
    std::array<float, blockSize> outputLeft {};
    std::array<float, blockSize> outputRight {};
    std::array<const float*, 2> inputs { left.data(), right.data() };
    std::array<float*, 2> outputs { outputLeft.data(), outputRight.data() };
    std::array<float*, 2> ampChannels { outputLeft.data(), outputRight.data() };

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + duration;
    std::uint64_t blocksProcessed {};
    auto finite = true;
    auto bounded = true;
    auto producedSignal = false;
    double phase = 0.0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        // A drifting phase keeps the excitation from repeating exactly per block,
        // so filter and sag state is continuously disturbed rather than settling.
        for (std::size_t sample = 0; sample < blockSize; ++sample)
        {
            const auto value = 0.25f * static_cast<float>(std::sin(phase))
                             * (1.0f + 0.5f * static_cast<float>(std::sin(phase * 0.0013)));
            left[sample] = value;
            right[sample] = -0.75f * value;
            outputLeft[sample] = value;
            outputRight[sample] = -0.75f * value;
            phase += 0.0271;
        }

        parameters.inputGainDb.store(blocksProcessed % 2 == 0 ? -12.0f : 12.0f,
                                     std::memory_order_relaxed);
        parameters.outputGainDb.store(blocksProcessed % 3 == 0 ? -6.0f : 0.0f,
                                      std::memory_order_relaxed);
        parameters.bypass.store(blocksProcessed % 11 == 0, std::memory_order_relaxed);

        // Automation, preset crossfades, and a full state reset all have to stay
        // stable while audio keeps flowing.
        auto ampParameters = (blocksProcessed / 4096) % 2 == 0 ? guitarPreset.parameters
                                                               : bassPreset.parameters;
        ampParameters.stages[0].driveDb = 6.0f + 12.0f * static_cast<float>(
            std::sin(static_cast<double>(blocksProcessed) * 0.0007));
        ampParameters.toneStack.mid = 0.5f + 0.45f * static_cast<float>(
            std::sin(static_cast<double>(blocksProcessed) * 0.0003));
        ampParameters.powerAmp.presence = 0.5f + 0.45f * static_cast<float>(
            std::cos(static_cast<double>(blocksProcessed) * 0.0005));
        amp.setParameters(ampParameters);

        if (blocksProcessed % 262144 == 262143) amp.reset();
        if (blocksProcessed % 131072 == 65536)
            amp.loadPreset((blocksProcessed / 131072) % 2 == 0 ? bassPreset : guitarPreset);

        amp.process(ampChannels.data(), 2, blockSize);

        nts::audio::AudioProcessContext context {
            inputs, outputs, blockSize, sampleRate, blocksProcessed * blockSize
        };
        engine.process(context);

        for (std::size_t sample = 0; sample < blockSize; ++sample)
        {
            finite = finite && std::isfinite(outputLeft[sample]) && std::isfinite(outputRight[sample]);
            // A stable amplifier must not run away, even under continuous
            // automation and repeated preset changes.
            bounded = bounded && std::abs(outputLeft[sample]) < 64.0f
                              && std::abs(outputRight[sample]) < 64.0f;
            producedSignal = producedSignal || std::abs(outputLeft[sample]) > 1.0e-6f;
        }

        ++blocksProcessed;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    TestHarness tests;
    tests.expect(finite, "continuous processing remains finite");
    tests.expect(bounded, "continuous processing stays bounded");
    tests.expect(producedSignal, "soak test processes a non-silent signal");
    tests.expect(elapsed >= duration, "soak test runs for the requested wall-clock duration");
    tests.expect(blocksProcessed > 0, "soak test processes audio blocks continuously");
    std::cout << "Processed " << blocksProcessed << " blocks through the amplifier and engine in "
              << elapsed.count() << " seconds\n";
    return tests.result();
}
