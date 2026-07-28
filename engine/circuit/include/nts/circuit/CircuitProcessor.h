#pragma once

#include "CircuitGraph.h"

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace nts::circuit
{
class CircuitProcessor
{
public:
    void prepare(const nts::dsp::ProcessSpec& spec);
    void reset() noexcept;
    [[nodiscard]] ValidationReport stageGraph(const CircuitGraphDescription& graph,
                                              const NeuralModelBytes& neuralModels = {});
    // Call at an audio-block boundary while the circuit engine is inactive. This
    // publishes compiled edits immediately so an unused engine cannot retain a
    // stale pending graph and block later edits.
    void publishPendingWithoutCrossfade() noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::vector<NodeTelemetry> telemetrySnapshot() const;
    [[nodiscard]] std::size_t latencySamples() const noexcept;

private:
    struct Slot { std::array<std::unique_ptr<CompiledCircuitGraph>, 2> channels; };
    nts::dsp::ProcessSpec currentSpec;
    std::array<std::unique_ptr<Slot>, 2> slots;
    std::atomic<int> activeSlot { -1 };
    std::atomic<int> pendingSlot { -1 };
    int fadingFromSlot { -1 };
    std::atomic<bool> crossfadeActive { false };
    std::vector<float> oldOutput;
    std::vector<float> newOutput;
    std::size_t crossfadePosition {};
    static constexpr std::size_t crossfadeLength = 2048;
};
} // namespace nts::circuit
