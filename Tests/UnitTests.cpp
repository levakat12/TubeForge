#include "TestHarness.h"

#include <nts/audio/LinearSmoother.h>
#include <nts/audio/ImmutableSnapshotExchange.h>
#include <nts/diagnostics/LatencyBudget.h>
#include <nts/diagnostics/DiagnosticsCollector.h>
#include <nts/diagnostics/ProcessMemory.h>
#include <nts/diagnostics/SpscRingBuffer.h>
#include <nts/state/ProjectState.h>
#include <nts/state/SettingsStore.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace
{
void testSmoothing(TestHarness& tests)
{
    nts::audio::LinearSmoother smoother;
    smoother.prepare(1000.0, 0.01);
    smoother.reset(1.0f);
    smoother.setTarget(2.0f);
    for (int index = 0; index < 10; ++index)
        static_cast<void>(smoother.next());
    tests.expectNear(smoother.currentValue(), 2.0, 1.0e-6, "parameter smoothing reaches its target");
}

void testLatency(TestHarness& tests)
{
    const nts::diagnostics::LatencyBudget latency { 32, 4, 128, 16, 8, 2 };
    tests.expectEqual(latency.processingSamples(), 158, "processing latency sums every engine source");
    tests.expectEqual(latency.totalSamples(), 190, "total latency includes the host buffer");
}

void testProcessMemory(TestHarness& tests)
{
    const auto memory = nts::diagnostics::sampleProcessMemory();
    tests.expect(memory.workingSetBytes > 0, "non-real-time process memory sampler reports the working set");
    tests.expect(memory.privateBytes > 0, "non-real-time process memory sampler reports private/virtual bytes");
}

void testAssetAndJobDiagnostics(TestHarness& tests)
{
    nts::diagnostics::DiagnosticsCollector diagnostics;
    diagnostics.setModelLoadStatus(nts::diagnostics::AssetLoadStatus::loading);
    diagnostics.setIrLoadStatus(nts::diagnostics::AssetLoadStatus::ready);
    diagnostics.reportBackgroundJobFailure(7);
    const auto snapshot = diagnostics.snapshot();
    tests.expectEqual(snapshot.modelLoadStatus, nts::diagnostics::AssetLoadStatus::loading,
                      "model load status is available to diagnostics consumers");
    tests.expectEqual(snapshot.irLoadStatus, nts::diagnostics::AssetLoadStatus::ready,
                      "IR load status is available to diagnostics consumers");
    tests.expectEqual(snapshot.backgroundJobFailureCount, std::uint64_t { 1 },
                      "background job failures are counted");
}

void testQueue(TestHarness& tests)
{
    nts::diagnostics::SpscRingBuffer<int, 8> queue;
    for (int value = 0; value < 7; ++value)
        tests.expect(queue.push(value), "queue accepts values until usable capacity");
    tests.expect(! queue.push(7), "queue reports full without overwriting unread data");

    for (int expected = 0; expected < 7; ++expected)
    {
        int actual = -1;
        tests.expect(queue.pop(actual), "queue returns every inserted value");
        tests.expectEqual(actual, expected, "queue preserves FIFO order");
    }
    tests.expect(queue.empty(), "queue reports empty after draining");
}

void testImmutableSnapshotExchange(TestHarness& tests)
{
    nts::audio::ImmutableSnapshotExchange<int> exchange(std::make_unique<const int>(10));
    {
        const auto audioRead = exchange.readForAudio();
        tests.expectEqual(*audioRead, 10, "audio thread reads the current immutable snapshot");
        exchange.publish(std::make_unique<const int>(20));
        tests.expectEqual(*audioRead, 10, "published state does not mutate an in-flight audio snapshot");
    }
    exchange.reclaimRetired();
    const auto nextRead = exchange.readForAudio();
    tests.expectEqual(*nextRead, 20, "subsequent audio block sees the published snapshot");
}

void testConcurrentSnapshotExchange(TestHarness& tests)
{
    struct Snapshot
    {
        std::uint64_t generation {};
        std::uint64_t inverse {};
    };

    nts::audio::ImmutableSnapshotExchange<Snapshot> exchange(
        std::make_unique<const Snapshot>(Snapshot { 0, ~std::uint64_t { 0 } }));
    std::atomic<bool> ready {};
    std::atomic<bool> stop {};
    std::atomic<bool> invalid {};
    std::atomic<std::uint64_t> reads {};

    std::thread reader([&]
    {
        ready.store(true, std::memory_order_release);
        while (! stop.load(std::memory_order_acquire))
        {
            const auto snapshot = exchange.readForAudio();
            const auto generation = snapshot->generation;
            if (snapshot->inverse != ~generation)
                invalid.store(true, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    while (! ready.load(std::memory_order_acquire))
        std::this_thread::yield();

    for (std::uint64_t generation = 1; generation <= 50'000; ++generation)
        exchange.publish(std::make_unique<const Snapshot>(Snapshot { generation, ~generation }));

    stop.store(true, std::memory_order_release);
    reader.join();
    exchange.reclaimRetired();
    tests.expect(! invalid.load(std::memory_order_relaxed),
                 "concurrent publication never exposes a torn or reclaimed snapshot");
    tests.expect(reads.load(std::memory_order_relaxed) > 0,
                 "audio reader progresses during concurrent snapshot publication");
}

void testStateMigrationAndValidation(TestHarness& tests)
{
    const auto migrated = nts::state::deserialize(
        R"({"inputGain":3.5,"outputGain":-2.0,"bypass":true})");
    tests.expect(static_cast<bool>(migrated), "legacy state migrates to the current schema");
    if (migrated)
    {
        tests.expectEqual(migrated.state->schemaVersion, 1, "migration writes schema version 1");
        tests.expectNear(migrated.state->engine.inputGainDb, 3.5, 1.0e-6, "migration preserves input gain");
        tests.expect(migrated.state->engine.bypass, "migration preserves bypass");
    }

    nts::state::ProjectState invalid;
    invalid.assets.relativePaths.push_back("C:/absolute/cab.wav");
    std::string error;
    tests.expect(! nts::state::validate(invalid, error), "preset validation rejects absolute asset paths");
    tests.expect(! error.empty(), "preset validation returns an actionable error");
}

void testStateRoundTrip(TestHarness& tests)
{
    nts::state::ProjectState original;
    original.engine = { 4.5f, -3.0f, true };
    original.device = { "ASIO", "Input", "Output", 96000.0, 32, { 0 }, { 0, 1 } };
    original.graph.latencySamples = 48;
    original.ui = { 900, 640, false };
    original.assets.relativePaths = { "irs/cab.wav", "models/amp.ntm" };

    const auto parsed = nts::state::deserialize(nts::state::serialize(original));
    tests.expect(static_cast<bool>(parsed), "serialized state parses successfully");
    if (parsed)
        tests.expectEqual(*parsed.state, original, "project state round-trips exactly");
}

void testPathsAndPersistentSettings(TestHarness& tests)
{
    const auto relative = nts::state::normalizeAssetPath(
        std::filesystem::path("C:/project/session/song.tforge"),
        std::filesystem::path("C:/project/session/assets/cab.wav"));
    tests.expectEqual(relative, std::string("assets/cab.wav"), "project asset path becomes relative and normalized");

    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto file = std::filesystem::temp_directory_path() / ("tubeforge-settings-" + unique + ".json");
    nts::state::SettingsStore store(file);
    nts::state::ProjectState state;
    state.engine.inputGainDb = 2.0f;
    std::string error;
    tests.expect(store.save(state, error), "persistent settings save succeeds");
    const auto loaded = store.load();
    tests.expect(static_cast<bool>(loaded), "persistent settings load succeeds");
    if (loaded)
        tests.expectEqual(*loaded.state, state, "persistent settings preserve state exactly");
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}
} // namespace

int main()
{
    TestHarness tests;
    testSmoothing(tests);
    testLatency(tests);
    testProcessMemory(tests);
    testAssetAndJobDiagnostics(tests);
    testQueue(tests);
    testImmutableSnapshotExchange(tests);
    testConcurrentSnapshotExchange(tests);
    testStateMigrationAndValidation(tests);
    testStateRoundTrip(tests);
    testPathsAndPersistentSettings(tests);
    return tests.result();
}
