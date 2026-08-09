#pragma once

// Shared by the PluginProcessor translation units.
//
// The processor's implementation is split across several files -- the audio path, host and
// project state, the asset loaders, and everything else -- and all of them need the same
// parameter ids, tables and small helpers. Those live here rather than being repeated,
// because a duplicated table is the thing that later disagrees with itself.
//
// Not a public header: nothing outside Source/PluginProcessor*.cpp should include it.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <TubeForgeAssets.h>

#include <nts/diagnostics/ProcessMemory.h>
#include <nts/ir/CabinetMatch.h>
#include <nts/reconstruction/StemRefinement.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>

namespace ParameterIds
{
constexpr auto input = "input";
constexpr auto output = "output";
constexpr auto bypass = "bypass";
constexpr auto gain = "gain";
constexpr auto bass = "bass";
constexpr auto mid = "mid";
constexpr auto treble = "treble";
constexpr auto presence = "presence";
constexpr auto resonance = "resonance";
constexpr auto master = "master";
constexpr auto cabinet = "cabinet";
constexpr auto instrument = "instrument";
constexpr auto topology = "topology";
constexpr auto stage1 = "stage1";
constexpr auto stage2 = "stage2";
constexpr auto stage3 = "stage3";
constexpr auto stage4 = "stage4";
constexpr auto bias = "bias";
constexpr auto lowCut = "lowCut";
constexpr auto highCut = "highCut";
constexpr auto oversampling = "oversampling";
constexpr auto sag = "sag";
constexpr auto feedback = "feedback";
constexpr auto crossover = "crossover";
constexpr auto cleanBlend = "cleanBlend";
/// A switch on the current voicing's front panel -- Ultra Lo, Deep and the rest. Which ones exist
/// depends on the voicing; see nts::amp::panelSwitchAppliesTo.
constexpr auto panelSwitch = "panelSwitch";
/// Untouched input summed over the whole amplifier. Distinct from `cleanBlend`, which blends only
/// the low band -- see AmpParameters::dryBlend for why the difference matters.
constexpr auto dryBlend = "dryBlend";
/// The bi-amp controls. Named for the band rather than for a front panel, because the two bands
/// are what the engine has -- see BassPathParameters.
constexpr auto lowBandDrive = "lowBandDrive";
constexpr auto lowBandLevel = "lowBandLevel";
constexpr auto highBandLevel = "highBandLevel";
constexpr auto cabinetAlignment = "cabinetAlignment";
constexpr auto tightness = "tightness";
constexpr auto pickEmphasis = "pickEmphasis";
constexpr auto engineMode = "engineMode";
constexpr auto neuralMonitor = "neuralMonitor";
constexpr auto neuralCompensation = "neuralCompensation";
constexpr auto circuitPreampTube = "circuitPreampTube";
constexpr auto circuitPowerTube = "circuitPowerTube";
constexpr auto circuitPowerTopology = "circuitPowerTopology";
constexpr auto circuitToneStack = "circuitToneStack";
constexpr auto circuitBackend = "circuitBackend";
constexpr auto circuitCabinetStyle = "circuitCabinetStyle";
constexpr auto gateEnabled = "gateEnabled";
constexpr auto gateThreshold = "gateThreshold";
constexpr auto gateDepth = "gateDepth";
constexpr auto gateAttack = "gateAttack";
constexpr auto gateHold = "gateHold";
constexpr auto gateRelease = "gateRelease";
constexpr auto loudnessMatch = "loudnessMatch";
constexpr auto cabinetWidth = "cabinetWidth";
constexpr auto tunerReference = "tunerReference";
constexpr auto delayMix = "delayMix";
constexpr auto delayTime = "delayTime";
constexpr auto delayFeedback = "delayFeedback";
constexpr auto delayTone = "delayTone";
constexpr auto reverbMix = "reverbMix";
constexpr auto reverbSize = "reverbSize";
constexpr auto reverbDamping = "reverbDamping";
constexpr auto cabinetBlend = "cabinetBlend";
constexpr auto tunerMute = "tunerMute";
constexpr auto performanceTier = "performanceTier";
/// Whether the song analyzer holds the rig. Not a tone control: it decides who *writes* the
/// tone controls. See Source/AutoMatch.h.
constexpr auto autoMatch = "autoMatch";
/// Whether a held rig also follows the live signal. Only ever moves the input trim and the gate
/// threshold, and only within 6 dB of what the match set. Off by default.
constexpr auto autoMatchTracking = "autoMatchTracking";
/* The cabinet stage's own controls.

   Named `cab*` rather than `cabinet*` to keep them visibly separate from the four that predate
   them -- `cabinet`, `cabinetBlend`, `cabinetWidth` and `cabinetAlignment` live in
   `ampControlIds` and cannot move, whereas everything here is saved through `cabinetControlIds`
   and `nts::state::CabinetState`. That split is not cosmetic: `ampControlIds` holds 58 of the 64
   entries `ProjectState::validate` accepts, and overrunning it makes every saved project fail to
   load. See the note at that list. */
constexpr auto cabLevelA = "cabLevelA";
constexpr auto cabLevelB = "cabLevelB";
constexpr auto cabPanA = "cabPanA";
constexpr auto cabPanB = "cabPanB";
constexpr auto cabPhaseA = "cabPhaseA";
constexpr auto cabPhaseB = "cabPhaseB";
constexpr auto cabDelayA = "cabDelayA";
constexpr auto cabMuteA = "cabMuteA";
constexpr auto cabMuteB = "cabMuteB";
constexpr auto cabLowCut = "cabLowCut";
constexpr auto cabHighCut = "cabHighCut";
constexpr auto cabDiBlend = "cabDiBlend";
constexpr auto cabOutputTrim = "cabOutputTrim";
/// The built-in cabinet model, per slot: which box, which microphone, and where it is pointed
/// from how far away. Ignored while that slot has a user impulse response loaded.
constexpr auto cabModelA = "cabModelA";
constexpr auto cabModelB = "cabModelB";
constexpr auto cabMicA = "cabMicA";
constexpr auto cabMicB = "cabMicB";
constexpr auto cabPositionA = "cabPositionA";
constexpr auto cabPositionB = "cabPositionB";
constexpr auto cabDistanceA = "cabDistanceA";
constexpr auto cabDistanceB = "cabDistanceB";
/// How a *loaded* response is prepared, per slot. Ignored while a slot is on the built-in model,
/// which is rendered to the right length and level by construction.
constexpr auto cabIrLengthA = "cabIrLengthA";
constexpr auto cabIrLengthB = "cabIrLengthB";
constexpr auto cabIrNormA = "cabIrNormA";
constexpr auto cabIrNormB = "cabIrNormB";
constexpr auto cabIrMinPhaseA = "cabIrMinPhaseA";
constexpr auto cabIrMinPhaseB = "cabIrMinPhaseB";
// Six per slot, in the order PedalParameters declares them, so the slot index can walk them.
constexpr auto pedal1Kind = "pedal1Kind";
constexpr auto pedal1Bypass = "pedal1Bypass";
constexpr auto pedal1Drive = "pedal1Drive";
constexpr auto pedal1Tone = "pedal1Tone";
constexpr auto pedal1Level = "pedal1Level";
constexpr auto pedal1Mix = "pedal1Mix";
constexpr auto pedal1AuxA = "pedal1AuxA";
constexpr auto pedal1AuxB = "pedal1AuxB";
constexpr auto pedal2Kind = "pedal2Kind";
constexpr auto pedal2Bypass = "pedal2Bypass";
constexpr auto pedal2Drive = "pedal2Drive";
constexpr auto pedal2Tone = "pedal2Tone";
constexpr auto pedal2Level = "pedal2Level";
constexpr auto pedal2Mix = "pedal2Mix";
constexpr auto pedal2AuxA = "pedal2AuxA";
constexpr auto pedal2AuxB = "pedal2AuxB";
constexpr auto pedal3Kind = "pedal3Kind";
constexpr auto pedal3Bypass = "pedal3Bypass";
constexpr auto pedal3Drive = "pedal3Drive";
constexpr auto pedal3Tone = "pedal3Tone";
constexpr auto pedal3Level = "pedal3Level";
constexpr auto pedal3Mix = "pedal3Mix";
constexpr auto pedal3AuxA = "pedal3AuxA";
constexpr auto pedal3AuxB = "pedal3AuxB";
constexpr auto pedal4Kind = "pedal4Kind";
constexpr auto pedal4Bypass = "pedal4Bypass";
constexpr auto pedal4Drive = "pedal4Drive";
constexpr auto pedal4Tone = "pedal4Tone";
constexpr auto pedal4Level = "pedal4Level";
constexpr auto pedal4Mix = "pedal4Mix";
constexpr auto pedal4AuxA = "pedal4AuxA";
constexpr auto pedal4AuxB = "pedal4AuxB";
} // namespace ParameterIds

