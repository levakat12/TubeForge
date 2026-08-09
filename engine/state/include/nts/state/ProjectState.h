#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nts::state
{
inline constexpr int currentSchemaVersion = 6;
inline constexpr std::string_view currentApplicationVersion = "0.10.0";
/** Slots the pedalboard has.

    Duplicates `nts::pedals::slotCount` deliberately: this library is the project format and
    does not depend on the audio engine. A static assertion in the processor holds the two
    together, so they cannot drift without the build failing.
*/
inline constexpr std::size_t maximumPedalSlots = 4;

/** Ceiling on `EngineState::ampControls`, enforced by `validate`.

    Deliberately loose, and load-bearing in a way that is easy to miss: the plug-in writes one
    entry per amplifier parameter it saves, so the day that list grows past this bound **every
    saved project stops loading** -- not a corrupted file and not a warning, but a valid project
    rejected as "Amp control state is invalid", caused by somebody adding a knob.

    Named here rather than left as a literal in `validate` so the writing side can be checked
    against it at compile time instead of by whoever next opens an old project.
*/
inline constexpr std::size_t maximumAmpControls = 64;

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
    /** SHA-256 of each response's bytes, empty when the built-in cabinet is in use. Schema 6.

        A path alone cannot tell "the file has moved" apart from "the file at that path is not the
        one you saved", and the second is the case that quietly changes somebody's mix: an impulse
        pack updated in place, or a file whose name was reused. Storing the digest turns that into
        something the plug-in can say out loud.

        Empty for a project written before schema 6, and empty is not an error -- it means the
        project cannot answer the question, which is different from answering it wrongly.
    */
    std::string cabinetIrHashA;
    std::string cabinetIrHashB;

    bool operator==(const AssetState&) const = default;
};

/** One pedal slot, as saved.

    A block of its own rather than more entries in `EngineState::ampControls`, for two
    reasons: a slot carries a file path as well as numbers, and the flat control vector is
    capped at 64 entries by validation -- four slots would have taken it past that and turned
    a schema addition into a change to a limit that exists for a different reason.

    `kind` is an index into the pedal engine's model table. It is held as an int so this header
    does not have to depend on the pedal engine, and it is range-checked on the way in. The
    first seven indices are the built-in archetypes and never move, so a project saved before
    the table grew recalls the same board.

    `auxA` and `auxB` are the model-specific voicing controls, and default to the neutral 5 --
    which is what a project saved before they existed gets, and is exactly what the archetypes
    that ignore them would have done anyway.
*/
struct PedalSlotState
{
    int kind {};
    bool bypassed {};
    float drive { 5.0f };
    float tone { 5.0f };
    float levelDb {};
    float mix { 100.0f };
    float auxA { 5.0f };
    float auxB { 5.0f };
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

/** Ceiling on `AutoMatchState::heldValues`, enforced by `validate`.

    Sized well above the owned set the plug-in actually has (47 at the time of writing) so that a
    control added to Auto Match is not also a project-format change. It exists to stop a corrupt
    or hostile file asking for an unbounded allocation, not to encode the current list -- which is
    why it is deliberately not `maximumAmpControls`, whose bound means something different.
*/
inline constexpr std::size_t maximumAutoMatchValues = 128;

/** What the song analyzer was holding when the project was saved.

    Held as an opaque block of numbers rather than as named controls, and the layering is the
    reason: this library is the project format and knows nothing about the plug-in's parameters.
    The plug-in writes one value per entry of its own owned table, in that table's order, and
    reads them back the same way -- so the same append-only rule that governs `ampControls`
    governs this, and for the same reason.

    `releasedIndices` are positions in that table the user had taken back. Stored as indices
    rather than as a bit mask because a mask is a number whose meaning is invisible in the file,
    and this format is meant to be readable.

    Empty is the normal state and is what every project written before schema 5 has: Auto Match
    off, or on but not holding anything.
*/
struct AutoMatchState
{
    /// True when a matched rig was being held. False means the values below are meaningless.
    bool holding {};
    /** Whether the match also took the effects chain, which decides how much of the table was
        held rather than merely recorded.

        Saved rather than derived from the values. A pedal bypass reads the same whether the
        match set it or the player did, so inferring it would sometimes conclude that a matched
        rig owned a board it never touched.
    */
    bool isolatedChain {};
    /// One value per owned control, in the plug-in's own table order.
    std::vector<float> heldValues;
    /// Positions in that table the user had taken back before saving.
    std::vector<int> releasedIndices;

    bool operator==(const AutoMatchState&) const = default;
};

/** Ceiling on `CabinetState::controls`, enforced by `validate`.

    A block of its own rather than more room in `maximumAmpControls`, and the difference is the
    point: that bound is nearly spent, and the cabinet's controls are not amplifier controls. See
    the note above `cabinetControlIds` in the plug-in. Sized well above the thirteen the cabinet
    currently has so that adding a control is not also a project-format change.
*/
inline constexpr std::size_t maximumCabinetControls = 48;

/** The cabinet stage's controls, added in schema 6.

    Opaque numbers in the plug-in's own list order, exactly like `EngineState::ampControls` and
    for the same layering reason: this library is the file format and knows nothing about
    parameters. The same append-only rule governs it.

    **Empty is the normal state for anything written before schema 6**, and it has to restore to
    the cabinet those projects actually had -- which is why every one of the plug-in's defaults
    for these controls is the value the field effectively held when it was unreachable. An empty
    block is therefore not a gap to be guessed at; it is a complete description.
*/
struct CabinetState
{
    std::vector<float> controls;

    bool operator==(const CabinetState&) const = default;
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
    AutoMatchState autoMatch;
    CabinetState cabinet;

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
