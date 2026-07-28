#include <nts/circuit/CircuitGraph.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <unordered_set>

namespace nts::circuit
{
namespace
{
void addError(ValidationReport& report, std::string node, std::string message)
{
    report.messages.push_back({ ValidationMessage::Severity::error, std::move(node), std::move(message) });
}

void addWarning(ValidationReport& report, std::string node, std::string message)
{
    report.messages.push_back({ ValidationMessage::Severity::warning, std::move(node), std::move(message) });
}

std::vector<std::size_t> topologicalOrder(const CircuitGraphDescription& graph,
                                          const std::unordered_map<std::string, std::size_t>& indices)
{
    std::vector<std::size_t> degree(graph.nodes.size());
    std::vector<std::vector<std::size_t>> outgoing(graph.nodes.size());
    for (const auto& edge : graph.connections)
    {
        if (edge.explicitFeedback) continue;
        const auto source = indices.find(edge.sourceNode);
        const auto target = indices.find(edge.targetNode);
        if (source == indices.end() || target == indices.end()) continue;
        outgoing[source->second].push_back(target->second);
        ++degree[target->second];
    }
    std::queue<std::size_t> ready;
    for (std::size_t i = 0; i < degree.size(); ++i) if (degree[i] == 0) ready.push(i);
    std::vector<std::size_t> order;
    while (!ready.empty())
    {
        const auto node = ready.front(); ready.pop(); order.push_back(node);
        for (const auto target : outgoing[node]) if (--degree[target] == 0) ready.push(target);
    }
    return order;
}

juce::var nodeToVar(const NodeSpec& node)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("id", juce::String(node.id));
    object->setProperty("type", juce::String(toString(node.type).data()));
    object->setProperty("modelId", juce::String(node.modelId));
    object->setProperty("backend", juce::String(toString(node.backend).data()));
    object->setProperty("channels", static_cast<int>(node.ports.channels));
    object->setProperty("inputPorts", static_cast<int>(node.ports.inputs));
    object->setProperty("outputPorts", static_cast<int>(node.ports.outputs));
    juce::Array<juce::var> parameters;
    for (const auto& parameter : node.parameters)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty("id", juce::String(parameter.id));
        entry->setProperty("value", parameter.value);
        parameters.add(juce::var(entry));
    }
    object->setProperty("parameters", parameters);
    return juce::var(object);
}
} // namespace

