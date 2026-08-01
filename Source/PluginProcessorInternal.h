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
#include <nts/reconstruction/StemRefinement.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

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
constexpr auto delayMix = "delayMix";
constexpr auto delayTime = "delayTime";
constexpr auto delayFeedback = "delayFeedback";
constexpr auto delayTone = "delayTone";
constexpr auto reverbMix = "reverbMix";
constexpr auto reverbSize = "reverbSize";
constexpr auto reverbDamping = "reverbDamping";
constexpr auto cabinetBlend = "cabinetBlend";
constexpr auto tunerMute = "tunerMute";
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
    ParameterIds::reverbDamping, ParameterIds::cabinetBlend
};

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

juce::File assistantPreferencesFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("TubeForge").getChildFile("assistant-preferences.json");
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
    for (int sample = 0; sample < samples; ++sample)
    {
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto value = buffer.getSample(channel, sample);
            result.peak = std::max(result.peak, std::abs(value)); energy += value * value;
            if (std::abs(value) >= 0.999f) ++result.clipped;
        }
        const auto left = buffer.getSample(0, sample);
        if (sample > 0 && std::signbit(left) != std::signbit(buffer.getSample(0, sample - 1))) ++crossings;
        if (channels > 1)
        {
            const auto right = buffer.getSample(1, sample);
            cross += left * right; leftEnergy += left * left; rightEnergy += right * right;
        }
    }
    result.rms = std::sqrt(static_cast<float>(energy / static_cast<double>(samples * channels)));
    result.zeroCrossingRate = static_cast<float>(crossings) / static_cast<float>(samples);
    if (channels > 1) result.correlation = static_cast<float>(cross / std::sqrt(std::max(1.0e-18, leftEnergy * rightEnergy)));
    return result;
}

} // namespace
