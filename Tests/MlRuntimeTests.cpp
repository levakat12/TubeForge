#include "PackedFixtures.h"
#include "TestHarness.h"
#include <nts/ml/NeuralAmpProcessor.h>
#include <nts/ml/NeuralModel.h>
#include <nts/ml/PackedTanhModel.h>
#include <nts/ml/PackedWaveNetModel.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <new>

namespace
{
thread_local bool countAllocations {};
std::atomic<std::size_t> allocationCount {};
}

void* operator new(std::size_t size)
{
    if (countAllocations) allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size)) return memory;
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace
{
void appendU32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    const auto offset = bytes.size(); bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void appendFloat(std::vector<std::byte>& bytes, float value)
{
    const auto offset = bytes.size(); bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void writeFloat(std::vector<std::byte>& bytes, std::size_t offset, float value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
std::vector<std::byte> fixture()
{
    std::vector<std::byte> bytes { std::byte { 'N' }, std::byte { 'T' }, std::byte { 'S' }, std::byte { 'M' } };
    for (const auto value : { 1u, 1u, 48000u, 1u, 1u, 1u }) appendU32(bytes, value);
    for (const auto value : { 0.5f, 0.25f, 0.1f, 0.75f, -0.05f }) appendFloat(bytes, value);
    return bytes;
}

using nts::test::wavenetFixture;
}

int main()
{
    TestHarness tests;
    nts::ml::PackedTanhModel model; std::string error; const auto bytes = fixture();
    tests.expect(model.load(bytes, error), "packed recurrent model loads: " + error);
    tests.expectEqual(model.stateSize(), std::size_t { 1 }, "packed runtime exposes state size");
    tests.expectEqual(model.sampleRate(), 48000, "packed runtime exposes sample rate");
    const std::array input { 0.2f, -0.1f, 0.0f, 0.3f };
    std::array<float, input.size()> first {}, second {};
    tests.expect(model.process(input, first), "packed runtime processes fixed vector");
    tests.expect(std::all_of(first.begin(), first.end(), [](float value) { return std::isfinite(value); }),
                 "packed runtime output is finite");
    model.reset(); model.process(input, second);
    for (std::size_t index = 0; index < first.size(); ++index)
        tests.expectNear(first[index], second[index], 1.0e-7, "state reset is deterministic");
    model.reset();
    std::array<float, input.size()> partitioned {};
    tests.expect(model.process(std::span(input.data(), 1), std::span(partitioned.data(), 1)),
                 "first arbitrary partition processes");
    tests.expect(model.process(std::span(input.data() + 1, input.size() - 1),
                               std::span(partitioned.data() + 1, partitioned.size() - 1)),
                 "second arbitrary partition processes");
    for (std::size_t index = 0; index < first.size(); ++index)
        tests.expectNear(first[index], partitioned[index], 1.0e-7, "packed runtime is block-size independent");
    auto invalid = bytes; invalid[0] = std::byte { 'X' };
    tests.expect(! model.load(invalid, error), "invalid packed header is rejected");

    nts::ml::NeuralAmpProcessor processor;
    processor.prepare(48000.0, 64, 1);
    nts::ml::PackedTanhModel stageReference;
    tests.expect(stageReference.load(bytes, error), "stage reference reloads");
    stageReference.reset();
    std::array<float, input.size()> expected {};
    stageReference.process(input, expected);
    tests.expect(processor.stageModel(bytes, input, expected, 1.0e-6f, -21.0f, error),
                 "validated model is staged: " + error);
    std::array<float, 64> block {}; block.fill(0.2f); float* channel = block.data();
    processor.process(&channel, 1, block.size());
    tests.expect(processor.hasActiveModel(), "staged model activates at the block boundary");
    tests.expect(std::all_of(block.begin(), block.end(), [](float value) { return std::isfinite(value); }),
                 "neural processor output remains finite");
    tests.expect(processor.calibrationReading().warning, "input-level mismatch warning is reported");
    tests.expect(processor.modelMemoryBytes() > 0, "model memory is reported");

    auto replacement = bytes;
    writeFloat(replacement, 4 + 6 * sizeof(std::uint32_t) + 3 * sizeof(float), 0.2f);
    nts::ml::PackedTanhModel replacementReference;
    tests.expect(replacementReference.load(replacement, error), "replacement fixture loads");
    std::array<float, input.size()> replacementExpected {};
    replacementReference.process(input, replacementExpected);
    tests.expect(processor.stageModel(replacement, input, replacementExpected, 1.0e-6f, -21.0f, error),
                 "replacement model validates off the audio path: " + error);
    float previous = block.back();
    float maximumStep {};
    for (int callback = 0; callback < 20; ++callback)
    {
        block.fill(0.2f); processor.process(&channel, 1, block.size());
        for (const auto value : block) { maximumStep = std::max(maximumStep, std::abs(value - previous)); previous = value; }
    }
    tests.expect(maximumStep < 0.08f, "model replacement crossfade is click-free");
    auto wrongExpected = replacementExpected; wrongExpected[0] += 0.5f;
    tests.expect(! processor.stageModel(bytes, input, wrongExpected, 1.0e-6f, -21.0f, error),
                 "a model with a failing test vector is rejected");
    processor.setMonitorMode(nts::ml::NeuralMonitorMode::bypassDi);
    block.fill(0.125f); processor.process(&channel, 1, block.size());
    tests.expectNear(block[10], 0.125, 1.0e-7, "bypass DI comparison is exact");
    processor.setMonitorMode(nts::ml::NeuralMonitorMode::model);
    allocationCount.store(0, std::memory_order_relaxed); countAllocations = true;
    block.fill(0.125f); processor.process(&channel, 1, block.size());
    countAllocations = false;
    tests.expectEqual(allocationCount.load(std::memory_order_relaxed), std::size_t { 0 },
                      "neural audio processing performs no heap allocation");

    nts::ml::PackedWaveNetModel wavenet;
    const auto wavenetBytes = wavenetFixture();
    tests.expect(wavenet.load(wavenetBytes, error), "packed WaveNet loads: " + error);
    tests.expectEqual(wavenet.channels(), std::size_t { 2 }, "WaveNet exposes its channel count");
    tests.expectEqual(wavenet.layerCount(), std::size_t { 2 }, "WaveNet exposes its layer count");
    // (2-1)*1 + (2-1)*2 + headKernel 2
    tests.expectEqual(wavenet.receptiveField(), std::size_t { 5 }, "WaveNet reports its receptive field");
    tests.expectEqual(wavenet.sampleRate(), 48000, "WaveNet exposes its sample rate");

    std::array<float, 96> wavenetInput {};
    for (std::size_t index = 0; index < wavenetInput.size(); ++index)
        wavenetInput[index] = 0.1f * std::sin(static_cast<float>(index) * 0.31f);
    std::array<float, 96> wavenetFirst {}, wavenetSecond {}, wavenetSplit {};
    wavenet.reset();
    tests.expect(wavenet.process(wavenetInput, wavenetFirst), "WaveNet processes a block");
    tests.expect(std::all_of(wavenetFirst.begin(), wavenetFirst.end(),
                             [](float value) { return std::isfinite(value); }),
                 "WaveNet output is finite");
    wavenet.reset();
    wavenet.process(wavenetInput, wavenetSecond);
    for (std::size_t index = 0; index < wavenetFirst.size(); ++index)
        tests.expectNear(wavenetFirst[index], wavenetSecond[index], 1.0e-7,
                         "WaveNet reset restores the primed state exactly");
    wavenet.reset();
    for (std::size_t offset = 0; offset < wavenetInput.size(); offset += 7)
    {
        const auto count = std::min<std::size_t>(7, wavenetInput.size() - offset);
        wavenet.process(std::span(wavenetInput.data() + offset, count),
                        std::span(wavenetSplit.data() + offset, count));
    }
    for (std::size_t index = 0; index < wavenetFirst.size(); ++index)
        tests.expectNear(wavenetFirst[index], wavenetSplit[index], 1.0e-7,
                         "WaveNet output is block-size independent");

    // A primed model is at rest under silence: the biases have already propagated, so the output is
    // a constant rather than a settling transient.
    std::array<float, 64> silence {}, idle {};
    wavenet.reset();
    wavenet.process(silence, idle);
    for (const auto value : idle)
        tests.expectNear(value, idle[0], 1.0e-7, "a primed WaveNet is stationary under silence");

    auto truncatedTable = wavenetBytes; truncatedTable.resize(4 + 11 * sizeof(std::uint32_t) + 4);
    tests.expect(! wavenet.load(truncatedTable, error), "a truncated WaveNet layer table is rejected");
    auto shortPayload = wavenetBytes; shortPayload.resize(shortPayload.size() - sizeof(float));
    tests.expect(! wavenet.load(shortPayload, error), "a WaveNet payload of the wrong size is rejected");
    auto wrongVersion = wavenetBytes; appendU32(wrongVersion, 0u);
    std::memcpy(wrongVersion.data() + 4, "\x02\x00\x00\x00", 4);
    tests.expect(! wavenet.load(wrongVersion, error), "a non-v3 header is rejected by the WaveNet loader");
    auto hugeDilation = wavenetBytes;
    const std::uint32_t outOfRange = 99999u;
    std::memcpy(hugeDilation.data() + 4 + 11 * sizeof(std::uint32_t) + sizeof(std::uint32_t),
                &outOfRange, sizeof(outOfRange));
    tests.expect(! wavenet.load(hugeDilation, error), "an out-of-range dilation is rejected");

    nts::ml::NeuralModel dispatched;
    tests.expect(dispatched.load(wavenetBytes, error), "the shared model dispatches to WaveNet: " + error);
    tests.expect(dispatched.isWaveNet(), "a v3 header selects the WaveNet implementation");
    tests.expectEqual(dispatched.warmUpSamples(), std::size_t { 5 }, "WaveNet reports its warm-up length");
    tests.expect(dispatched.load(bytes, error), "the shared model still loads recurrent packs: " + error);
    tests.expect(! dispatched.isWaveNet(), "a v1 header selects the recurrent implementation");

    nts::ml::NeuralAmpProcessor wavenetProcessor;
    wavenetProcessor.prepare(48000.0, 64, 1);
    nts::ml::PackedWaveNetModel wavenetReference;
    tests.expect(wavenetReference.load(wavenetBytes, error), "WaveNet staging reference loads");
    std::array<float, 64> stageInput {}, stageExpected {};
    for (std::size_t index = 0; index < stageInput.size(); ++index)
        stageInput[index] = 0.08f * std::sin(static_cast<float>(index) * 0.19f);
    wavenetReference.reset();
    wavenetReference.process(stageInput, stageExpected);
    tests.expect(wavenetProcessor.stageModel(wavenetBytes, stageInput, stageExpected, 1.0e-6f, -21.0f, error),
                 "a WaveNet model validates and stages: " + error);
    std::array<float, 64> wavenetBlock {};
    auto* wavenetChannel = wavenetBlock.data();
    wavenetBlock.fill(0.1f); wavenetProcessor.process(&wavenetChannel, 1, wavenetBlock.size());
    tests.expect(wavenetProcessor.hasActiveModel(), "the staged WaveNet activates");
    tests.expect(wavenetProcessor.modelMemoryBytes() > 0, "WaveNet memory is reported");
    allocationCount.store(0, std::memory_order_relaxed); countAllocations = true;
    wavenetBlock.fill(0.1f); wavenetProcessor.process(&wavenetChannel, 1, wavenetBlock.size());
    countAllocations = false;
    tests.expectEqual(allocationCount.load(std::memory_order_relaxed), std::size_t { 0 },
                      "WaveNet audio processing performs no heap allocation");
    return tests.result();
}