ValidationReport CircuitCompiler::validate(const CircuitGraphDescription& graph)
{
    ValidationReport report;
    if (graph.schemaVersion < 0 || graph.schemaVersion > currentCircuitSchemaVersion)
        addError(report, {}, "Unsupported circuit schema version");
    if (graph.nodes.empty()) addError(report, {}, "Circuit has no nodes");

    std::unordered_map<std::string, std::size_t> indices;
    std::size_t inputCount = 0, outputCount = 0;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index)
    {
        const auto& node = graph.nodes[index];
        if (node.id.empty()) addError(report, {}, "Every node needs a stable id");
        else if (!indices.emplace(node.id, index).second) addError(report, node.id, "Duplicate node id");
        inputCount += node.type == NodeType::input ? 1U : 0U;
        outputCount += node.type == NodeType::output ? 1U : 0U;
        const auto expectedInputs = node.type == NodeType::input ? 0U : 1U;
        const auto expectedOutputs = node.type == NodeType::output ? 0U : 1U;
        if (node.ports.inputs != expectedInputs || node.ports.outputs != expectedOutputs || node.ports.channels != 1)
            addError(report, node.id, "Node port schema is incompatible; stereo uses independent mono graphs");
        if (node.type == NodeType::triodeStage && TubeLibrary::find(node.modelId) == nullptr)
            addError(report, node.id, "Unknown electrical tube definition: " + node.modelId);
        if (node.type == NodeType::triodeStage)
            if (const auto* tube = TubeLibrary::find(node.modelId))
            {
                const auto voltage = parameterValue(node, "plate-voltage-v", tube->nominalPlateVoltage);
                if (voltage > tube->maximumPlateVoltage * 0.92f)
                    addWarning(report, node.id, "Plate supply is near or beyond this virtual tube model's validated range");
            }

        const auto schema = parameterSchema(node.type);
        std::unordered_set<std::string> parameterIds;
        for (const auto& parameter : node.parameters)
        {
            if (!parameterIds.insert(parameter.id).second) addError(report, node.id, "Duplicate parameter: " + parameter.id);
            const auto descriptor = std::find_if(schema.begin(), schema.end(), [&](const auto& item) { return item.id == parameter.id; });
            if (descriptor == schema.end()) addWarning(report, node.id, "Unknown parameter preserved: " + parameter.id);
            else if (!std::isfinite(parameter.value) || parameter.value < descriptor->minimum || parameter.value > descriptor->maximum)
                addError(report, node.id, "Parameter outside valid range: " + parameter.id);
        }
    }
    if (inputCount != 1) addError(report, {}, "Circuit requires exactly one input node");
    if (outputCount != 1) addError(report, {}, "Circuit requires exactly one output node");

    for (const auto& edge : graph.connections)
    {
        const auto source = indices.find(edge.sourceNode);
        const auto target = indices.find(edge.targetNode);
        if (source == indices.end()) addError(report, edge.sourceNode, "Connection source does not exist");
        if (target == indices.end()) addError(report, edge.targetNode, "Connection target does not exist");
        if (source == indices.end() || target == indices.end()) continue;
        if (edge.sourcePort != 0 || edge.targetPort != 0) addError(report, edge.targetNode, "Only port zero is available");
        if (edge.explicitFeedback && graph.nodes[target->second].type != NodeType::feedback)
            addError(report, edge.targetNode, "Explicit feedback edges must terminate at a feedback node");
        if (graph.nodes[source->second].type == NodeType::output)
            addError(report, edge.sourceNode, "Output nodes cannot drive forward connections");
        if (graph.nodes[target->second].type == NodeType::input)
            addError(report, edge.targetNode, "Input nodes cannot receive forward connections");
    }

    const auto order = topologicalOrder(graph, indices);
    if (order.size() != graph.nodes.size())
        addError(report, {}, "Circuit contains a cycle outside an explicit feedback path");

    if (report.isValid() && inputCount == 1 && outputCount == 1)
    {
        const auto input = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const auto& node) { return node.type == NodeType::input; });
        const auto output = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const auto& node) { return node.type == NodeType::output; });
        std::unordered_set<std::size_t> reachable { static_cast<std::size_t>(std::distance(graph.nodes.begin(), input)) };
        for (const auto node : order)
            for (const auto& edge : graph.connections)
                if (!edge.explicitFeedback && edge.sourceNode == graph.nodes[node].id && reachable.contains(node))
                    reachable.insert(indices.at(edge.targetNode));
        if (!reachable.contains(static_cast<std::size_t>(std::distance(graph.nodes.begin(), output))))
            addError(report, output->id, "Output is not reachable from input");
        std::unordered_set<std::size_t> reachesOutput { static_cast<std::size_t>(std::distance(graph.nodes.begin(), output)) };
        for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator)
            for (const auto& edge : graph.connections)
                if (!edge.explicitFeedback && edge.targetNode == graph.nodes[*iterator].id
                    && reachesOutput.contains(*iterator))
                    reachesOutput.insert(indices.at(edge.sourceNode));
        for (std::size_t index = 0; index < graph.nodes.size(); ++index)
            if (graph.nodes[index].type == NodeType::cabinet
                && (!reachable.contains(index) || !reachesOutput.contains(index)))
                addError(report, graph.nodes[index].id, "Cabinet must be placed on the active input-to-output path");
    }
    return report;
}

