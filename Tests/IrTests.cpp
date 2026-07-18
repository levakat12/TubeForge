#include "TestHarness.h"

#include <nts/ir/CabinetIrLoader.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

int main()
{
    TestHarness tests;
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("tubeforge-cabinet-test", ".wav");
    {
        juce::AudioBuffer<float> buffer(1, 128);
        buffer.clear(); buffer.setSample(0, 8, 0.8f); buffer.setSample(0, 12, -0.2f);
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        auto writer = stream != nullptr
            ? format.createWriterFor(stream, juce::AudioFormatWriterOptions {}
                                                 .withSampleRate(24000.0)
                                                 .withNumChannels(1)
                                                 .withBitsPerSample(16))
            : nullptr;
        tests.expect(writer != nullptr, "test cabinet WAV writer opens");
        if (writer != nullptr)
        {
            tests.expect(writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()),
                         "test cabinet WAV is written");
        }
    }

    std::atomic<bool> completed {};
    nts::ir::CabinetLoadResult result;
    {
        nts::ir::CabinetIrLoader loader;
        nts::dsp::ImpulsePreparationOptions options;
        options.outputChannels = 2;
        loader.loadAsync(file, 48000.0, options,
                         [&result, &completed](nts::ir::CabinetLoadResult loaded)
                         {
                             result = std::move(loaded);
                             completed.store(true, std::memory_order_release);
                         });
        for (int attempt = 0; attempt < 1000 && ! completed.load(std::memory_order_acquire); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    tests.expect(completed.load(std::memory_order_acquire), "cabinet IR worker completes asynchronously");
    tests.expect(static_cast<bool>(result), "cabinet IR file decodes and preprocesses: " + result.error);
    if (result)
    {
        tests.expectEqual(result.impulse.channels.size(), std::size_t { 2 }, "cabinet loader converts mono to stereo");
        tests.expect(result.impulse.channels[0].size() > 128, "cabinet loader resamples to engine rate");
    }
    file.deleteFile();
    return tests.result();
}
