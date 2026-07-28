#include "nts/ml/PackedTanhModel.h"

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
constexpr std::size_t v1HeaderBytes = 4 + 6 * sizeof(std::uint32_t);
constexpr std::size_t v2HeaderBytes = 4 + 11 * sizeof(std::uint32_t);

std::uint32_t readU32(const std::byte* data) noexcept
{
    std::uint32_t value {}; std::memcpy(&value, data, sizeof(value)); return value;
}
bool finite(std::span<const float> values) noexcept
{
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}
float sigmoid(float value) noexcept
{
    value = std::clamp(value, -30.0f, 30.0f); return 1.0f / (1.0f + std::exp(-value));
}
float clean(float value) noexcept
{
    if (! std::isfinite(value)) return 0.0f;
    return std::abs(value) < 1.0e-30f ? 0.0f : value;
}
}

bool PackedTanhModel::load(std::span<const std::byte> bytes, std::string& error)
{
    loaded = false; modelArchitecture = PackedArchitecture::none;
    if (bytes.size() < v1HeaderBytes || std::memcmp(bytes.data(), "NTSM", 4) != 0)
    {
        error = "Invalid packed model header"; return false;
    }
    const auto version = readU32(bytes.data() + 4);
    const auto architectureCode = readU32(bytes.data() + 8);
    const auto rate = readU32(bytes.data() + 12);
    const auto inputChannels = readU32(bytes.data() + 16);
    const auto stateCount = readU32(bytes.data() + 20);
    const auto outputChannels = readU32(bytes.data() + 24);
    std::size_t headerBytes = v1HeaderBytes, controlCount = 0, layers = 1, kernelSize = 1;
    if (version == 2)
    {
        if (bytes.size() < v2HeaderBytes) { error = "Packed v2 header is truncated"; return false; }
        controlCount = readU32(bytes.data() + 28);
        layers = readU32(bytes.data() + 40);
        kernelSize = readU32(bytes.data() + 44);
        headerBytes = v2HeaderBytes;
    }
    if ((version != 1 && version != 2) || inputChannels != 1 || outputChannels != 1
        || stateCount == 0 || stateCount > 4096 || rate < 8000 || rate > 384000
        || controlCount > 64 || architectureCode < 1 || architectureCode > 4)
    {
        error = "Unsupported packed model dimensions, architecture, or version"; return false;
    }
    const auto h = static_cast<std::size_t>(stateCount);
    std::size_t floatCount {};
    if (architectureCode == 1 && version == 1)
        floatCount = h + h * h + h + h + 1;
    else if (architectureCode == 2 && version == 2)
        floatCount = 4 * h * (1 + controlCount) + 4 * h * h + 4 * h + h + 2;
    else if (architectureCode == 3 && version == 2)
        floatCount = 3 * h * (1 + controlCount) + 3 * h * h + 3 * h + h + 1;
    else if (architectureCode == 4 && version == 2 && layers > 0 && layers <= 32
             && kernelSize >= 2 && kernelSize <= 32)
        floatCount = h + h * controlCount + layers * h * kernelSize + h + 2;
    else
    {
        error = "Packed model version and architecture do not match"; return false;
    }
    if (floatCount > (std::numeric_limits<std::size_t>::max() - headerBytes) / sizeof(float)
        || bytes.size() != headerBytes + floatCount * sizeof(float))
    {
        error = "Packed model payload size is invalid"; return false;
    }
    std::vector<float> values(floatCount);
    std::memcpy(values.data(), bytes.data() + headerBytes, floatCount * sizeof(float));
    if (! finite(values)) { error = "Packed model contains non-finite weights"; return false; }
    auto cursor = values.begin();
    const auto take = [&cursor](std::vector<float>& target, std::size_t count)
    { target.assign(cursor, cursor + static_cast<std::ptrdiff_t>(count)); cursor += static_cast<std::ptrdiff_t>(count); };
    inputWeight.clear(); recurrentWeight.clear(); bias.clear(); outputWeight.clear();
    residualGain = 0.0f; tcnLayers = layers; tcnKernelSize = kernelSize;
    if (architectureCode == 1)
    {
        take(inputWeight, h); take(recurrentWeight, h * h); take(bias, h); take(outputWeight, h); outputBias = *cursor;
    }
    else if (architectureCode == 2)
    {
        take(inputWeight, 4 * h * (1 + controlCount)); take(recurrentWeight, 4 * h * h);
        take(bias, 4 * h); take(outputWeight, h); outputBias = *cursor++; residualGain = *cursor;
    }
    else if (architectureCode == 3)
    {
        take(inputWeight, 3 * h * (1 + controlCount)); take(recurrentWeight, 3 * h * h);
        take(bias, 3 * h); take(outputWeight, h); outputBias = *cursor;
    }
    else
    {
        take(inputWeight, h + h * controlCount); take(recurrentWeight, layers * h * kernelSize);
        bias.clear(); take(outputWeight, h); outputBias = *cursor++; residualGain = *cursor;
    }
    hiddenSize = h; controls.assign(controlCount, 0.0f); state.assign(h, 0.0f); nextState.assign(h, 0.0f);
    cell.assign(h, 0.0f); nextCell.assign(h, 0.0f); gateValues.assign(4 * h, 0.0f);
    tcnHistory.clear(); tcnHistoryOffsets.clear(); tcnHistoryLengths.clear(); tcnPositions.clear();
    if (architectureCode == 4)
    {
        std::size_t offset {};
        for (std::size_t layer = 0; layer < layers; ++layer)
        {
            const auto length = 1 + (kernelSize - 1) * (std::size_t { 1 } << layer);
            tcnHistoryOffsets.push_back(offset); tcnHistoryLengths.push_back(length); tcnPositions.push_back(0);
            offset += h * length;
        }
        tcnHistory.assign(offset, 0.0f);
    }
    modelSampleRate = static_cast<int>(rate); modelArchitecture = static_cast<PackedArchitecture>(architectureCode);
    loaded = true; error.clear(); return true;
}

