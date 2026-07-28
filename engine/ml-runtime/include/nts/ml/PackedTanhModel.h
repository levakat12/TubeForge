#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nts::ml
{
enum class PackedArchitecture { none = 0, tanhRnn = 1, lstm = 2, gru = 3, causalTcn = 4 };

class PackedTanhModel
{
public:
    bool load(std::span<const std::byte> bytes, std::string& error);
    bool loadFile(const std::filesystem::path& path, std::string& error);
    void reset() noexcept;
    float processSample(float input) noexcept;
    bool process(std::span<const float> input, std::span<float> output) noexcept;
    bool setControls(std::span<const float> values) noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return loaded; }
    [[nodiscard]] std::size_t stateSize() const noexcept { return state.size(); }
    [[nodiscard]] int sampleRate() const noexcept { return modelSampleRate; }
    [[nodiscard]] std::size_t controlCount() const noexcept { return controls.size(); }
    [[nodiscard]] PackedArchitecture architecture() const noexcept { return modelArchitecture; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

private:
    int modelSampleRate {};
    PackedArchitecture modelArchitecture { PackedArchitecture::none };
    std::size_t hiddenSize {};
    std::size_t tcnLayers {};
    std::size_t tcnKernelSize {};
    std::vector<float> inputWeight;
    std::vector<float> recurrentWeight;
    std::vector<float> bias;
    std::vector<float> outputWeight;
    float outputBias {};
    float residualGain {};
    std::vector<float> controls;
    std::vector<float> state;
    std::vector<float> nextState;
    std::vector<float> cell;
    std::vector<float> nextCell;
    std::vector<float> gateValues;
    std::vector<float> tcnHistory;
    std::vector<std::size_t> tcnHistoryOffsets;
    std::vector<std::size_t> tcnHistoryLengths;
    std::vector<std::size_t> tcnPositions;
    bool loaded {};
};
} // namespace nts::ml
