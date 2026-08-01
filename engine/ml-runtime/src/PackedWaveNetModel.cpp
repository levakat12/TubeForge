#include "nts/ml/PackedWaveNetModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace nts::ml
{
namespace
{
constexpr std::size_t v3HeaderBytes = 4 + 11 * sizeof(std::uint32_t);
constexpr float leakySlope = 0.01f;

std::uint32_t readU32(const std::byte* data) noexcept
{
    std::uint32_t value {}; std::memcpy(&value, data, sizeof(value)); return value;
}
bool finite(std::span<const float> values) noexcept
{
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}
float leakyRelu(float value) noexcept { return value >= 0.0f ? value : leakySlope * value; }
}

bool PackedWaveNetModel::load(std::span<const std::byte> bytes, std::string& error)
{
    loaded = false;
    if (bytes.size() < v3HeaderBytes || std::memcmp(bytes.data(), "NTSM", 4) != 0)
    { error = "Invalid packed model header"; return false; }
    if (readU32(bytes.data() + 4) != 3 || readU32(bytes.data() + 8) != 5)
    { error = "Packed model is not a version 3 WaveNet"; return false; }
    const auto rate = readU32(bytes.data() + 12);
    const auto inputChannels = readU32(bytes.data() + 16);
    const auto channels = static_cast<std::size_t>(readU32(bytes.data() + 20));
    const auto outputChannels = readU32(bytes.data() + 24);
    const auto controls = readU32(bytes.data() + 28);
    const auto layerCount = static_cast<std::size_t>(readU32(bytes.data() + 32));
    const auto flags = readU32(bytes.data() + 36);
    const auto headKernel = static_cast<std::size_t>(readU32(bytes.data() + 40));
    if (inputChannels != 1 || outputChannels != 1 || controls != 0 || rate < 8000 || rate > 384000
        || channels == 0 || channels > maximumChannels || layerCount == 0
        || layerCount > maximumLayers || headKernel < 1 || headKernel > maximumKernel || flags > 1)
    { error = "Unsupported packed WaveNet dimensions"; return false; }
    if (bytes.size() < v3HeaderBytes + layerCount * 2 * sizeof(std::uint32_t))
    { error = "Packed WaveNet layer table is truncated"; return false; }

    std::vector<Layer> parsed(layerCount);
    std::size_t weightCount {}, historyFloats {}, receptive {};
    for (std::size_t index = 0; index < layerCount; ++index)
    {
        const auto* entry = bytes.data() + v3HeaderBytes + index * 2 * sizeof(std::uint32_t);
        auto& layer = parsed[index];
        layer.kernelSize = static_cast<std::size_t>(readU32(entry));
        layer.dilation = static_cast<std::size_t>(readU32(entry + sizeof(std::uint32_t)));
        if (layer.kernelSize < 2 || layer.kernelSize > maximumKernel
            || layer.dilation < 1 || layer.dilation > maximumDilation)
        { error = "Packed WaveNet layer geometry is out of range"; return false; }
        layer.weightOffset = weightCount;              weightCount += channels * channels * layer.kernelSize;
        layer.biasOffset = weightCount;                weightCount += channels;
        layer.mixinOffset = weightCount;               weightCount += channels;
        layer.oneOffset = weightCount;                 weightCount += channels * channels;
        layer.oneBiasOffset = weightCount;             weightCount += channels;
        layer.historyColumns = (layer.kernelSize - 1) * layer.dilation + 1;
        layer.historyOffset = historyFloats;           historyFloats += channels * layer.historyColumns;
        layer.position = 0;
        receptive += (layer.kernelSize - 1) * layer.dilation;
    }
    // The rechannel precedes the layers in the payload; the head and its scalars follow them.
    const auto rechannel = weightCount;
    for (auto& layer : parsed)
    {
        layer.weightOffset += channels; layer.biasOffset += channels; layer.mixinOffset += channels;
        layer.oneOffset += channels; layer.oneBiasOffset += channels;
    }
    const auto head = channels + rechannel;
    const auto totalFloats = head + channels * headKernel + (flags == 1 ? 1u : 0u) + 1u;
    const auto tableBytes = v3HeaderBytes + layerCount * 2 * sizeof(std::uint32_t);
    if (totalFloats > (std::numeric_limits<std::size_t>::max() - tableBytes) / sizeof(float)
        || bytes.size() != tableBytes + totalFloats * sizeof(float))
    { error = "Packed WaveNet payload size is invalid"; return false; }

    std::vector<float> values(totalFloats);
    std::memcpy(values.data(), bytes.data() + tableBytes, totalFloats * sizeof(float));
    if (! finite(values)) { error = "Packed WaveNet contains non-finite weights"; return false; }

    weights = std::move(values);
    layers = std::move(parsed);
    channelCount = channels;
    headKernelSize = headKernel;
    headHasBias = flags == 1;
    rechannelOffset = 0;
    headOffset = head;
    headBias = headHasBias ? weights[head + channels * headKernel] : 0.0f;
    headScale = weights.back();
    receptiveFieldSamples = receptive + headKernel;
    modelSampleRate = static_cast<int>(rate);
    history.assign(historyFloats, 0.0f);
    headHistory.assign(channels * headKernel, 0.0f);
    trunk.assign(channels, 0.0f);
    headAccumulator.assign(channels, 0.0f);
    activation.assign(channels, 0.0f);
    headPosition = 0;
    loaded = true;
    primeFromSilence();
    error.clear();
    return true;
}

void PackedWaveNetModel::primeFromSilence() noexcept
{
    std::fill(history.begin(), history.end(), 0.0f);
    std::fill(headHistory.begin(), headHistory.end(), 0.0f);
    for (auto& layer : layers) layer.position = 0;
    headPosition = 0;
    for (std::size_t sample = 0; sample < receptiveFieldSamples; ++sample) (void) processSample(0.0f);
    primedHistory = history;
    primedHeadHistory = headHistory;
    primedPositions.resize(layers.size());
    for (std::size_t index = 0; index < layers.size(); ++index) primedPositions[index] = layers[index].position;
    primedHeadPosition = headPosition;
}

bool PackedWaveNetModel::loadFile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (! stream) { error = "Unable to open packed model"; return false; }
    std::vector<char> raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return load(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()), error);
}

