#include "nts/state/ProjectState.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace nts::state
{
namespace
{
juce::var integerArray(const std::vector<int>& values)
{
    juce::Array<juce::var> result;
    for (const auto value : values)
        result.add(value);
    return result;
}

juce::var stringArray(const std::vector<std::string>& values)
{
    juce::Array<juce::var> result;
    for (const auto& value : values)
        result.add(juce::String::fromUTF8(value.c_str()));
    return result;
}

std::vector<int> readIntegerArray(const juce::var& value)
{
    std::vector<int> result;
    if (const auto* array = value.getArray())
    {
        result.reserve(static_cast<std::size_t>(array->size()));
        for (const auto& item : *array)
            result.push_back(static_cast<int>(item));
    }
    return result;
}

std::vector<std::string> readStringArray(const juce::var& value)
{
    std::vector<std::string> result;
    if (const auto* array = value.getArray())
    {
        result.reserve(static_cast<std::size_t>(array->size()));
        for (const auto& item : *array)
            result.push_back(item.toString().toStdString());
    }
    return result;
}

juce::var migrateV0ToV1(const juce::var& source)
{
    auto migrated = source.clone();
    auto* root = migrated.getDynamicObject();
    root->setProperty("schemaVersion", 1);
    root->setProperty("applicationVersion", juce::String(currentApplicationVersion.data()));

    if (! root->hasProperty("engine"))
    {
        auto* engine = new juce::DynamicObject();
        engine->setProperty("inputGainDb", source.getProperty("inputGain", 0.0));
        engine->setProperty("outputGainDb", source.getProperty("outputGain", 0.0));
        engine->setProperty("bypass", source.getProperty("bypass", false));
        root->setProperty("engine", juce::var(engine));
    }

    if (! root->hasProperty("device")) root->setProperty("device", juce::var(new juce::DynamicObject()));
    if (! root->hasProperty("graph")) root->setProperty("graph", juce::var(new juce::DynamicObject()));
    if (! root->hasProperty("ui")) root->setProperty("ui", juce::var(new juce::DynamicObject()));
    if (! root->hasProperty("assets")) root->setProperty("assets", juce::var(new juce::DynamicObject()));
    return migrated;
}

juce::var toVar(const ProjectState& state)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("schemaVersion", state.schemaVersion);
    root->setProperty("applicationVersion", juce::String::fromUTF8(state.applicationVersion.c_str()));

    auto* engine = new juce::DynamicObject();
    engine->setProperty("inputGainDb", state.engine.inputGainDb);
    engine->setProperty("outputGainDb", state.engine.outputGainDb);
    engine->setProperty("bypass", state.engine.bypass);
    root->setProperty("engine", juce::var(engine));

    auto* device = new juce::DynamicObject();
    device->setProperty("backend", juce::String::fromUTF8(state.device.backend.c_str()));
    device->setProperty("inputDevice", juce::String::fromUTF8(state.device.inputDevice.c_str()));
    device->setProperty("outputDevice", juce::String::fromUTF8(state.device.outputDevice.c_str()));
    device->setProperty("sampleRate", state.device.sampleRate);
    device->setProperty("bufferSize", state.device.bufferSize);
    device->setProperty("inputChannels", integerArray(state.device.inputChannels));
    device->setProperty("outputChannels", integerArray(state.device.outputChannels));
    root->setProperty("device", juce::var(device));

    auto* graph = new juce::DynamicObject();
    graph->setProperty("latencySamples", state.graph.latencySamples);
    graph->setProperty("enabled", state.graph.enabled);
    root->setProperty("graph", juce::var(graph));

    auto* ui = new juce::DynamicObject();
    ui->setProperty("width", state.ui.width);
    ui->setProperty("height", state.ui.height);
    ui->setProperty("diagnosticsVisible", state.ui.diagnosticsVisible);
    root->setProperty("ui", juce::var(ui));

    auto* assets = new juce::DynamicObject();
    assets->setProperty("relativePaths", stringArray(state.assets.relativePaths));
    root->setProperty("assets", juce::var(assets));
    return juce::var(root);
}

ProjectState fromVar(const juce::var& root)
{
    ProjectState state;
    state.schemaVersion = static_cast<int>(root.getProperty("schemaVersion", 0));
    state.applicationVersion = root.getProperty("applicationVersion", "").toString().toStdString();

    const auto engine = root.getProperty("engine", {});
    state.engine.inputGainDb = static_cast<float>(static_cast<double>(engine.getProperty("inputGainDb", 0.0)));
    state.engine.outputGainDb = static_cast<float>(static_cast<double>(engine.getProperty("outputGainDb", 0.0)));
    state.engine.bypass = static_cast<bool>(engine.getProperty("bypass", false));

    const auto device = root.getProperty("device", {});
    state.device.backend = device.getProperty("backend", "").toString().toStdString();
    state.device.inputDevice = device.getProperty("inputDevice", "").toString().toStdString();
    state.device.outputDevice = device.getProperty("outputDevice", "").toString().toStdString();
    state.device.sampleRate = static_cast<double>(device.getProperty("sampleRate", 48000.0));
    state.device.bufferSize = static_cast<int>(device.getProperty("bufferSize", 256));
    state.device.inputChannels = readIntegerArray(device.getProperty("inputChannels", {}));
    state.device.outputChannels = readIntegerArray(device.getProperty("outputChannels", {}));

    const auto graph = root.getProperty("graph", {});
    state.graph.latencySamples = static_cast<int>(graph.getProperty("latencySamples", 0));
    state.graph.enabled = static_cast<bool>(graph.getProperty("enabled", true));

    const auto ui = root.getProperty("ui", {});
    state.ui.width = static_cast<int>(ui.getProperty("width", 760));
    state.ui.height = static_cast<int>(ui.getProperty("height", 520));
    state.ui.diagnosticsVisible = static_cast<bool>(ui.getProperty("diagnosticsVisible", true));

    const auto assets = root.getProperty("assets", {});
    state.assets.relativePaths = readStringArray(assets.getProperty("relativePaths", {}));
    return state;
}
} // namespace

