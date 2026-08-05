#pragma once

#include "PackedTanhModel.h"
#include "PackedWaveNetModel.h"

#include <cstddef>
#include <span>
#include <string>

namespace nts::ml
{
/** The one model type the audio path talks to, dispatching on what the packed header declares.

    Deliberately not a virtual interface. The alternative -- `unique_ptr<Base>` per channel per slot
    -- would make `Slot` non-copyable, put an allocation between staging and activation, and add an
    indirect call per block for a choice that is fixed at load time. Holding both implementations by
    value costs a handful of empty vectors, keeps the slot a plain value the worker can hand over
    with a move, and leaves the audio thread allocation-free.
*/
class NeuralModel
{
public:
    bool load(std::span<const std::byte> bytes, std::string& error);
    void reset() noexcept;
    bool process(std::span<const float> input, std::span<float> output) noexcept;
    bool setControls(std::span<const float> values) noexcept;

    [[nodiscard]] bool isLoaded() const noexcept;
    [[nodiscard]] int sampleRate() const noexcept;
    [[nodiscard]] std::size_t memoryBytes() const noexcept;
    [[nodiscard]] bool isWaveNet() const noexcept { return wavenetSelected; }

    /** Opts the recurrent architectures into approximate gate activations. Off by default.

        A no-op for WaveNet captures, whose only nonlinearity is a leaky ReLU that is already two
        instructions. See PackedTanhModel::setApproximateActivations for the trade.
    */
    void setApproximateActivations(bool enabled) noexcept
    { recurrent.setApproximateActivations(enabled); }
    [[nodiscard]] bool approximatesActivations() const noexcept
    { return recurrent.approximatesActivations(); }
    /** Samples of history a freshly reset model needs before its output is meaningful. */
    [[nodiscard]] std::size_t warmUpSamples() const noexcept;

private:
    PackedTanhModel recurrent;
    PackedWaveNetModel wavenet;
    bool wavenetSelected {};
};
} // namespace nts::ml
