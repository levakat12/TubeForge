#include "nts/nam/Capture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace nts::nam
{
namespace
{
// Mirrors reader.py. Widening either list means widening the Python one in the same commit, or
// the parity gate fails -- which is the intended way to find out.
constexpr std::array supportedVersions { "0.7.0" };
constexpr std::array supportedActivations { "LeakyReLU", "ReLU", "Tanh" };

// The packer's limits, mirroring packed.py. They exist because the runtime sizes fixed buffers
// from these numbers; see PackedWaveNetModel.
constexpr int maximumLayers = 64;
constexpr int maximumKernel = 64;
constexpr int maximumDilation = 4096;

/// Fails with a named reason. Every refusal in this file goes through here so none of them can
/// be a bare `return false` that leaves the caller with an empty message.
bool refuse(std::string& error, std::string reason)
{
    error = std::move(reason);
    return false;
}

[[nodiscard]] juce::var property(const juce::var& value, const char* name)
{
    return value.isObject() ? value.getProperty(juce::Identifier(name), juce::var()) : juce::var();
}

[[nodiscard]] std::string text(const juce::var& value, const char* name)
{
    const auto found = property(value, name);
    return found.isVoid() || found.isUndefined() ? std::string {}
                                                 : found.toString().toStdString();
}

[[nodiscard]] std::optional<double> number(const juce::var& value)
{
    if (value.isDouble() || value.isInt() || value.isInt64()) return static_cast<double>(value);
    return std::nullopt;
}

[[nodiscard]] int integerOr(const juce::var& value, const char* name, int fallback)
{
    const auto found = property(value, name);
    const auto parsed = number(found);
    return parsed ? static_cast<int>(std::llround(*parsed)) : fallback;
}

/// A sub-object's `active` flag. The conditioning blocks are absent in every corpus capture, so
/// absent means inactive; only an explicitly active one is a refusal.
[[nodiscard]] bool isActive(const juce::var& block)
{
    return block.isObject() && static_cast<bool>(property(block, "active"));
}

bool refuseIfActive(const juce::var& block, const char* name, std::string& error)
{
    if (! isActive(block)) return true;
    return refuse(error, std::string(name)
                             + " is active; conditioned WaveNet features are not implemented");
}

bool readLayerArray(const juce::var& config, juce::var& array, std::vector<LayerSpec>& layers,
                    std::string& error)
{
    const auto arrays = property(config, "layers");
    const auto* list = arrays.getArray();
    if (list == nullptr || list->size() != 1)
        return refuse(error, "Only single-layer-array WaveNet captures are supported");
    array = list->getReference(0);

    const auto* kernels = property(array, "kernel_sizes").getArray();
    const auto* dilations = property(array, "dilations").getArray();
    const auto* activations = property(array, "activation").getArray();
    if (kernels == nullptr || dilations == nullptr || activations == nullptr)
        return refuse(error, "Layer array is missing kernel_sizes, dilations, or activation");
    if (kernels->size() != dilations->size() || kernels->size() != activations->size())
        return refuse(error, "Layer array kernel_sizes, dilations, and activation lengths disagree");
    if (kernels->isEmpty()) return refuse(error, "Layer array is empty");

    const auto channels = integerOr(array, "channels", 0);
    if (channels <= 0) return refuse(error, "Layer array declares no channels");
    if (integerOr(array, "bottleneck", channels) != channels)
        return refuse(error, "Bottleneck channels differing from layer channels are not implemented");
    for (const auto* name : { "groups_input", "groups_input_mixin" })
        if (integerOr(array, name, 1) != 1)
            return refuse(error, "Grouped convolutions (" + std::string(name) + ") are not implemented");

    const auto layerOneByOne = property(array, "layer1x1");
    if (integerOr(layerOneByOne, "groups", 1) != 1)
        return refuse(error, "Grouped layer 1x1 convolutions are not implemented");
    // Absent means present-and-active in the format's defaults, so only an explicit false refuses.
    if (layerOneByOne.isObject())
    {
        const auto active = property(layerOneByOne, "active");
        if (! active.isVoid() && ! static_cast<bool>(active))
            return refuse(error, "Captures without an active layer 1x1 are not implemented");
    }
    if (! refuseIfActive(property(array, "head1x1"), "head1x1", error)) return false;
    for (const auto* name : { "conv_pre_film", "conv_post_film", "input_mixin_pre_film",
                              "input_mixin_post_film", "activation_pre_film", "activation_post_film",
                              "layer1x1_post_film", "head1x1_post_film" })
        if (! refuseIfActive(property(array, name), name, error)) return false;

    const auto slimmable = property(array, "slimmable");
    if (! slimmable.isVoid() && static_cast<bool>(slimmable))
        return refuse(error, "Nested slimmable layers are not implemented");
    if (const auto* gating = property(array, "gating_mode").getArray())
        for (const auto& mode : *gating)
        {
            const auto name = mode.toString().toLowerCase();
            if (name != "none" && name != "false")
                return refuse(error, "Gating mode '" + mode.toString().toStdString()
                                         + "' is not implemented");
        }
    if (const auto* secondary = property(array, "secondary_activation").getArray())
        for (const auto& entry : *secondary)
            if (! entry.isVoid()) return refuse(error, "Secondary activations are not implemented");

    layers.clear();
    layers.reserve(static_cast<std::size_t>(kernels->size()));
    for (int index = 0; index < kernels->size(); ++index)
    {
        const auto activation = activations->getReference(index);
        LayerSpec layer;
        layer.kernelSize = static_cast<int>(kernels->getReference(index));
        layer.dilation = static_cast<int>(dilations->getReference(index));
        if (activation.isObject())
        {
            layer.activation = text(activation, "type");
            layer.negativeSlope = number(property(activation, "negative_slope")).value_or(0.01);
        }
        else
        {
            layer.activation = activation.toString().toStdString();
            layer.negativeSlope = 0.01;
        }
        if (std::find(supportedActivations.begin(), supportedActivations.end(), layer.activation)
            == supportedActivations.end())
            return refuse(error, "Activation '" + layer.activation + "' is not implemented");
        if (layer.kernelSize < 1 || layer.dilation < 1)
            return refuse(error, "Kernel sizes and dilations must be positive");
        layers.push_back(std::move(layer));
    }
    return true;
}

bool readSubmodel(const juce::var& model, int sampleRate, WaveNetSpec& spec, std::string& error)
{
    if (text(model, "architecture") != "WaveNet")
        return refuse(error, "Submodel architecture '" + text(model, "architecture")
                                 + "' is not implemented");
    const auto config = property(model, "config");
    if (! property(config, "head").isVoid())
        return refuse(error, "Layer-array-external heads are not implemented");

    juce::var array;
    if (! readLayerArray(config, array, spec.layers, error)) return false;
    const auto head = property(array, "head");
    if (integerOr(head, "out_channels", 1) != 1)
        return refuse(error, "Multi-channel heads are not implemented");

    spec.channels = integerOr(array, "channels", 0);
    spec.inputSize = integerOr(array, "input_size", 1);
    spec.conditionSize = integerOr(array, "condition_size", 1);
    spec.headKernelSize = integerOr(head, "kernel_size", 1);
    const auto headBias = property(head, "bias");
    spec.headBias = headBias.isVoid() ? true : static_cast<bool>(headBias);
    spec.headScale = number(property(config, "head_scale")).value_or(1.0);
    spec.sampleRate = sampleRate;
    if (spec.inputSize != 1 || spec.conditionSize != 1)
        return refuse(error, "Only single-channel input and condition are implemented");

    const auto* weights = property(model, "weights").getArray();
    if (weights == nullptr) return refuse(error, "Capture declares no weights");
    spec.weights.clear();
    spec.weights.reserve(static_cast<std::size_t>(weights->size()));
    for (const auto& weight : *weights)
        spec.weights.push_back(static_cast<float>(static_cast<double>(weight)));

    if (spec.weights.size() != spec.weightCount())
        return refuse(error, "Capture declares " + std::to_string(spec.weights.size())
                                 + " weights but its geometry implies "
                                 + std::to_string(spec.weightCount()));
    if (! std::all_of(spec.weights.begin(), spec.weights.end(),
                      [](float value) { return std::isfinite(value); }))
        return refuse(error, "Capture contains non-finite weights");
    // The trailing weight duplicates config head_scale in every corpus file. Disagreement means
    // the layout assumption is wrong for this capture, which is exactly when to stop rather than
    // pack something the runtime would read as a different model.
    if (std::abs(static_cast<double>(spec.weights.back()) - spec.headScale)
        > 1.0e-6 * std::max(1.0, std::abs(spec.headScale)))
        return refuse(error, "Trailing weight does not match head_scale; weight layout is not as expected");
    return true;
}

void writeU32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    // Written a byte at a time rather than memcpy'd so the format stays little-endian by
    // construction rather than by whatever the host happens to be.
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
}

void writeFloat(std::vector<std::byte>& bytes, float value)
{
    std::uint32_t bits {};
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(bytes, bits);
}
} // namespace

