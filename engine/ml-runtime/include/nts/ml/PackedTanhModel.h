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

    /** Swaps the gate activations for rational approximations. Off by default.

        Worth roughly a third of a recurrent model's cost, because the gates are where the time
        goes. It changes the output -- by under 1e-4 per activation, which an LSTM's contractive
        recurrence does not amplify, but it is not bit-exact -- so it is opt-in rather than
        assumed. The exact path stays the reference the packed test vectors and the Python
        parity suites are checked against; nothing validates a model through the fast path.

        See dsp::fastTanh for the approximation and its error bound.
    */
    void setApproximateActivations(bool enabled) noexcept { approximateActivations = enabled; }
    /** Whether the request is actually being honoured for this model's architecture.

        Declined for `tanhRnn`, which evaluates a single tanh per hidden unit per sample: measured
        at 64 units it runs *slower* approximated than exact, because the per-sample test on the
        flag costs about what the one substitution saves. The architectures with three sigmoids
        and two tanh per unit are the ones with something to gain.
    */
    [[nodiscard]] bool approximatesActivations() const noexcept
    { return approximateActivations && modelArchitecture != PackedArchitecture::tanhRnn; }

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
    bool approximateActivations {};
};
} // namespace nts::ml