void PackedWaveNetModel::reset() noexcept
{
    if (! loaded || primedHistory.size() != history.size()) return;
    std::copy(primedHistory.begin(), primedHistory.end(), history.begin());
    std::copy(primedHeadHistory.begin(), primedHeadHistory.end(), headHistory.begin());
    for (std::size_t index = 0; index < layers.size(); ++index) layers[index].position = primedPositions[index];
    headPosition = primedHeadPosition;
}

float PackedWaveNetModel::processSample(float input) noexcept
{
    if (! loaded) return 0.0f;
    const auto channels = channelCount;
    for (std::size_t row = 0; row < channels; ++row)
    {
        trunk[row] = weights[rechannelOffset + row] * input;
        headAccumulator[row] = 0.0f;
    }
    for (auto& layer : layers)
    {
        float* const ring = history.data() + layer.historyOffset;
        const auto columns = layer.historyColumns;
        std::copy(trunk.begin(), trunk.end(), ring + layer.position * channels);
        for (std::size_t row = 0; row < channels; ++row)
            activation[row] = weights[layer.biasOffset + row] + weights[layer.mixinOffset + row] * input;
        for (std::size_t tap = 0; tap < layer.kernelSize; ++tap)
        {
            const auto lag = (layer.kernelSize - 1 - tap) * layer.dilation;
            const auto column = (layer.position + columns - lag % columns) % columns;
            const float* const source = ring + column * channels;
            const float* const kernel = weights.data() + layer.weightOffset + tap;
            for (std::size_t row = 0; row < channels; ++row)
            {
                float sum = 0.0f;
                const float* const kernelRow = kernel + row * channels * layer.kernelSize;
                for (std::size_t column2 = 0; column2 < channels; ++column2)
                    sum += kernelRow[column2 * layer.kernelSize] * source[column2];
                activation[row] += sum;
            }
        }
        for (std::size_t row = 0; row < channels; ++row)
        {
            activation[row] = leakyRelu(activation[row]);
            headAccumulator[row] += activation[row];
        }
        for (std::size_t row = 0; row < channels; ++row)
        {
            float sum = weights[layer.oneBiasOffset + row];
            const float* const kernelRow = weights.data() + layer.oneOffset + row * channels;
            for (std::size_t column2 = 0; column2 < channels; ++column2)
                sum += kernelRow[column2] * activation[column2];
            trunk[row] += sum;
        }
        layer.position = layer.position + 1 == columns ? 0 : layer.position + 1;
    }
    std::copy(headAccumulator.begin(), headAccumulator.end(), headHistory.data() + headPosition * channels);
    float output = headBias;
    for (std::size_t tap = 0; tap < headKernelSize; ++tap)
    {
        const auto lag = headKernelSize - 1 - tap;
        const auto column = (headPosition + headKernelSize - lag % headKernelSize) % headKernelSize;
        const float* const source = headHistory.data() + column * channels;
        const float* const kernel = weights.data() + headOffset + tap;
        for (std::size_t row = 0; row < channels; ++row)
            output += kernel[row * headKernelSize] * source[row];
    }
    headPosition = headPosition + 1 == headKernelSize ? 0 : headPosition + 1;
    return output * headScale;
}

bool PackedWaveNetModel::process(std::span<const float> input, std::span<float> output) noexcept
{
    if (! loaded || input.size() != output.size()) return false;
    for (std::size_t index = 0; index < input.size(); ++index)
        output[index] = processSample(input[index]);
    return true;
}

bool PackedWaveNetModel::setControls(std::span<const float> values) noexcept
{
    return values.empty();
}

std::size_t PackedWaveNetModel::memoryBytes() const noexcept
{
    return (weights.size() + history.size() + headHistory.size() + primedHistory.size()
            + primedHeadHistory.size() + trunk.size() + headAccumulator.size() + activation.size())
           * sizeof(float) + layers.size() * sizeof(Layer);
}
} // namespace nts::ml
