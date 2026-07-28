#include "TestHarness.h"

#include <nts/circuit/CircuitProcessor.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

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
std::vector<std::byte> neuralFixture()
{
    std::vector<std::byte> bytes { std::byte { 'N' }, std::byte { 'T' }, std::byte { 'S' }, std::byte { 'M' } };
    for (const auto value : { 1u, 1u, 48000u, 1u, 1u, 1u }) appendU32(bytes, value);
    for (const auto value : { 0.5f, 0.25f, 0.1f, 0.75f, -0.05f }) appendFloat(bytes, value);
    return bytes;
}
}

int main()
{
    TestHarness tests;
    using namespace nts::circuit;
    const auto graph = makeSimpleCircuit({});
    const auto validation = CircuitCompiler::validate(graph);
    tests.expect(validation.isValid(), "simple macro controls produce a valid circuit");
    tests.expectEqual(graph.nodes.size(), std::size_t { 10 }, "simple circuit includes the complete amplifier chain");
    tests.expect(TubeLibrary::find("tube.12ax7.v1") != nullptr, "electrical tube library uses stable ids");

    ValidationReport report;
    auto runtime = CircuitCompiler::compile(graph, { 48000.0, 64, 1 }, {}, report);
    tests.expect(runtime != nullptr && report.isValid(), "physical circuit compiles off the audio path");
    std::array<float, 64> input {}, output {};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.18f * static_cast<float>(std::sin(
            2.0 * 3.141592653589793 * 440.0 * static_cast<double>(i) / 48000.0));
    runtime->process(input, output);
    tests.expect(std::all_of(output.begin(), output.end(), [](float value) { return std::isfinite(value); }),
                 "circuit output remains finite");
    tests.expect(std::any_of(output.begin(), output.end(), [](float value) { return std::abs(value) > 1.0e-5f; }),
                 "compiled graph produces signal");

    auto alternate = graph;
    auto& triode = *std::find_if(alternate.nodes.begin(), alternate.nodes.end(),
                                 [](const auto& node) { return node.type == NodeType::triodeStage; });
    triode.modelId = "tube.12au7.v1";
    auto alternateRuntime = CircuitCompiler::compile(alternate, { 48000.0, 64, 1 }, {}, report);
    std::array<float, 64> alternateOutput {};
    alternateRuntime->process(input, alternateOutput);
    float tubeDifference {};
    for (std::size_t i = 0; i < output.size(); ++i) tubeDifference += std::abs(output[i] - alternateOutput[i]);
    tests.expect(tubeDifference > 1.0e-3f, "electrical tube substitution changes nonlinear behavior");

    const auto json = serializeCircuit(graph);
    CircuitGraphDescription restored; std::string error;
    tests.expect(deserializeCircuit(json, restored, error), "circuit JSON restores: " + error);
    tests.expectEqual(serializeCircuit(restored), json, "circuit saves and restores exactly with stable ids");
    auto legacyJson = json;
    const auto schemaPosition = legacyJson.find("\"schemaVersion\": 1");
    if (schemaPosition != std::string::npos) legacyJson.replace(schemaPosition, 18, "\"schemaVersion\": 0");
    CircuitGraphDescription migrated;
    tests.expect(deserializeCircuit(legacyJson, migrated, error) && migrated.schemaVersion == currentCircuitSchemaVersion,
                 "schema v0 preset migrates to the current stable graph schema");

    auto invalid = graph;
    invalid.connections.push_back({ "output", 0, "v1", 0, false });
    tests.expect(!CircuitCompiler::validate(invalid).isValid(), "cycles outside explicit feedback are rejected");
    invalid = graph;
    auto& invalidTriode = *std::find_if(invalid.nodes.begin(), invalid.nodes.end(),
                                        [](const auto& node) { return node.type == NodeType::triodeStage; });
    invalidTriode.parameters.push_back({ "plate-voltage-v", 900.0f });
    tests.expect(!CircuitCompiler::validate(invalid).isValid(), "unsafe virtual parameter ranges are rejected");

    const auto& tone = *std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                     [](const auto& node) { return node.type == NodeType::toneStack; });
    tests.expect(std::isfinite(toneStackMagnitude(tone, 1000.0, 48000.0)),
                 "component-value-derived tone response is available to engineering UI");

    auto hybridGraph = graph;
    auto& hybridNode = *std::find_if(hybridGraph.nodes.begin(), hybridGraph.nodes.end(),
                                     [](const auto& node) { return node.type == NodeType::triodeStage; });
    hybridNode.backend = ModelBackend::neuralSurrogate;
    NeuralModelBytes models; models.emplace(hybridNode.id, neuralFixture());
    auto hybrid = CircuitCompiler::compile(hybridGraph, { 48000.0, 64, 1 }, models, report);
    tests.expect(hybrid != nullptr && report.isValid(), "neural surrogate implements the physical component interface");
    if (hybrid)
    {
        hybrid->process(input, output);
        tests.expect(std::all_of(output.begin(), output.end(), [](float value) { return std::isfinite(value); }),
                     "hybrid neural circuit processes safely in real time");
    }
    auto wrongRateHybrid = CircuitCompiler::compile(hybridGraph, { 44100.0, 64, 1 }, models, report);
    tests.expect(wrongRateHybrid == nullptr && !report.isValid(), "invalid neural surrogate sample rate is rejected before activation");

    runtime->reset(); runtime->process(input, output);
    const auto resetReference = output;
    runtime->process(input, output); runtime->reset(); runtime->process(input, output);
    for (std::size_t i = 0; i < output.size(); ++i)
        tests.expectNear(output[i], resetReference[i], 1.0e-6, "component state reset is deterministic");
    std::array<float, 64> hardInput {}; hardInput.fill(0.9f);
    for (int blockIndex = 0; blockIndex < 40; ++blockIndex) runtime->process(hardInput, output);
    const auto telemetry = runtime->telemetrySnapshot();
    const auto powerTelemetry = std::find_if(telemetry.begin(), telemetry.end(), [](const auto& node)
    {
        return node.type == NodeType::powerStage;
    });
    tests.expect(powerTelemetry != telemetry.end() && powerTelemetry->values.sagPercent > 0.0f,
                 "stateful bounded supply sags under sustained current demand");

    SimpleControls extremeControls { -2.0f, 3.0f, 4.0f, -1.0f, 2.0f, 3.0f, 4.0f, -2.0f };
    tests.expect(CircuitCompiler::validate(makeSimpleCircuit(extremeControls)).isValid(),
                 "simple macro extremes clamp into valid component ranges");

    CircuitProcessor processor;
    processor.prepare({ 48000.0, 64, 1 });
    tests.expect(processor.stageGraph(graph).isValid(), "initial immutable graph is staged");
    std::array<float, 64> block {}; block.fill(0.1f); float* channel = block.data();
    processor.process(&channel, 1, block.size());
    tests.expect(processor.stageGraph(alternate).isValid(), "replacement immutable graph is staged");
    auto previous = block.back(); float maximumStep {};
    for (int callback = 0; callback < 36; ++callback)
    {
        block.fill(0.1f); processor.process(&channel, 1, block.size());
        for (const auto value : block) { maximumStep = std::max(maximumStep, std::abs(value - previous)); previous = value; }
    }
    tests.expect(maximumStep < 0.25f, "graph publication crossfade is click-free");
    tests.expect(!processor.telemetrySnapshot().empty(), "UI receives copied per-stage telemetry snapshots");

    CircuitProcessor inactiveProcessor;
    inactiveProcessor.prepare({ 48000.0, 64, 1 });
    tests.expect(inactiveProcessor.stageGraph(graph).isValid(), "inactive engine accepts its first graph");
    inactiveProcessor.publishPendingWithoutCrossfade();
    tests.expect(inactiveProcessor.stageGraph(alternate).isValid(),
                 "inactive engine publishes pending work and accepts a later part switch");
    inactiveProcessor.publishPendingWithoutCrossfade();
    tests.expect(! inactiveProcessor.telemetrySnapshot().empty(),
                 "inactive publication makes the selected graph ready before engine switching");
    std::array<float, 64> inactiveBlock {}; inactiveBlock.fill(0.05f); float* inactiveChannel = inactiveBlock.data();
    tests.expect(inactiveProcessor.stageGraph(graph).isValid(), "active circuit accepts another graph");
    inactiveProcessor.process(&inactiveChannel, 1, inactiveBlock.size());
    inactiveProcessor.publishPendingWithoutCrossfade();
    tests.expect(inactiveProcessor.stageGraph(alternate).isValid(),
                 "leaving circuit mode cancels an unfinished crossfade without blocking edits");

    allocationCount.store(0, std::memory_order_relaxed); countAllocations = true;
    block.fill(0.1f); processor.process(&channel, 1, block.size());
    countAllocations = false;
    tests.expectEqual(allocationCount.load(std::memory_order_relaxed), std::size_t { 0 },
                      "circuit audio processing performs no heap allocation");
    return tests.result();
}