GearKind gearKindFrom(std::string_view gearType) noexcept
{
    if (gearType == "amp") return GearKind::amp;
    if (gearType == "full-rig") return GearKind::fullRig;
    if (gearType == "pedal") return GearKind::pedal;
    return GearKind::unknown;
}

const char* gearKindName(GearKind kind) noexcept
{
    switch (kind)
    {
        case GearKind::amp: return "Amp";
        case GearKind::fullRig: return "Full rig";
        case GearKind::pedal: return "Pedal";
        case GearKind::unknown: break;
    }
    return "Unknown";
}

const char* tierName(Tier tier) noexcept { return tier == Tier::lite ? "lite" : "standard"; }

std::string CaptureMetadata::licenseText() const
{
    const auto author = modeledBy.empty() ? std::string("an unnamed author") : modeledBy;
    const auto subject = name.empty() ? std::string("an unnamed capture") : name;
    return "Derived from the Neural Amp Modeler capture '" + subject + "' by " + author + ".\n"
           "Converted for local use. Redistribution requires the capture author's permission.\n";
}

std::string CaptureMetadata::displayName(std::string_view fileStem) const
{
    if (! fileStem.empty()) return std::string(fileStem);
    if (! name.empty()) return name;
    if (! gearMake.empty() && ! gearModel.empty() && gearMake != gearModel)
        return gearMake + " " + gearModel;
    if (! gearModel.empty()) return gearModel;
    if (! gearMake.empty()) return gearMake;
    return "Unnamed capture";
}

