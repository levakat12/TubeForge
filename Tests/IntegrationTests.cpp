#include "TestHarness.h"

#include <nts/audio/CoreEngine.h>
#include <nts/diagnostics/DiagnosticsCollector.h>
#include <nts/diagnostics/BackgroundWorker.h>
#include <nts/state/ProjectState.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

namespace
{
std::atomic<bool> countAllocations {};
std::atomic<std::size_t> allocationCount {};
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
struct EngineFixture
{
    explicit EngineFixture(std::size_t samples, double sampleRate = 48000.0)
        : left(samples), right(samples), outputLeft(samples), outputRight(samples), engine(parameters, meters)
    {
        for (std::size_t sample = 0; sample < samples; ++sample)
        {
            left[sample] = 0.2f * std::sin(static_cast<float>(sample) * 0.07f);
            right[sample] = 0.15f * std::cos(static_cast<float>(sample) * 0.05f);
        }
        engine.prepare(sampleRate, samples, 2, 2);
    }

    void process(std::uint64_t position = 0) noexcept
    {
        std::array<const float*, 2> inputPointers { left.data(), right.data() };
        std::array<float*, 2> outputPointers { outputLeft.data(), outputRight.data() };
        nts::audio::AudioProcessContext context {
            inputPointers, outputPointers, left.size(), 48000.0, position
        };
        engine.process(context);
    }

    nts::audio::RuntimeParameters parameters;
    nts::audio::MeterState meters;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> outputLeft;
    std::vector<float> outputRight;
    nts::audio::CoreEngine engine;
};

void testWrapperEquivalence(TestHarness& tests)
{
    EngineFixture standalone(512);
    EngineFixture vst3(512);
    standalone.parameters.inputGainDb.store(3.0f);
    standalone.parameters.outputGainDb.store(-1.0f);
    vst3.parameters.inputGainDb.store(3.0f);
    vst3.parameters.outputGainDb.store(-1.0f);
    standalone.process();
    vst3.process();
    tests.expectEqual(standalone.outputLeft, vst3.outputLeft,
                      "standalone and VST3 engine paths are bit-identical");
    tests.expectEqual(standalone.outputRight, vst3.outputRight,
                      "standalone and VST3 right channels are bit-identical");
}

void testNoAudioThreadAllocation(TestHarness& tests)
{
    EngineFixture fixture(32, 96000.0);
    allocationCount.store(0);
    countAllocations.store(true);
    fixture.process();
    countAllocations.store(false);
    tests.expectEqual(allocationCount.load(), std::size_t { 0 },
                      "audio processing performs no heap allocation after prepare");
}

void testRapidParametersAndStress(TestHarness& tests)
{
    EngineFixture fixture(32, 96000.0);
    const auto longRun = std::getenv("TUBEFORGE_LONG_STRESS") != nullptr;
    const auto blocks = longRun ? 10'800'000 : 20'000;
    bool finite = true;
    for (int block = 0; block < blocks; ++block)
    {
        fixture.parameters.inputGainDb.store(block % 2 == 0 ? -60.0f : 24.0f, std::memory_order_relaxed);
        fixture.parameters.outputGainDb.store(block % 3 == 0 ? -24.0f : 6.0f, std::memory_order_relaxed);
        fixture.parameters.bypass.store(block % 5 == 0, std::memory_order_relaxed);
        fixture.process(static_cast<std::uint64_t>(block) * 32);
        finite = finite && std::isfinite(fixture.outputLeft[0]) && std::isfinite(fixture.outputRight[0]);
    }
    tests.expect(finite, longRun ? "60-minute stress processing remains finite"
                                 : "96 kHz / 32-sample stress processing remains finite");
}

void testInvalidStateAndDiagnostics(TestHarness& tests)
{
    const auto invalid = nts::state::deserialize("{not-json");
    tests.expect(! invalid, "corrupt project state is rejected without throwing");

    nts::diagnostics::DiagnosticsCollector diagnostics;
    diagnostics.prepare(96000.0, 32);
    const auto started = diagnostics.beginCallback();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    diagnostics.endCallback(started, 32, 0);
    tests.expect(diagnostics.snapshot().dropoutCount >= 1, "callback diagnostics detect an overrun");
}


void testRepeatedPresetLoading(TestHarness& tests)
{
    nts::state::ProjectState state;
    for (int iteration = 0; iteration < 2000; ++iteration)
    {
        state.engine.inputGainDb = static_cast<float>((iteration % 84) - 60);
        const auto parsed = nts::state::deserialize(nts::state::serialize(state, false));
        tests.expect(static_cast<bool>(parsed), "repeated preset loading remains valid");
        if (! parsed)
            break;
    }
}

void testBackgroundWorkerFailureIsolation(TestHarness& tests)
{
    std::atomic<int> failures {};
    {
        nts::diagnostics::BackgroundWorker worker([&failures] { failures.fetch_add(1); });
        worker.submit([](std::stop_token) { throw 42; });
        for (int attempt = 0; attempt < 100 && failures.load() == 0; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    tests.expectEqual(failures.load(), 1, "background worker exceptions are contained and reported");
}
} // namespace

int main()
{
    TestHarness tests;
    testWrapperEquivalence(tests);
    testNoAudioThreadAllocation(tests);
    testRapidParametersAndStress(tests);
    testInvalidStateAndDiagnostics(tests);
    testRepeatedPresetLoading(tests);
    testBackgroundWorkerFailureIsolation(tests);
    return tests.result();
}