std::unique_ptr<CompiledCircuitGraph> CircuitCompiler::compile(
    const CircuitGraphDescription& graph, const nts::dsp::ProcessSpec& spec,
    const NeuralModelBytes& neuralModels, ValidationReport& report)
{
    report = validate(graph);
    for (const auto& node : graph.nodes)
        if (node.backend == ModelBackend::neuralSurrogate && !neuralModels.contains(node.id))
            addError(report, node.id, "Neural surrogate bytes were not supplied");
    if (!report.isValid()) return {};

    std::unordered_map<std::string, std::size_t> originalIndices;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) originalIndices.emplace(graph.nodes[i].id, i);
    const auto order = topologicalOrder(graph, originalIndices);
    std::vector<std::size_t> runtimeIndex(graph.nodes.size());
    for (std::size_t i = 0; i < order.size(); ++i) runtimeIndex[order[i]] = i;

    auto result = std::make_unique<CompiledCircuitGraph>();
    result->graphDescription = graph;
    result->graphDescription.schemaVersion = currentCircuitSchemaVersion;
    result->maximumBlockSize = spec.maximumBlockSize;
    result->runtimeNodes.reserve(graph.nodes.size());
    std::vector<std::size_t> pathLatency(graph.nodes.size());
    for (std::size_t runtime = 0; runtime < order.size(); ++runtime)
    {
        const auto original = order[runtime];
        const auto& node = graph.nodes[original];
        std::unique_ptr<INonlinearComponentModel> hybrid;
        if (node.backend == ModelBackend::neuralSurrogate)
        {
            auto neural = std::make_unique<PackedNeuralComponentModel>();
            std::string error;
            if (!neural->load(neuralModels.at(node.id), error))
            {
                addError(report, node.id, "Could not load neural surrogate: " + error); return {};
            }
            if (neural->modelSampleRate() != static_cast<int>(std::lround(spec.sampleRate)))
            {
                addError(report, node.id, "Neural surrogate sample rate does not match the circuit runtime"); return {};
            }
            hybrid = std::move(neural);
        }
        CompiledCircuitGraph::RuntimeNode entry;
        entry.spec = node;
        entry.component = createComponent(node, std::move(hybrid));
        entry.buffer.resize(spec.maximumBlockSize);
        for (const auto& edge : graph.connections)
            if (!edge.explicitFeedback && edge.targetNode == node.id)
                entry.predecessors.push_back(runtimeIndex[originalIndices.at(edge.sourceNode)]);
        entry.component->prepare({ spec.sampleRate, spec.maximumBlockSize, 1 });
        entry.component->reset();
        auto preceding = std::size_t {};
        for (const auto predecessor : entry.predecessors) preceding = std::max(preceding, pathLatency[predecessor]);
        pathLatency[runtime] = preceding + entry.component->latencySamples();
        if (node.type == NodeType::input) result->inputIndex = runtime;
        if (node.type == NodeType::output) result->outputIndex = runtime;
        result->runtimeNodes.push_back(std::move(entry));
    }
    result->latency = pathLatency[result->outputIndex];
    return result;
}

void CompiledCircuitGraph::reset() noexcept
{
    for (auto& node : runtimeNodes) node.component->reset();
}

void CompiledCircuitGraph::process(std::span<const float> input, std::span<float> output) noexcept
{
    const auto count = std::min({ input.size(), output.size(), maximumBlockSize });
    if (runtimeNodes.empty() || count == 0) { std::fill(output.begin(), output.end(), 0.0f); return; }
    for (std::size_t index = 0; index < runtimeNodes.size(); ++index)
    {
        auto& node = runtimeNodes[index];
        auto buffer = std::span<float>(node.buffer).first(count);
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        if (index == inputIndex) std::copy_n(input.begin(), count, buffer.begin());
        else
            for (const auto predecessor : node.predecessors)
                for (std::size_t sample = 0; sample < count; ++sample)
                    buffer[sample] += runtimeNodes[predecessor].buffer[sample];
        node.component->process(buffer);
    }
    std::copy_n(runtimeNodes[outputIndex].buffer.begin(), count, output.begin());
    if (output.size() > count) std::fill(output.begin() + static_cast<std::ptrdiff_t>(count), output.end(), 0.0f);
}

std::vector<NodeTelemetry> CompiledCircuitGraph::telemetrySnapshot() const
{
    std::vector<NodeTelemetry> result;
    result.reserve(runtimeNodes.size());
    for (const auto& node : runtimeNodes) result.push_back({ node.spec.id, node.spec.type, node.component->telemetry() });
    return result;
}