std::size_t WaveNetSpec::weightCount() const noexcept
{
    const auto width = static_cast<std::size_t>(std::max(0, channels));
    std::size_t perLayer {};
    for (const auto& layer : layers)
        perLayer += width * width * static_cast<std::size_t>(layer.kernelSize) // dilated conv
                    + width                                                    // its bias
                    + width * static_cast<std::size_t>(conditionSize)          // input mixin
                    + width * width + width;                                   // 1x1 and its bias
    const auto head = width * static_cast<std::size_t>(headKernelSize) + (headBias ? 1u : 0u);
    // + the input rechannel, + the trailing head scale.
    return perLayer + width * static_cast<std::size_t>(inputSize) + head + 1;
}

std::size_t WaveNetSpec::receptiveField() const noexcept
{
    std::size_t total {};
    for (const auto& layer : layers)
        total += static_cast<std::size_t>(layer.kernelSize - 1) * static_cast<std::size_t>(layer.dilation);
    return total + static_cast<std::size_t>(headKernelSize);
}

const WaveNetSpec* Capture::spec(Tier tier) const noexcept
{
    const auto& requested = tier == Tier::lite ? lite : standard;
    if (requested) return &*requested;
    const auto& other = tier == Tier::lite ? standard : lite;
    return other ? &*other : nullptr;
}

bool Capture::hasTier(Tier tier) const noexcept
{
    return (tier == Tier::lite ? lite : standard).has_value();
}

