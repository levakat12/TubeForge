#pragma once

#include "Components.h"

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nts::circuit
{
using NeuralModelBytes = std::unordered_map<std::string, std::vector<std::byte>>;

class CompiledCircuitGraph
{
public:
    CompiledCircuitGraph() = default;
    CompiledCircuitGraph(CompiledCircuitGraph&&) noexcept = default;
    CompiledCircuitGraph& operator=(CompiledCircuitGraph&&) noexcept = default;
    CompiledCircuitGraph(const CompiledCircuitGraph&) = delete;
    CompiledCircuitGraph& operator=(const CompiledCircuitGraph&) = delete;

    void reset() noexcept;
    void process(std::span<const float> input, std::span<float> output) noexcept;
    [[nodiscard]] std::vector<NodeTelemetry> telemetrySnapshot() const;
    [[nodiscard]] std::size_t latencySamples() const noexcept { return latency; }
    [[nodiscard]] const CircuitGraphDescription& description() const noexcept { return graphDescription; }
    [[nodiscard]] bool isReady() const noexcept { return !runtimeNodes.empty(); }

private:
    friend class CircuitCompiler;
    struct RuntimeNode
    {
        NodeSpec spec;
        std::unique_ptr<ICircuitComponent> component;
        std::vector<float> buffer;
        std::vector<std::size_t> predecessors;
    };
    CircuitGraphDescription graphDescription;
    std::vector<RuntimeNode> runtimeNodes;
    std::size_t inputIndex {};
    std::size_t outputIndex {};
    std::size_t maximumBlockSize {};
    std::size_t latency {};
};

class CircuitCompiler
{
public:
    [[nodiscard]] static ValidationReport validate(const CircuitGraphDescription& graph);
    [[nodiscard]] static std::unique_ptr<CompiledCircuitGraph> compile(
        const CircuitGraphDescription& graph, const nts::dsp::ProcessSpec& spec,
        const NeuralModelBytes& neuralModels, ValidationReport& report);
};

[[nodiscard]] CircuitGraphDescription makeSimpleCircuit(const SimpleControls& controls);
[[nodiscard]] std::string serializeCircuit(const CircuitGraphDescription& graph);
[[nodiscard]] bool deserializeCircuit(std::string_view json, CircuitGraphDescription& graph,
                                      std::string& error);
} // namespace nts::circuit
