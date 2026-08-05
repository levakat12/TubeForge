#include "nts/ml/PackedTanhModel.h"

#include "nts/dsp/Nonlinear.h"
#include "nts/dsp/Simd.h"

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
    // Resolved once per sample rather than per gate, and it already accounts for the
    // architectures where the approximation is not worth taking.
    const auto useApproximation = approximatesActivations();
    if (modelArchitecture == PackedArchitecture::tanhRnn)
    {
        for (std::size_t row = 0; row < h; ++row)
        {
            // Was hand-unrolled into four accumulators here and nowhere else; dsp::dotProduct
            // is the same idea with SSE2 behind it, and now every architecture below gets it.
            const auto sum = inputWeight[row] * input + bias[row]
                           + dsp::dotProduct(recurrentWeight.data() + row * h, state.data(), h);
            nextState[row] = useApproximation ? dsp::fastTanh(sum) : std::tanh(sum);
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
            sum += dsp::dotProduct(recurrentWeight.data() + row * h, state.data(), h);
            gateValues[row] = sum;
        }
        for (std::size_t row = 0; row < h; ++row)
        {
            // Hoisted out of the loop body as function pointers would cost a call per gate, so
            // the branch is on the flag and the compiler keeps both bodies straight-line.
            const auto i = useApproximation ? dsp::fastSigmoid(gateValues[row]) : sigmoid(gateValues[row]);
            const auto f = useApproximation ? dsp::fastSigmoid(gateValues[h + row]) : sigmoid(gateValues[h + row]);
            const auto g = useApproximation ? dsp::fastTanh(gateValues[2 * h + row]) : std::tanh(gateValues[2 * h + row]);
            const auto o = useApproximation ? dsp::fastSigmoid(gateValues[3 * h + row]) : sigmoid(gateValues[3 * h + row]);
            nextCell[row] = f * cell[row] + i * g;
            nextState[row] = o * (useApproximation ? dsp::fastTanh(nextCell[row]) : std::tanh(nextCell[row]));
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
            // Three separate passes rather than one fused loop over the three weight rows. The
            // fused form was chosen for state locality, but state is h floats and stays in L1
            // across all three regardless, so the vector width is worth more than the reuse.
            const auto rr = dsp::dotProduct(recurrentWeight.data() + row * h, state.data(), h);
            const auto rz = dsp::dotProduct(recurrentWeight.data() + (h + row) * h, state.data(), h);
            const auto rn = dsp::dotProduct(recurrentWeight.data() + (2 * h + row) * h, state.data(), h);
            const auto resetInput = gateValues[row] + rr;
            const auto updateInput = gateValues[h + row] + rz;
            const auto resetGate = useApproximation ? dsp::fastSigmoid(resetInput) : sigmoid(resetInput);
            const auto updateGate = useApproximation ? dsp::fastSigmoid(updateInput) : sigmoid(updateInput);
            const auto candidateInput = gateValues[2 * h + row] + resetGate * rn;
            const auto candidate = useApproximation ? dsp::fastTanh(candidateInput) : std::tanh(candidateInput);
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
                gateValues[row] = useApproximation ? dsp::fastTanh(sum) : std::tanh(sum);
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