CircuitGraphDescription makeSimpleCircuit(const SimpleControls& raw)
{
    const auto tubeCharacter = std::clamp(raw.tubeCharacter, 0.0f, 1.0f);
    const auto headroom = std::clamp(raw.headroom, 0.0f, 1.0f);
    const auto breakup = std::clamp(raw.breakup, 0.0f, 1.0f);
    const auto tightness = std::clamp(raw.tightness, 0.0f, 1.0f);
    const auto sag = std::clamp(raw.sag, 0.0f, 1.0f);
    const auto powerSize = std::clamp(raw.powerSize, 0.0f, 1.0f);
    const auto feedback = std::clamp(raw.feedback, 0.0f, 1.0f);
    const auto cabinet = std::clamp(raw.cabinet, 0.0f, 1.0f);
    const auto tube = tubeCharacter < 0.33f ? "tube.12au7.v1" : tubeCharacter < 0.68f ? "tube.12at7.v1" : "tube.12ax7.v1";
    CircuitGraphDescription graph;
    graph.nodes = {
        { "input", NodeType::input, "linear.input.v1", ModelBackend::staticTransfer, { 0, 1, 1 }, { { "gain", 1.0f } } },
        { "tight-filter", NodeType::filter, "rc.highpass.v1", ModelBackend::graybox, {}, { { "cutoff-hz", 45.0f + tightness * 135.0f }, { "high-pass", 1.0f } } },
        { "v1", NodeType::triodeStage, tube, ModelBackend::graybox, {}, { { "drive", 0.15f + breakup * 0.8f }, { "bias", 0.42f + tubeCharacter * 0.16f }, { "plate-voltage-v", 170.0f + headroom * 160.0f }, { "cathode-resistance-ohm", 820.0f + headroom * 1700.0f } } },
        { "tone", NodeType::toneStack, "passive.fmv.v1", ModelBackend::graybox, {}, { { "bass", 0.55f }, { "middle", 0.58f }, { "treble", 0.52f }, { "slope-resistance-ohm", 100000.0f }, { "bass-cap-f", 22.0e-9f }, { "mid-cap-f", 22.0e-9f }, { "treble-cap-f", 250.0e-12f } } },
        { "pi", NodeType::phaseInverter, "long-tail-pair.v1", ModelBackend::graybox, {}, { { "drive", 0.25f + breakup * 0.55f }, { "imbalance", 0.02f + tubeCharacter * 0.08f } } },
        { "power", NodeType::powerStage, powerSize < 0.45f ? "power.6v6.pp.v1" : "power.el34.pp.v1", ModelBackend::graybox, {}, { { "drive", 0.25f + breakup * 0.7f }, { "sag", sag }, { "feedback", feedback * 0.75f }, { "supply-voltage-v", 250.0f + powerSize * 300.0f }, { "topology", powerSize < 0.2f ? 0.0f : 2.0f }, { "tube-count", powerSize < 0.5f ? 2.0f : 4.0f }, { "bias", 0.5f + (1.0f - breakup) * 0.2f }, { "load-ohm", 5000.0f - powerSize * 1800.0f }, { "damping", feedback }, { "saturation", 0.7f - powerSize * 0.4f }, { "supply-attack-ms", 4.0f + sag * 12.0f }, { "supply-recovery-ms", 35.0f + sag * 130.0f }, { "rectifier-stiffness", 1.0f - sag * 0.75f }, { "ripple", sag * 0.08f } } },
        { "feedback", NodeType::feedback, "nfb.presence.v1", ModelBackend::graybox, {}, { { "amount", feedback * 0.75f }, { "presence", 0.5f } } },
        { "transformer", NodeType::transformer, "transformer.output.v1", ModelBackend::graybox, {}, { { "saturation", 0.7f - powerSize * 0.45f }, { "turns-ratio", 12.0f + powerSize * 22.0f }, { "leakage", 0.4f - powerSize * 0.2f }, { "damping", feedback } } },
        { "cabinet", NodeType::cabinet, "cabinet.reactive.v1", ModelBackend::graybox, {}, { { "resonance", cabinet }, { "brightness", 1.0f - cabinet * 0.45f } } },
        { "output", NodeType::output, "linear.output.v1", ModelBackend::staticTransfer, { 1, 0, 1 }, { { "gain", 0.72f } } }
    };
    for (std::size_t index = 1; index < graph.nodes.size(); ++index)
        graph.connections.push_back({ graph.nodes[index - 1].id, 0, graph.nodes[index].id, 0, false });
    return graph;
}

