#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nts::state
{
inline constexpr int currentSchemaVersion = 4;
inline constexpr std::string_view currentApplicationVersion = "0.10.0";
/** Slots the pedalboard has.

    Duplicates `nts::pedals::slotCount` deliberately: this library is the project format and
    does not depend on the audio engine. A static assertion in the processor holds the two
    together, so they cannot drift without the build failing.
*/
inline constexpr std::size_t maximumPedalSlots = 4;

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
    /** Project-relative paths to user cabinet impulse responses, empty when the built-in
        response is in use. Held per slot rather than in relativePaths because which slot a
        response belongs to is part of the setting, not incidental. Added in schema 3.
    */
    std::string cabinetIrPathA;
    std::string cabinetIrPathB;

    bool operator==(const AssetState&) const = default;
};

/** One pedal slot, as saved.

    A block of its own rather than more entries in `EngineState::ampControls`, for two
    reasons: a slot carries a file path as well as numbers, and the flat control vector is
    capped at 64 entries by validation -- four slots would have taken it past that and turned
    a schema addition into a change to a limit that exists for a different reason.

    `kind` is the PedalKind enumerator. It is held as an int so this header does not have to
    depend on the pedal engine, and it is range-checked on the way in.
*/
struct PedalSlotState
{
    int kind {};
    bool bypassed {};
    float drive { 5.0f };
    float tone { 5.0f };
    float levelDb {};
    float mix { 100.0f };
    /// The artifact directory staged into this slot, empty when there is no capture.
    std::string modelPath;

    bool operator==(const PedalSlotState&) const = default;
};

struct PedalboardState
{
    /// Empty means no board at all, which is what a project written before schema 4 has and
    /// what a rig of amplifier and cabinet alone still writes.
    std::vector<PedalSlotState> slots;

    bool operator==(const PedalboardState&) const = default;
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
    PedalboardState pedalboard;

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
