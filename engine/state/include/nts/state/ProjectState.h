#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nts::state
{
inline constexpr int currentSchemaVersion = 2;
inline constexpr std::string_view currentApplicationVersion = "0.10.0";

struct EngineState
{
    float inputGainDb {};
    float outputGainDb {};
    bool bypass {};
    std::vector<float> ampControls;

    bool operator==(const EngineState&) const = default;
};

struct DeviceState
{
    std::string backend;
    std::string inputDevice;
    std::string outputDevice;
    double sampleRate { 48000.0 };
    int bufferSize { 256 };
    std::vector<int> inputChannels;
    std::vector<int> outputChannels;

    bool operator==(const DeviceState&) const = default;
};

struct GraphState
{
    int latencySamples {};
    bool enabled { true };
    std::string physicalCircuitJson;

    bool operator==(const GraphState&) const = default;
};

struct UiState
{
    int width { 760 };
    int height { 520 };
    bool diagnosticsVisible { true };

    bool operator==(const UiState&) const = default;
};

struct AssetState
{
    std::vector<std::string> relativePaths;

    bool operator==(const AssetState&) const = default;
};

struct ProjectState
{
    int schemaVersion { currentSchemaVersion };
    std::string applicationVersion { currentApplicationVersion };
    EngineState engine;
    DeviceState device;
    GraphState graph;
    UiState ui;
    AssetState assets;

    bool operator==(const ProjectState&) const = default;
};

struct StateResult
{
    std::optional<ProjectState> state;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return state.has_value(); }
};

[[nodiscard]] std::string serialize(const ProjectState& state, bool pretty = true);
[[nodiscard]] StateResult deserialize(std::string_view json);
[[nodiscard]] bool validate(const ProjectState& state, std::string& error) noexcept;
[[nodiscard]] std::string normalizeAssetPath(const std::filesystem::path& projectFile,
                                             const std::filesystem::path& assetFile);
} // namespace nts::state