std::string serializeCircuit(const CircuitGraphDescription& graph)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("schemaVersion", currentCircuitSchemaVersion);
    root->setProperty("id", juce::String(graph.id));
    root->setProperty("name", juce::String(graph.name));
    juce::Array<juce::var> nodes;
    for (const auto& node : graph.nodes) nodes.add(nodeToVar(node));
    root->setProperty("nodes", nodes);
    juce::Array<juce::var> connections;
    for (const auto& connection : graph.connections)
    {
        auto* edge = new juce::DynamicObject();
        edge->setProperty("sourceNode", juce::String(connection.sourceNode));
        edge->setProperty("sourcePort", static_cast<int>(connection.sourcePort));
        edge->setProperty("targetNode", juce::String(connection.targetNode));
        edge->setProperty("targetPort", static_cast<int>(connection.targetPort));
        edge->setProperty("explicitFeedback", connection.explicitFeedback);
        connections.add(juce::var(edge));
    }
    root->setProperty("connections", connections);
    return juce::JSON::toString(juce::var(root), true).toStdString();
}

bool deserializeCircuit(std::string_view json, CircuitGraphDescription& graph, std::string& error)
{
    juce::var parsed;
    const auto parseResult = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())), parsed);
    if (parseResult.failed() || !parsed.isObject()) { error = parseResult.getErrorMessage().toStdString(); return false; }
    const auto* root = parsed.getDynamicObject();
    const auto schema = static_cast<int>(root->getProperty("schemaVersion"));
    if (schema < 0 || schema > currentCircuitSchemaVersion) { error = "Unsupported circuit schema version"; return false; }
    CircuitGraphDescription candidate;
    candidate.schemaVersion = currentCircuitSchemaVersion;
    candidate.id = root->getProperty("id").toString().toStdString();
    candidate.name = root->getProperty("name").toString().toStdString();
    const auto* nodes = root->getProperty("nodes").getArray();
    const auto* connections = root->getProperty("connections").getArray();
    if (nodes == nullptr || connections == nullptr) { error = "Circuit JSON needs nodes and connections arrays"; return false; }
    for (const auto& value : *nodes)
    {
        const auto* object = value.getDynamicObject();
        if (object == nullptr) { error = "Invalid node entry"; return false; }
        NodeSpec node;
        node.id = object->getProperty("id").toString().toStdString();
        const auto typeText = object->getProperty("type").toString().toStdString();
        const auto backendText = object->getProperty("backend").toString().toStdString();
        if (!nodeTypeFromString(typeText, node.type) || !backendFromString(backendText, node.backend))
        { error = "Unknown stable node type or backend id"; return false; }
        node.modelId = object->getProperty("modelId").toString().toStdString();
        const auto channels = static_cast<int>(object->getProperty("channels"));
        node.ports.channels = static_cast<std::size_t>(std::max(1, channels));
        node.ports.inputs = static_cast<std::size_t>(std::max(0, static_cast<int>(object->getProperty("inputPorts"))));
        node.ports.outputs = static_cast<std::size_t>(std::max(0, static_cast<int>(object->getProperty("outputPorts"))));
        if (schema == 0 && !object->hasProperty("inputPorts"))
        {
            node.ports.inputs = node.type == NodeType::input ? 0U : 1U;
            node.ports.outputs = node.type == NodeType::output ? 0U : 1U;
        }
        if (const auto* parameters = object->getProperty("parameters").getArray())
            for (const auto& parameterValueVar : *parameters)
                if (const auto* parameter = parameterValueVar.getDynamicObject())
                    node.parameters.push_back({ parameter->getProperty("id").toString().toStdString(),
                                                static_cast<float>(parameter->getProperty("value")) });
        candidate.nodes.push_back(std::move(node));
    }
    for (const auto& value : *connections)
    {
        const auto* object = value.getDynamicObject();
        if (object == nullptr) { error = "Invalid connection entry"; return false; }
        candidate.connections.push_back({ object->getProperty("sourceNode").toString().toStdString(),
                                          static_cast<std::size_t>(static_cast<int>(object->getProperty("sourcePort"))),
                                          object->getProperty("targetNode").toString().toStdString(),
                                          static_cast<std::size_t>(static_cast<int>(object->getProperty("targetPort"))),
                                          static_cast<bool>(object->getProperty("explicitFeedback")) });
    }
    const auto validation = CircuitCompiler::validate(candidate);
    if (!validation.isValid())
    {
        error = validation.messages.empty() ? "Invalid circuit" : validation.messages.front().message; return false;
    }
    graph = std::move(candidate); error.clear(); return true;
}
} // namespace nts::circuit
