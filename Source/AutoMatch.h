#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

/** Auto Match: the song analyzer holds the rig, and warns before the user fights it.

    See `docs/auto-match-plan.md` for the whole design. This header is the one place the *owned
    set* is written down -- the controls a matched rig actually sets, and therefore the controls
    a manual edit has to be warned about. It is deliberately separate from `ParameterIds` in
    `PluginProcessorInternal.h`, which is private to the processor translation units: the editor
    pages have to mark their own controls as owned, so this list has to be reachable from them.

    The ids here are string literals rather than references to `ParameterIds`, and that
    duplication is checked rather than trusted: the processor resolves every one of them through
    `AudioProcessorValueTreeState::getParameter` at construction and asserts on a null, so an id
    that does not name a real parameter fails at startup instead of silently never being owned.
*/
namespace tf::automatch
{
/** Where a rig stands with respect to the analyzer.

    `armed` and `holding` are distinct because the switch being on is not the same as there
    being an analysis to hold: a user who turns Auto Match on before importing a song has asked
    for something that has not happened yet, and no control should be guarded in the meantime.
*/
enum class State
{
    off,
    /// On, but no matched rig has been applied yet. Nothing is owned.
    armed,
    /// A matched rig is applied and every owned control is guarded.
    holding,
    /// Holding, with at least one control handed back to the user.
    overridden
};

struct OwnedParameter
{
    std::string_view id;
    /// Heading this control appears under in the Auto Match panel.
    std::string_view group;
    /** Only owned when the candidate was applied with Isolate chain on.

        The pedals and the two sends are not part of a matched rig -- the offline render never
        contained them. `applyRecoveredRig` touches them only when asked to isolate the chain,
        so guarding them unconditionally would warn about a control Auto Match never wrote.
    */
    bool chainOnly {};
};

/** Every control a matched rig writes, in the order `applyRecoveredRig` writes them.

    Deliberately **not** here, and each for a reason: `output` and `bypass` are level and
    monitoring rather than tone; `performanceTier` is a statement about the machine, not about
    the sound; `tunerMute`, `tunerReference`, the pedal voicing controls other than bypass, and
    everything on the Circuit and Neural pages are outside what the reconstruction search fits.
    Auto Match must never guard a control it did not compute.
*/
inline constexpr std::array owned {
    // Core tone -- the Amplifier page's seven knobs.
    OwnedParameter { "gain", "Core tone" },
    OwnedParameter { "bass", "Core tone" },
    OwnedParameter { "mid", "Core tone" },
    OwnedParameter { "treble", "Core tone" },
    OwnedParameter { "presence", "Core tone" },
    OwnedParameter { "resonance", "Core tone" },
    OwnedParameter { "master", "Core tone" },
    // Which amplifier, and on which instrument.
    OwnedParameter { "instrument", "Voicing" },
    OwnedParameter { "topology", "Voicing" },
    OwnedParameter { "panelSwitch", "Voicing" },
    OwnedParameter { "engineMode", "Voicing" },
    // Everything ahead of the distortion.
    OwnedParameter { "input", "Front end" },
    OwnedParameter { "lowCut", "Front end" },
    OwnedParameter { "highCut", "Front end" },
    OwnedParameter { "tightness", "Front end" },
    OwnedParameter { "pickEmphasis", "Front end" },
    OwnedParameter { "bias", "Front end" },
    // The preamp itself, and the rate it runs at.
    OwnedParameter { "stage1", "Preamp stages" },
    OwnedParameter { "stage2", "Preamp stages" },
    OwnedParameter { "stage3", "Preamp stages" },
    OwnedParameter { "stage4", "Preamp stages" },
    OwnedParameter { "oversampling", "Preamp stages" },
    // Power section.
    OwnedParameter { "sag", "Power section" },
    OwnedParameter { "feedback", "Power section" },
    OwnedParameter { "loudnessMatch", "Power section" },
    // The bi-amp split.
    OwnedParameter { "crossover", "Bi-amp" },
    OwnedParameter { "cleanBlend", "Bi-amp" },
    OwnedParameter { "dryBlend", "Bi-amp" },
    OwnedParameter { "lowBandDrive", "Bi-amp" },
    OwnedParameter { "lowBandLevel", "Bi-amp" },
    OwnedParameter { "highBandLevel", "Bi-amp" },
    // The gate, which a matched rig sets from the reference's noise floor.
    OwnedParameter { "gateEnabled", "Noise gate" },
    OwnedParameter { "gateThreshold", "Noise gate" },
    OwnedParameter { "gateDepth", "Noise gate" },
    OwnedParameter { "gateAttack", "Noise gate" },
    OwnedParameter { "gateHold", "Noise gate" },
    OwnedParameter { "gateRelease", "Noise gate" },
    // Cabinet section. The loaded impulse responses themselves are not parameters and are not
    // owned -- a match cannot supply one, which is why applying a candidate warns about them.
    OwnedParameter { "cabinet", "Cabinet" },
    OwnedParameter { "cabinetBlend", "Cabinet" },
    OwnedParameter { "cabinetWidth", "Cabinet" },
    OwnedParameter { "cabinetAlignment", "Cabinet" },
    // Only with Isolate chain on; see OwnedParameter::chainOnly.
    OwnedParameter { "pedal1Bypass", "Chain", true },
    OwnedParameter { "pedal2Bypass", "Chain", true },
    OwnedParameter { "pedal3Bypass", "Chain", true },
    OwnedParameter { "pedal4Bypass", "Chain", true },
    OwnedParameter { "delayMix", "Chain", true },
    OwnedParameter { "reverbMix", "Chain", true },
    /* The cabinet controls a match fits and now actually writes.

       **Appended, not filed under Cabinet above.** The masks and the saved `heldValues` are
       positional, so inserting these beside their group would move every chain entry after them.
       `restoreAutoMatchProjectState` refuses a saved hold whose length disagrees rather than
       mapping it, so a stale project is declined rather than mis-guarded either way -- but the
       append-only rule is what keeps that a safety net instead of the mechanism.

       `cabHighCut` is the one that matters: `setCandidateParameters` fits it from the reference's
       measured darkness, and until the cabinet became a stage with its own controls there was no
       parameter to write it into, so the match's one cabinet measurement was discarded on apply.
       It is owned now because it is written now, which is the rule the whole table follows.

       Deliberately **not** owned: the built-in model's cabinet, microphone, position and distance,
       and everything per-slot. A match does not fit those -- it fits a filter, and which speaker
       best resembles that filter is a suggestion the Cabinet page makes rather than a value the
       analyzer computed. Auto Match must never guard a control it did not write. */
    OwnedParameter { "cabLowCut", "Cabinet" },
    OwnedParameter { "cabHighCut", "Cabinet" },
    OwnedParameter { "cabDiBlend", "Cabinet" },
};

inline constexpr std::size_t ownedCount = owned.size();

/* The released, gesture and external-write sets are single 64-bit words so that the guard can
   update them from the audio thread with one atomic operation and no lock. Growing the owned set
   past 64 entries is legal, but it has to become a std::bitset behind a mutex first -- and that
   mutex would then be taken on the audio thread, which is why this stops the build instead. */
static_assert(ownedCount <= 64,
              "the Auto Match masks are 64-bit words; see the note above before growing this list");

/// Index of an id in `owned`, or -1. `constexpr` so the ids can be checked at compile time.
[[nodiscard]] constexpr int indexOf(std::string_view id) noexcept
{
    for (std::size_t index = 0; index < owned.size(); ++index)
        if (owned[index].id == id) return static_cast<int>(index);
    return -1;
}

/// A few spot checks, so a typo in the table above is a build failure rather than a control that
/// silently never gets guarded.
static_assert(indexOf("gain") >= 0);
static_assert(indexOf("presence") >= 0);
static_assert(indexOf("resonance") >= 0);
static_assert(indexOf("gateThreshold") >= 0);
static_assert(indexOf("output") < 0, "output is a level control and is deliberately not owned");

[[nodiscard]] constexpr std::uint64_t bit(std::size_t index) noexcept
{
    return std::uint64_t { 1 } << index;
}

/// Every owned parameter, or only the amplifier ones when the chain was left alone.
[[nodiscard]] constexpr std::uint64_t maskFor(bool isolatedChain) noexcept
{
    std::uint64_t mask {};
    for (std::size_t index = 0; index < owned.size(); ++index)
        if (isolatedChain || ! owned[index].chainOnly) mask |= bit(index);
    return mask;
}
} // namespace tf::automatch