namespace
{
// Expands from the same list as the Param enumerators, so index i is ParameterIds::<the
// i'th name> by construction rather than by convention.
constexpr std::array parameterIdList {
#define TUBEFORGE_DECLARE_PARAM_ID(name) ParameterIds::name,
    TUBEFORGE_RUNTIME_PARAMETERS(TUBEFORGE_DECLARE_PARAM_ID)
#undef TUBEFORGE_DECLARE_PARAM_ID
};
static_assert(parameterIdList.size() == static_cast<std::size_t>(TubeForgeAudioProcessor::Param::count));

constexpr std::array ampControlIds {
    ParameterIds::gain, ParameterIds::bass, ParameterIds::mid, ParameterIds::treble,
    ParameterIds::presence, ParameterIds::resonance, ParameterIds::master, ParameterIds::cabinet,
    ParameterIds::instrument, ParameterIds::topology, ParameterIds::stage1, ParameterIds::stage2,
    ParameterIds::stage3, ParameterIds::stage4, ParameterIds::bias, ParameterIds::lowCut,
    ParameterIds::highCut, ParameterIds::oversampling, ParameterIds::sag, ParameterIds::feedback,
    ParameterIds::crossover, ParameterIds::cleanBlend, ParameterIds::cabinetAlignment,
    ParameterIds::tightness, ParameterIds::pickEmphasis, ParameterIds::engineMode,
    ParameterIds::neuralMonitor, ParameterIds::neuralCompensation,
    ParameterIds::circuitPreampTube, ParameterIds::circuitPowerTube,
    ParameterIds::circuitPowerTopology, ParameterIds::circuitToneStack,
    ParameterIds::circuitBackend, ParameterIds::circuitCabinetStyle,
    // Appended so existing saved projects, which hold fewer entries, still map
    // positionally onto the ids ahead of these.
    ParameterIds::gateEnabled, ParameterIds::gateThreshold, ParameterIds::gateDepth,
    ParameterIds::gateAttack, ParameterIds::gateHold, ParameterIds::gateRelease,
    // Appended for the same reason as the gate block above: saved projects hold fewer
    // entries and must keep mapping onto the ids ahead of these.
    ParameterIds::delayMix, ParameterIds::delayTime, ParameterIds::delayFeedback,
    ParameterIds::delayTone, ParameterIds::reverbMix, ParameterIds::reverbSize,
    ParameterIds::reverbDamping, ParameterIds::cabinetBlend,
    // Appended for the same reason as the blocks above.
    ParameterIds::loudnessMatch, ParameterIds::cabinetWidth, ParameterIds::tunerReference,
    /* The bi-amp and dry-blend controls, appended -- and they were briefly not.

       Written next to `crossover` and `cleanBlend` first, because that is where they belong by
       meaning, and that is the one thing this list must never be ordered by. `applyProjectState`
       walks a saved project's values positionally against this table, so inserting four ids in
       the middle shifted every id after them by four: an old project's Cab alignment landed in
       Dry blend, its Tightness in Low drive, and so on to the end of the list.

       Caught by launching the standalone and reading the panel -- Low drive sat at 5.4 and Low
       level at -1.0 where the defaults are 2.6 and 0.0. Nothing failed, nothing warned, and every
       test still passed, because a positional remap produces perfectly valid numbers in the wrong
       fields. The grouping that reads well here is the grouping that corrupts saved state; the
       append-only rule the blocks above state is the whole contract. */
    ParameterIds::dryBlend, ParameterIds::lowBandDrive,
    ParameterIds::lowBandLevel, ParameterIds::highBandLevel,
    // Appended, for the reason stated at length above.
    ParameterIds::panelSwitch,
    /* Auto Match, appended for the same reason again.

       Saved here rather than in a block of its own so that a project remembers whether the
       analyzer was holding the rig. What it cannot remember this way is *which* controls had
       been handed back -- that needs the released set and the matched values, which is a
       schema-5 addition and is not done. Until it is, reopening a project with Auto Match on
       comes back `armed` rather than `holding`: the switch is on, nothing is guarded, and
       re-applying a candidate puts it back. Coming back armed is the safe direction to be
       wrong in -- the alternative is a rig quietly re-writing knobs the user had taken back. */
    ParameterIds::autoMatch,
    // Appended, for the reason stated at length above.
    ParameterIds::autoMatchTracking
};
/* One entry of `EngineState::ampControls` is written per id above, and `nts::state::validate`
   rejects a longer array than this -- so overrunning it does not truncate a save, it makes **every
   saved project fail to load** as "Amp control state is invalid". A valid project the user can no
   longer open, caused by adding a knob.

   The bi-amp band controls took this to 54. Whoever next appends here finds out at compile time
   rather than from a bug report, which is the whole point of stating it where the list is. */
static_assert(ampControlIds.size() <= nts::state::maximumAmpControls,
              "ampControlIds has outgrown what ProjectState::validate will accept");

/** The cabinet stage's controls, saved through `nts::state::CabinetState` at schema 6.

    **A separate list, and that is the whole point.** `ampControlIds` above holds 58 of the 64
    entries `ProjectState::validate` accepts, and the cabinet alone wants thirteen. Appending
    them there would leave five slots for everything this plug-in ever grows, and the failure
    mode when that runs out is not a truncated save -- it is every saved project refusing to
    open. A block per subsystem is what stops the next feature having that argument.

    The same append-only rule applies *within* this list, for exactly the reason spelled out at
    length above `ampControlIds`: `applyProjectState` walks a saved project's values positionally
    against it, so inserting an id in the middle silently moves every value after it into the
    wrong control. Append, always.

    The four cabinet controls that predate this block -- `cabinet`, `cabinetBlend`,
    `cabinetWidth`, `cabinetAlignment` -- are deliberately **not** here. They are already saved
    through `ampControlIds` at fixed positions, and writing them twice would mean two sources of
    truth for one value.
*/
constexpr std::array cabinetControlIds {
    ParameterIds::cabLevelA, ParameterIds::cabLevelB, ParameterIds::cabPanA,
    ParameterIds::cabPanB, ParameterIds::cabPhaseA, ParameterIds::cabPhaseB,
    ParameterIds::cabDelayA, ParameterIds::cabMuteA, ParameterIds::cabMuteB,
    ParameterIds::cabLowCut, ParameterIds::cabHighCut, ParameterIds::cabDiBlend,
    ParameterIds::cabOutputTrim,
    // Appended, for the reason stated above. The model controls arrived after the first block.
    ParameterIds::cabModelA, ParameterIds::cabModelB, ParameterIds::cabMicA,
    ParameterIds::cabMicB, ParameterIds::cabPositionA, ParameterIds::cabPositionB,
    ParameterIds::cabDistanceA, ParameterIds::cabDistanceB,
    // Appended, for the reason stated above. The loaded-response controls arrived after the model.
    ParameterIds::cabIrLengthA, ParameterIds::cabIrLengthB, ParameterIds::cabIrNormA,
    ParameterIds::cabIrNormB, ParameterIds::cabIrMinPhaseA, ParameterIds::cabIrMinPhaseB
};
static_assert(cabinetControlIds.size() <= nts::state::maximumCabinetControls,
              "cabinetControlIds has outgrown what ProjectState::validate will accept");

/// Row per slot, column per PedalControl. The single place the two are paired.
constexpr std::array<std::array<const char*, TubeForgeAudioProcessor::pedalParameterStride>,
                     nts::pedals::slotCount> pedalParameterIds { {
    { ParameterIds::pedal1Kind, ParameterIds::pedal1Bypass, ParameterIds::pedal1Drive,
      ParameterIds::pedal1Tone, ParameterIds::pedal1Level, ParameterIds::pedal1Mix,
      ParameterIds::pedal1AuxA, ParameterIds::pedal1AuxB },
    { ParameterIds::pedal2Kind, ParameterIds::pedal2Bypass, ParameterIds::pedal2Drive,
      ParameterIds::pedal2Tone, ParameterIds::pedal2Level, ParameterIds::pedal2Mix,
      ParameterIds::pedal2AuxA, ParameterIds::pedal2AuxB },
    { ParameterIds::pedal3Kind, ParameterIds::pedal3Bypass, ParameterIds::pedal3Drive,
      ParameterIds::pedal3Tone, ParameterIds::pedal3Level, ParameterIds::pedal3Mix,
      ParameterIds::pedal3AuxA, ParameterIds::pedal3AuxB },
    { ParameterIds::pedal4Kind, ParameterIds::pedal4Bypass, ParameterIds::pedal4Drive,
      ParameterIds::pedal4Tone, ParameterIds::pedal4Level, ParameterIds::pedal4Mix,
      ParameterIds::pedal4AuxA, ParameterIds::pedal4AuxB } } };

void setCircuitParameter(nts::circuit::NodeSpec& node, std::string_view id, float value)
{
    const auto found = std::find_if(node.parameters.begin(), node.parameters.end(), [id](const auto& parameter)
    { return parameter.id == id; });
    if (found != node.parameters.end()) found->value = value;
    else node.parameters.push_back({ std::string(id), value });
}

std::uint64_t circuitHash(const nts::circuit::CircuitGraphDescription& graph)
{
    const auto json = nts::circuit::serializeCircuit(graph);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : json) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ULL; }
    return hash;
}

