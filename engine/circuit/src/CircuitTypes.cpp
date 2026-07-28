#include <nts/circuit/CircuitTypes.h>

#include <algorithm>

namespace nts::circuit
{
bool ValidationReport::isValid() const noexcept
{
    return std::none_of(messages.begin(), messages.end(), [](const auto& message)
    {
        return message.severity == ValidationMessage::Severity::error;
    });
}

std::string_view toString(NodeType type) noexcept
{
    switch (type)
    {
        case NodeType::input: return "input";
        case NodeType::filter: return "filter";
        case NodeType::triodeStage: return "triode-stage";
        case NodeType::toneStack: return "tone-stack";
        case NodeType::phaseInverter: return "phase-inverter";
        case NodeType::powerStage: return "power-stage";
        case NodeType::feedback: return "feedback";
        case NodeType::transformer: return "transformer";
        case NodeType::cabinet: return "cabinet";
        case NodeType::output: return "output";
    }
    return "filter";
}

std::string_view toString(ModelBackend backend) noexcept
{
    switch (backend)
    {
        case ModelBackend::staticTransfer: return "static-v1";
        case ModelBackend::graybox: return "graybox-v1";
        case ModelBackend::numerical: return "numerical-v1";
        case ModelBackend::neuralSurrogate: return "neural-surrogate-v1";
    }
    return "graybox-v1";
}

bool nodeTypeFromString(std::string_view text, NodeType& result) noexcept
{
    for (const auto type : { NodeType::input, NodeType::filter, NodeType::triodeStage,
                             NodeType::toneStack, NodeType::phaseInverter, NodeType::powerStage,
                             NodeType::feedback, NodeType::transformer, NodeType::cabinet,
                             NodeType::output })
        if (text == toString(type)) { result = type; return true; }
    return false;
}

bool backendFromString(std::string_view text, ModelBackend& result) noexcept
{
    for (const auto backend : { ModelBackend::staticTransfer, ModelBackend::graybox,
                                ModelBackend::numerical, ModelBackend::neuralSurrogate })
        if (text == toString(backend)) { result = backend; return true; }
    return false;
}

float parameterValue(const NodeSpec& node, std::string_view id, float fallback) noexcept
{
    const auto found = std::find_if(node.parameters.begin(), node.parameters.end(),
                                    [id](const auto& parameter) { return parameter.id == id; });
    return found == node.parameters.end() ? fallback : found->value;
}
} // namespace nts::circuit
