#include "nts/ml/PackedFeatureRegressor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace nts::ml
{
namespace
{
/// magic, version, architecture, inputCount, hiddenCount, outputCount.
constexpr std::size_t v4HeaderBytes = 4 + 5 * sizeof(std::uint32_t);
constexpr std::uint32_t regressorArchitecture = 6;
/* Bounds rather than trust. The header is four numbers that decide how many megabytes get
   allocated and how far the evaluation loops read, so a corrupt or hostile file is a resize and a
   walk off the end of a buffer unless they are checked before either happens. The values mirror
   the limits `pack_regressor` enforces on the way out. */
constexpr std::size_t maximumInputs = 512;
constexpr std::size_t maximumHidden = 256;
constexpr std::size_t maximumOutputs = 64;

std::uint32_t readU32(const std::byte* data) noexcept
{
    std::uint32_t value {}; std::memcpy(&value, data, sizeof(value)); return value;
}
bool finite(std::span<const float> values) noexcept
{
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}
}

bool PackedFeatureRegressor::load(std::span<const std::byte> bytes, std::string& error)
{
    loaded = false;
    if (bytes.size() < v4HeaderBytes || std::memcmp(bytes.data(), "NTSM", 4) != 0)
    {
        error = "Invalid packed model header";
        return false;
    }
    const auto version = readU32(bytes.data() + 4);
    const auto architecture = readU32(bytes.data() + 8);
    if (version != 4 || architecture != regressorArchitecture)
    {
        error = "Not a packed feature regressor";
        return false;
    }
    const std::size_t inputCount = readU32(bytes.data() + 12);
    const std::size_t hiddenCount = readU32(bytes.data() + 16);
    const std::size_t outputCount = readU32(bytes.data() + 20);
    if (inputCount == 0 || inputCount > maximumInputs || hiddenCount == 0
        || hiddenCount > maximumHidden || outputCount == 0 || outputCount > maximumOutputs)
    {
        error = "Packed regressor dimensions are out of range";
        return false;
    }

    const auto floatCount = inputCount * 2                 // mean and scale
                          + inputCount * hiddenCount + hiddenCount
                          + hiddenCount * outputCount + outputCount;
    if (bytes.size() != v4HeaderBytes + floatCount * sizeof(float))
    {
        error = "Packed regressor payload size does not match its header";
        return false;
    }

    std::vector<float> values(floatCount);
    std::memcpy(values.data(), bytes.data() + v4HeaderBytes, floatCount * sizeof(float));
    if (! finite(values))
    {
        error = "Packed regressor contains non-finite weights";
        return false;
    }

    auto cursor = values.begin();
    const auto take = [&cursor](std::size_t count)
    {
        std::vector<float> block(cursor, cursor + static_cast<std::ptrdiff_t>(count));
        cursor += static_cast<std::ptrdiff_t>(count);
        return block;
    };
    inputMean = take(inputCount);
    inputScale = take(inputCount);
    hiddenWeight = take(inputCount * hiddenCount);
    hiddenBias = take(hiddenCount);
    outputWeight = take(hiddenCount * outputCount);
    outputBias = take(outputCount);

    /* A zero scale would divide a standardised feature to infinity. The exporter already replaces
       the spread of a constant feature with one, so a zero here means a corrupt file rather than a
       degenerate column -- but the evaluation must not be the thing that finds out. */
    if (std::any_of(inputScale.begin(), inputScale.end(),
                    [](float value) { return std::abs(value) < 1.0e-20f; }))
    {
        error = "Packed regressor has a zero input scale";
        return false;
    }

    inputs = inputCount;
    hidden = hiddenCount;
    outputs = outputCount;
    loaded = true;
    return true;
}

bool PackedFeatureRegressor::loadFile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (! file)
    {
        error = "Could not open " + path.string();
        return false;
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    return load({ reinterpret_cast<const std::byte*>(raw.data()), raw.size() }, error);
}

bool PackedFeatureRegressor::predict(std::span<const float> features, std::span<float> results) const
{
    if (! loaded || features.size() != inputs || results.size() != outputs) return false;

    // Small enough to sit on the stack of the worker that calls this -- 256 floats at the format's
    // ceiling. Heap-allocating per call would be the only allocation in an otherwise plain
    // evaluation, and this is called once per match.
    float activations[maximumHidden] {};
    for (std::size_t unit = 0; unit < hidden; ++unit)
    {
        auto sum = hiddenBias[unit];
        for (std::size_t input = 0; input < inputs; ++input)
        {
            // Row-major over inputs, matching numpy's `(inputs, hidden)` C-order layout.
            const auto standardised = (features[input] - inputMean[input]) / inputScale[input];
            sum += standardised * hiddenWeight[input * hidden + unit];
        }
        activations[unit] = std::tanh(sum);
    }
    for (std::size_t output = 0; output < outputs; ++output)
    {
        auto sum = outputBias[output];
        for (std::size_t unit = 0; unit < hidden; ++unit)
            sum += activations[unit] * outputWeight[unit * outputs + output];
        // Clamped before the exponential rather than after, so a pathological input cannot
        // produce an infinity that then reads as a perfectly confident 1.
        sum = std::clamp(sum, -30.0f, 30.0f);
        results[output] = 1.0f / (1.0f + std::exp(-sum));
    }
    return true;
}
} // namespace nts::ml
