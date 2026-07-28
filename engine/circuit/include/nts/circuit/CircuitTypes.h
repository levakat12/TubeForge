#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nts::circuit
{
inline constexpr int currentCircuitSchemaVersion = 1;

enum class NodeType
{
    input, filter, triodeStage, toneStack, phaseInverter, powerStage,
    feedback, transformer, cabinet, output
};

enum class ModelBackend { staticTransfer, graybox, numerical, neuralSurrogate };
enum class PowerTopology { singleEnded, pushPullClassA, pushPullClassAB };

struct ParameterDescriptor
{
    std::string id;
    float minimum {};
    float maximum { 1.0f };
    float defaultValue {};
    std::string unit;
};

struct NodeParameter { std::string id; float value {}; };

struct PortSchema
{
    std::size_t inputs { 1 };
    std::size_t outputs { 1 };
    std::size_t channels { 1 };
};

struct TubeDefinition
{
    std::string id;
    std::string family;
    float amplificationFactor {};
    float plateResistanceOhms {};
    float transconductanceSiemens {};
    float nominalPlateVoltage {};
    float maximumPlateVoltage {};
    float nominalCathodeResistanceOhms {};
    float nominalPlateResistanceOhms {};
};

struct NodeSpec
{
    std::string id;
    NodeType type { NodeType::filter };
    std::string modelId;
    ModelBackend backend { ModelBackend::graybox };
    PortSchema ports;
    std::vector<NodeParameter> parameters;
};

struct Connection
{
    std::string sourceNode;
    std::size_t sourcePort {};
    std::string targetNode;
    std::size_t targetPort {};
    bool explicitFeedback {};
};

struct CircuitGraphDescription
{
    int schemaVersion { currentCircuitSchemaVersion };
    std::string id { "tubeforge.circuit.default" };
    std::string name { "TubeForge Circuit" };
    std::vector<NodeSpec> nodes;
    std::vector<Connection> connections;
};

struct ComponentTelemetry
{
    float inputRms {};
    float outputRms {};
    float plateCurrentMilliamps {};
    float supplyVoltage {};
    float headroomDb {};
    float clippingBalance {};
    float sagPercent {};
    float normalizedOperatingPoint {};
    float stageGainDb {};
    float clippingContribution {};
    float harmonicEstimate {};
    bool valid { true };
};

struct NodeTelemetry
{
    std::string nodeId;
    NodeType type {};
    ComponentTelemetry values;
};

struct ValidationMessage
{
    enum class Severity { warning, error };
    Severity severity { Severity::error };
    std::string nodeId;
    std::string message;
};

struct ValidationReport
{
    std::vector<ValidationMessage> messages;
    [[nodiscard]] bool isValid() const noexcept;
};

struct SimpleControls
{
    float tubeCharacter { 0.5f };
    float headroom { 0.5f };
    float breakup { 0.35f };
    float tightness { 0.5f };
    float sag { 0.25f };
    float powerSize { 0.6f };
    float feedback { 0.3f };
    float cabinet { 0.5f };
};

[[nodiscard]] std::string_view toString(NodeType type) noexcept;
[[nodiscard]] std::string_view toString(ModelBackend backend) noexcept;
[[nodiscard]] bool nodeTypeFromString(std::string_view text, NodeType& result) noexcept;
[[nodiscard]] bool backendFromString(std::string_view text, ModelBackend& result) noexcept;
[[nodiscard]] float parameterValue(const NodeSpec& node, std::string_view id, float fallback) noexcept;
} // namespace nts::circuit