std::string serialize(const ProjectState& state, bool pretty)
{
    return juce::JSON::toString(toVar(state), pretty).toStdString();
}

StateResult deserialize(std::string_view json)
{
    juce::var parsed;
    const auto parseResult = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())), parsed);
    if (parseResult.failed() || ! parsed.isObject())
        return { std::nullopt, "Invalid JSON project state" };

    const auto sourceVersion = static_cast<int>(parsed.getProperty("schemaVersion", 0));
    if (sourceVersion > currentSchemaVersion || sourceVersion < 0)
        return { std::nullopt, "Unsupported project schema version" };

    if (sourceVersion == 0)
        parsed = migrateV0ToV1(parsed);

    auto state = fromVar(parsed);
    std::string error;
    if (! validate(state, error))
        return { std::nullopt, std::move(error) };
    return { std::move(state), {} };
}

bool validate(const ProjectState& state, std::string& error) noexcept
{
    if (state.schemaVersion != currentSchemaVersion)
    {
        error = "Project schema is not current";
        return false;
    }
    if (state.applicationVersion.empty())
    {
        error = "Application version is missing";
        return false;
    }
    if (! std::isfinite(state.engine.inputGainDb) || state.engine.inputGainDb < -60.0f || state.engine.inputGainDb > 24.0f
        || ! std::isfinite(state.engine.outputGainDb) || state.engine.outputGainDb < -60.0f || state.engine.outputGainDb > 24.0f)
    {
        error = "Engine gain is outside the supported range";
        return false;
    }
    if (! std::isfinite(state.device.sampleRate) || state.device.sampleRate < 8000.0 || state.device.sampleRate > 384000.0)
    {
        error = "Device sample rate is invalid";
        return false;
    }
    if (state.device.bufferSize < 0 || state.device.bufferSize > 65536)
    {
        error = "Device buffer size is invalid";
        return false;
    }
    if (state.graph.latencySamples < 0)
    {
        error = "Graph latency cannot be negative";
        return false;
    }
    for (const auto& path : state.assets.relativePaths)
    {
        if (path.empty() || std::filesystem::path(path).is_absolute())
        {
            error = "Asset paths must be non-empty and project-relative";
            return false;
        }
    }
    error.clear();
    return true;
}

std::string normalizeAssetPath(const std::filesystem::path& projectFile,
                               const std::filesystem::path& assetFile)
{
    const auto projectDirectory = std::filesystem::absolute(projectFile).parent_path().lexically_normal();
    const auto absoluteAsset = std::filesystem::absolute(assetFile).lexically_normal();
    std::error_code error;
    const auto relative = std::filesystem::relative(absoluteAsset, projectDirectory, error);
    if (! error && ! relative.empty())
        return relative.generic_string();
    return absoluteAsset.generic_string();
}
} // namespace nts::state