std::filesystem::path logPath()
{
    return std::filesystem::path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                     .getChildFile("TubeForge")
                                     .getChildFile("logs")
                                     .getChildFile("tubeforge.jsonl")
                                     .getFullPathName()
                                     .toStdString());
}

/** Where the "don't warn me again" answer lives.

    A file of its own rather than a field in the assistant's preferences, which is a different
    thing: that file is what the assistant has learned about how somebody plays, and this is one
    interface answer about one dialog. Mixing them would mean clearing the assistant's
    personalization also silently turned a warning back on.
*/
juce::File autoMatchPreferencesFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("auto-match-preferences.json");
}

juce::File assistantPreferencesFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("assistant-preferences.json");
}

/// Where converted `.nam` captures live. Beside the profile library, for the same reason: it is
/// user data that outlives any one project and must not sit inside the plug-in's install.
juce::File captureLibraryPath()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("captures");
}

/// Where the impulse-response browser keeps its root folder, favourites and recents. Beside the
/// other TubeForge preferences rather than in the project: a shelf of responses is a statement
/// about this machine, not about one rig.
juce::File cabinetLibraryPreferencesPath()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("cabinet-library.json");
}

std::filesystem::path tonePackageLibraryPath()
{
    return std::filesystem::path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("profiles").getFullPathName().toStdString());
}