bool readCapture(const juce::String& json, Capture& capture, std::string& error)
{
    capture = {};
    juce::var document;
    if (juce::JSON::parse(json, document).failed() || ! document.isObject())
        return refuse(error, "Capture is not valid JSON");

    capture.version = text(document, "version");
    if (std::find(supportedVersions.begin(), supportedVersions.end(), capture.version)
        == supportedVersions.end())
        return refuse(error, "NAM format version '" + capture.version + "' is not supported");
    const auto rate = number(property(document, "sample_rate")).value_or(0.0);
    capture.sampleRate = static_cast<int>(rate);
    if (capture.sampleRate <= 0) return refuse(error, "Capture declares no sample rate");

    const auto metadata = property(document, "metadata");
    capture.metadata.name = text(metadata, "name");
    capture.metadata.modeledBy = text(metadata, "modeled_by");
    capture.metadata.gearMake = text(metadata, "gear_make");
    capture.metadata.gearModel = text(metadata, "gear_model");
    capture.metadata.gearType = text(metadata, "gear_type");
    capture.metadata.toneType = text(metadata, "tone_type");
    capture.metadata.gear = gearKindFrom(capture.metadata.gearType);
    capture.metadata.loudness = number(property(metadata, "loudness"));
    capture.metadata.gain = number(property(metadata, "gain"));
    capture.metadata.validationEsr = number(property(property(metadata, "training"), "validation_esr"));

    const auto architecture = text(document, "architecture");
    if (architecture == "WaveNet")
    {
        WaveNetSpec spec;
        if (! readSubmodel(document, capture.sampleRate, spec, error)) return false;
        capture.standard = std::move(spec);
        return true;
    }
    if (architecture != "SlimmableContainer")
        return refuse(error, "Capture architecture '" + architecture + "' is not implemented");

    const auto* submodels = property(property(document, "config"), "submodels").getArray();
    if (submodels == nullptr || submodels->isEmpty())
        return refuse(error, "Container declares no submodels");

    // Ordered by max_value, smallest first: that is what distinguishes the tiers, and the file
    // does not otherwise label them.
    std::vector<juce::var> ordered(submodels->begin(), submodels->end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const juce::var& left, const juce::var& right)
    {
        return number(property(left, "max_value")).value_or(0.0)
             < number(property(right, "max_value")).value_or(0.0);
    });

    WaveNetSpec liteSpec, standardSpec;
    if (! readSubmodel(property(ordered.front(), "model"), capture.sampleRate, liteSpec, error))
        return false;
    if (! readSubmodel(property(ordered.back(), "model"), capture.sampleRate, standardSpec, error))
        return false;
    capture.lite = std::move(liteSpec);
    capture.standard = std::move(standardSpec);
    return true;
}

bool packWaveNet(const WaveNetSpec& spec, std::vector<std::byte>& bytes, std::string& error)
{
    bytes.clear();
    if (spec.layers.empty() || static_cast<int>(spec.layers.size()) > maximumLayers)
        return refuse(error, "WaveNet captures are limited to " + std::to_string(maximumLayers) + " layers");
    for (const auto& layer : spec.layers)
    {
        if (layer.kernelSize < 2 || layer.kernelSize > maximumKernel)
            return refuse(error, "Kernel size " + std::to_string(layer.kernelSize) + " is out of range");
        if (layer.dilation < 1 || layer.dilation > maximumDilation)
            return refuse(error, "Dilation " + std::to_string(layer.dilation) + " is out of range");
        // The runtime implements one activation. A capture using a curve the reader accepts but
        // the runtime does not is refused here rather than silently played as the wrong one.
        if (layer.activation != "LeakyReLU" || std::abs(layer.negativeSlope - 0.01) > 1.0e-9)
            return refuse(error, "The packed runtime implements LeakyReLU(0.01) layers only");
    }
    if (spec.inputSize != 1 || spec.conditionSize != 1)
        return refuse(error, "Packed WaveNet supports single-channel input and condition only");
    if (spec.weights.size() != spec.weightCount())
        return refuse(error, "Weight vector does not match the declared geometry");

    // Header, layer table, payload -- the layout packed.py documents. Fields, in order: magic,
    // version, architecture, sampleRate, inputChannels, channels, outputChannels, controlCount,
    // layerCount, flags, headKernel, reserved.
    bytes.reserve(48 + spec.layers.size() * 8 + spec.weights.size() * sizeof(float));
    for (const auto character : { 'N', 'T', 'S', 'M' })
        bytes.push_back(static_cast<std::byte>(character));
    writeU32(bytes, 3);
    writeU32(bytes, 5);
    writeU32(bytes, static_cast<std::uint32_t>(spec.sampleRate));
    writeU32(bytes, 1);
    writeU32(bytes, static_cast<std::uint32_t>(spec.channels));
    writeU32(bytes, 1);
    writeU32(bytes, 0);
    writeU32(bytes, static_cast<std::uint32_t>(spec.layers.size()));
    writeU32(bytes, spec.headBias ? 1u : 0u);
    writeU32(bytes, static_cast<std::uint32_t>(spec.headKernelSize));
    writeU32(bytes, 0);
    for (const auto& layer : spec.layers)
    {
        writeU32(bytes, static_cast<std::uint32_t>(layer.kernelSize));
        writeU32(bytes, static_cast<std::uint32_t>(layer.dilation));
    }
    for (const auto weight : spec.weights) writeFloat(bytes, weight);
    error.clear();
    return true;
}
} // namespace nts::nam