bool PackedTanhModel::loadFile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (! stream) { error = "Unable to open packed model"; return false; }
    std::vector<char> raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return load(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()), error);
}

void PackedTanhModel::reset() noexcept
{
    std::fill(state.begin(), state.end(), 0.0f); std::fill(nextState.begin(), nextState.end(), 0.0f);
    std::fill(cell.begin(), cell.end(), 0.0f); std::fill(nextCell.begin(), nextCell.end(), 0.0f);
    std::fill(gateValues.begin(), gateValues.end(), 0.0f); std::fill(tcnHistory.begin(), tcnHistory.end(), 0.0f);
    std::fill(tcnPositions.begin(), tcnPositions.end(), 0);
}

bool PackedTanhModel::setControls(std::span<const float> values) noexcept
{
    if (values.size() != controls.size() || ! finite(values)) return false;
    for (std::size_t index = 0; index < values.size(); ++index) controls[index] = std::clamp(values[index], -1.0f, 1.0f);
    return true;
}

float PackedTanhModel::processSample(float input) noexcept
{
    if (! loaded || ! std::isfinite(input)) return 0.0f;
    const auto h = hiddenSize, c = controls.size();
    if (modelArchitecture == PackedArchitecture::tanhRnn)
    {
        for (std::size_t row = 0; row < h; ++row)
        {
            auto sum = inputWeight[row] * input + bias[row];
            const auto* weights = recurrentWeight.data() + row * h;
            float lane0 {}, lane1 {}, lane2 {}, lane3 {};
            std::size_t column {};
            for (; column + 3 < h; column += 4)
            {
                lane0 += weights[column] * state[column];
                lane1 += weights[column + 1] * state[column + 1];
                lane2 += weights[column + 2] * state[column + 2];
                lane3 += weights[column + 3] * state[column + 3];
            }
            sum += (lane0 + lane1) + (lane2 + lane3);
            for (; column < h; ++column) sum += weights[column] * state[column];
            nextState[row] = std::tanh(sum);
        }
        state.swap(nextState);
    }
    else if (modelArchitecture == PackedArchitecture::lstm)
    {
        for (std::size_t row = 0; row < 4 * h; ++row)
        {
            auto sum = inputWeight[row * (1 + c)] * input + bias[row];
            for (std::size_t control = 0; control < c; ++control)
                sum += inputWeight[row * (1 + c) + 1 + control] * controls[control];
            for (std::size_t column = 0; column < h; ++column) sum += recurrentWeight[row * h + column] * state[column];
            gateValues[row] = sum;
        }
        for (std::size_t row = 0; row < h; ++row)
        {
            const auto i = sigmoid(gateValues[row]), f = sigmoid(gateValues[h + row]);
            const auto g = std::tanh(gateValues[2 * h + row]), o = sigmoid(gateValues[3 * h + row]);
            nextCell[row] = f * cell[row] + i * g; nextState[row] = o * std::tanh(nextCell[row]);
        }
        state.swap(nextState); cell.swap(nextCell);
    }
    else if (modelArchitecture == PackedArchitecture::gru)
    {
        for (std::size_t row = 0; row < 3 * h; ++row)
        {
            auto sum = inputWeight[row * (1 + c)] * input + bias[row];
            for (std::size_t control = 0; control < c; ++control)
                sum += inputWeight[row * (1 + c) + 1 + control] * controls[control];
            gateValues[row] = sum;
        }
        for (std::size_t row = 0; row < h; ++row)
        {
            auto rr = 0.0f, rz = 0.0f, rn = 0.0f;
            for (std::size_t column = 0; column < h; ++column)
            {
                rr += recurrentWeight[row * h + column] * state[column];
                rz += recurrentWeight[(h + row) * h + column] * state[column];
                rn += recurrentWeight[(2 * h + row) * h + column] * state[column];
            }
            const auto resetGate = sigmoid(gateValues[row] + rr);
            const auto updateGate = sigmoid(gateValues[h + row] + rz);
            const auto candidate = std::tanh(gateValues[2 * h + row] + resetGate * rn);
            nextState[row] = (1.0f - updateGate) * candidate + updateGate * state[row];
        }
        state.swap(nextState);
    }
    else
    {
        for (std::size_t row = 0; row < h; ++row)
        {
            auto value = inputWeight[row] * input;
            for (std::size_t control = 0; control < c; ++control) value += inputWeight[h + row * c + control] * controls[control];
            nextState[row] = value;
        }
        for (std::size_t layer = 0; layer < tcnLayers; ++layer)
        {
            const auto length = tcnHistoryLengths[layer], position = tcnPositions[layer];
            const auto offset = tcnHistoryOffsets[layer], dilation = std::size_t { 1 } << layer;
            for (std::size_t row = 0; row < h; ++row) tcnHistory[offset + row * length + position] = nextState[row];
            for (std::size_t row = 0; row < h; ++row)
            {
                auto sum = 0.0f;
                for (std::size_t tap = 0; tap < tcnKernelSize; ++tap)
                    sum += recurrentWeight[(layer * h + row) * tcnKernelSize + tap]
                         * tcnHistory[offset + row * length + (position + length - (tap * dilation) % length) % length];
                gateValues[row] = std::tanh(sum);
            }
            for (std::size_t row = 0; row < h; ++row) nextState[row] += gateValues[row];
            tcnPositions[layer] = (position + 1) % length;
        }
        state = nextState;
    }
    auto output = outputBias + residualGain * input;
    for (std::size_t index = 0; index < h; ++index) output += outputWeight[index] * state[index];
    return clean(output);
}

bool PackedTanhModel::process(std::span<const float> input, std::span<float> output) noexcept
{
    if (! loaded || output.size() < input.size()) return false;
    for (std::size_t index = 0; index < input.size(); ++index) output[index] = processSample(input[index]);
    return true;
}

std::size_t PackedTanhModel::memoryBytes() const noexcept
{
    return (inputWeight.capacity() + recurrentWeight.capacity() + bias.capacity() + outputWeight.capacity()
        + controls.capacity() + state.capacity() + nextState.capacity() + cell.capacity() + nextCell.capacity()
        + gateValues.capacity() + tcnHistory.capacity()) * sizeof(float)
        + (tcnHistoryOffsets.capacity() + tcnHistoryLengths.capacity() + tcnPositions.capacity()) * sizeof(std::size_t);
}
} // namespace nts::ml