struct BufferMeasurement
{
    float peak {}, rms {}, zeroCrossingRate {}, correlation { 1.0f };
    std::uint32_t clipped {};
};

BufferMeasurement measureBuffer(const juce::AudioBuffer<float>& buffer) noexcept
{
    BufferMeasurement result;
    const auto channels = std::min(2, buffer.getNumChannels());
    const auto samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0) return result;
    double energy = 0.0, cross = 0.0, leftEnergy = 0.0, rightEnergy = 0.0;
    std::uint32_t crossings {};
    // Hoisted out of the loop. getSample is a bounds-checked accessor with two levels of
    // indirection, and this ran up to five times per sample, twice per block.
    const auto* const leftChannel = buffer.getReadPointer(0);
    const auto* const rightChannel = channels > 1 ? buffer.getReadPointer(1) : nullptr;
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto left = leftChannel[sample];
        result.peak = std::max(result.peak, std::abs(left)); energy += left * left;
        if (std::abs(left) >= 0.999f) ++result.clipped;
        if (sample > 0 && std::signbit(left) != std::signbit(leftChannel[sample - 1])) ++crossings;
        if (rightChannel != nullptr)
        {
            const auto right = rightChannel[sample];
            result.peak = std::max(result.peak, std::abs(right)); energy += right * right;
            if (std::abs(right) >= 0.999f) ++result.clipped;
            cross += left * right; leftEnergy += left * left; rightEnergy += right * right;
        }
    }
    result.rms = std::sqrt(static_cast<float>(energy / static_cast<double>(samples * channels)));
    result.zeroCrossingRate = static_cast<float>(crossings) / static_cast<float>(samples);
    if (channels > 1) result.correlation = static_cast<float>(cross / std::sqrt(std::max(1.0e-18, leftEnergy * rightEnergy)));
    return result;
}

} // namespace
