#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nts::ml
{
/** Real-time inference for NAM-derived WaveNet captures packed as NTSM v3.

    The geometry is not fixed by the header the way the recurrent architectures' is: kernel size and
    dilation vary per layer, so the header is followed by a layer table. Everything the audio thread
    touches is sized during `load`.

    `reset` restores a *primed* state rather than a zeroed one. A WaveNet's response to silence is
    not zero -- every layer has a convolution bias, so silence propagates a non-zero constant up the
    stack -- and a model started from zeroed buffers disagrees with its own exported test vectors
    for a full receptive field, which for the corpus captures is 6347 samples. Priming costs a
    receptive field of inference, far too much for the audio thread, so `load` primes once and
    snapshots the result; `reset` is then a copy.
*/
class PackedWaveNetModel
{
public:
    static constexpr std::size_t maximumLayers = 64;
    static constexpr std::size_t maximumKernel = 64;
    static constexpr std::size_t maximumDilation = 4096;
    static constexpr std::size_t maximumChannels = 64;

    bool load(std::span<const std::byte> bytes, std::string& error);
    bool loadFile(const std::filesystem::path& path, std::string& error);
    void reset() noexcept;
    float processSample(float input) noexcept;
    bool process(std::span<const float> input, std::span<float> output) noexcept;
    bool setControls(std::span<const float> values) noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return loaded; }
    [[nodiscard]] int sampleRate() const noexcept { return modelSampleRate; }
    [[nodiscard]] std::size_t channels() const noexcept { return channelCount; }
    [[nodiscard]] std::size_t layerCount() const noexcept { return layers.size(); }
    [[nodiscard]] std::size_t receptiveField() const noexcept { return receptiveFieldSamples; }
    [[nodiscard]] std::size_t stateSize() const noexcept { return history.size() + headHistory.size(); }
    [[nodiscard]] std::size_t controlCount() const noexcept { return 0; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

private:
    struct Layer
    {
        std::size_t kernelSize {};
        std::size_t dilation {};
        std::size_t weightOffset {};    // into `weights`, at the dilated convolution kernel
        std::size_t biasOffset {};
        std::size_t mixinOffset {};
        std::size_t oneOffset {};
        std::size_t oneBiasOffset {};
        std::size_t historyOffset {};   // into `history`
        std::size_t historyColumns {};
        std::size_t position {};
    };

    void primeFromSilence() noexcept;

    int modelSampleRate {};
    std::size_t channelCount {};
    std::size_t headKernelSize {};
    std::size_t receptiveFieldSamples {};
    bool headHasBias {};
    float headBias {};
    float headScale {};
    std::size_t rechannelOffset {};
    std::size_t headOffset {};
    std::vector<float> weights;
    std::vector<Layer> layers;
    std::vector<float> history;
    std::vector<float> headHistory;
    std::vector<float> primedHistory;
    std::vector<float> primedHeadHistory;
    std::vector<std::size_t> primedPositions;
    std::size_t headPosition {};
    std::size_t primedHeadPosition {};
    std::vector<float> trunk;
    std::vector<float> headAccumulator;
    std::vector<float> activation;
    bool loaded {};
};
} // namespace nts::ml
