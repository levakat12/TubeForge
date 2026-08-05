#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace nts::test
{
using LayerGeometry = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

inline void appendPackedU32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    const auto offset = bytes.size(); bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline void appendPackedFloat(std::vector<std::byte>& bytes, float value)
{
    const auto offset = bytes.size(); bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/** The layer geometry every capture in the NAM corpus uses: 23 layers, kernels of 6 with two of 15,
    dilations cycling 1,3,7,17,41,101,239 three times with a 1,13 pair between the second and third.
    Benchmarks that use anything smaller measure a model nobody runs.
*/
inline LayerGeometry namCorpusLayers()
{
    const std::uint32_t cycle[] { 1u, 3u, 7u, 17u, 41u, 101u, 239u };
    LayerGeometry layers;
    for (const auto dilation : cycle) layers.emplace_back(6u, dilation);
    for (const auto dilation : cycle) layers.emplace_back(6u, dilation);
    layers.emplace_back(15u, 1u); layers.emplace_back(15u, 13u);
    for (const auto dilation : cycle) layers.emplace_back(6u, dilation);
    return layers;
}

/** An NTSM v2 conditioned LSTM.

    The architecture Phase 5 recommends, and the one whose cost is dominated by gate activations:
    three sigmoids and two tanh per hidden unit per sample, against the single tanh of the v1
    architecture-1 model. Anything measuring or testing activation behaviour needs this rather
    than the simpler fixture, or it is exercising the case with the least of them.
*/
inline std::vector<std::byte> lstmFixture(std::size_t hidden, std::uint32_t sampleRate = 48000)
{
    std::vector<std::byte> bytes { std::byte{'N'}, std::byte{'T'}, std::byte{'S'}, std::byte{'M'} };
    // version, architecture, rate, inputChannels, stateCount, outputChannels, controlCount,
    // two unused slots, layers, kernelSize -- eleven words, matching the v2 header length.
    for (const auto value : { 2u, 2u, sampleRate, 1u, static_cast<std::uint32_t>(hidden), 1u,
                              0u, 0u, 0u, 1u, 1u }) appendPackedU32(bytes, value);
    for (std::size_t row = 0; row < 4 * hidden; ++row) appendPackedFloat(bytes, 0.02f);
    for (std::size_t row = 0; row < 4 * hidden; ++row)
        for (std::size_t column = 0; column < hidden; ++column)
            appendPackedFloat(bytes, row % hidden == column ? 0.12f : 0.0f);
    for (std::size_t row = 0; row < 4 * hidden; ++row) appendPackedFloat(bytes, 0.0f);
    for (std::size_t row = 0; row < hidden; ++row) appendPackedFloat(bytes, 0.01f);
    appendPackedFloat(bytes, 0.0f); appendPackedFloat(bytes, 0.0f); return bytes;
}

/** An NTSM v3 WaveNet with the given geometry and deterministic weights.

    The weights are a fixed sinusoid rather than random values so that a failure is reproducible and
    the same fixture can be compared across builds.
*/
inline std::vector<std::byte> wavenetFixture(std::uint32_t channels = 2,
                                             std::uint32_t headKernel = 2,
                                             const LayerGeometry& layers = { { 2u, 1u }, { 2u, 2u } },
                                             std::uint32_t sampleRate = 48000)
{
    std::vector<std::byte> bytes { std::byte { 'N' }, std::byte { 'T' }, std::byte { 'S' }, std::byte { 'M' } };
    for (const auto value : { 3u, 5u, sampleRate, 1u, channels, 1u, 0u,
                              static_cast<std::uint32_t>(layers.size()), 1u, headKernel, 0u })
        appendPackedU32(bytes, value);
    for (const auto& layer : layers) { appendPackedU32(bytes, layer.first); appendPackedU32(bytes, layer.second); }
    std::size_t floats = channels;
    for (const auto& layer : layers)
        floats += static_cast<std::size_t>(channels) * channels * layer.first
                  + channels + channels + static_cast<std::size_t>(channels) * channels + channels;
    floats += static_cast<std::size_t>(channels) * headKernel + 2;
    for (std::size_t index = 0; index < floats; ++index)
        appendPackedFloat(bytes, 0.35f * std::sin(static_cast<float>(index) * 0.7f) + 0.05f);
    return bytes;
}
} // namespace nts::test
