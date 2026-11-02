#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nts::ml
{
/** A dense regressor over already-measured features, loaded from an NTSM v4 artifact.

    **Separate from `PackedTanhModel` on purpose, because it is a different kind of model.** That
    class is a per-sample audio processor: it has state, a reset, a sample rate that means
    something, and it is pinned to one input and one output channel because every architecture it
    carries is a filter. This one takes a vector of numbers the analysis already produced and
    returns a vector of numbers, once, off the audio thread. Widening the other class to admit it
    would have meant `inputChannels` meaning audio channels in one file and a feature count in the
    next, with a loader guessing which from the architecture code.

    What it *does* share is the container: the same `NTSM` magic, the same artifact directory, the
    same sha256 in the manifest and the same exported test vectors. Those are what make a model
    load only if the C++ evaluation matches the Python one that trained it, and they are the whole
    reason this went into the packed format rather than into a bespoke file of its own.

    The standardisation is part of the weights rather than a separate step the caller applies. A
    caller that loaded the weights and forgot to standardise would not crash -- it would produce
    confident nonsense from a network evaluated far outside the range it was fitted on -- so the
    opportunity to forget is removed instead of documented.

    Not real-time safe: `load` allocates. `predict` does not, and is `const`, but it is intended
    for the reconstruction worker rather than the audio callback and makes no claim beyond that.
*/
class PackedFeatureRegressor
{
public:
    bool load(std::span<const std::byte> bytes, std::string& error);
    bool loadFile(const std::filesystem::path& path, std::string& error);

    /** Evaluates one feature vector.

        `features` must be exactly `inputCount()` long and `outputs` exactly `outputCount()`;
        anything else returns false rather than reading what it can. A feature vector of the wrong
        length is a version mismatch between the analysis and the model, and quietly evaluating
        the overlap would produce a plausible answer from a misaligned input -- the failure this
        whole format exists to prevent.
    */
    [[nodiscard]] bool predict(std::span<const float> features, std::span<float> outputs) const;

    [[nodiscard]] bool isLoaded() const noexcept { return loaded; }
    [[nodiscard]] std::size_t inputCount() const noexcept { return inputs; }
    [[nodiscard]] std::size_t hiddenCount() const noexcept { return hidden; }
    [[nodiscard]] std::size_t outputCount() const noexcept { return outputs; }

private:
    bool loaded {};
    std::size_t inputs {}, hidden {}, outputs {};
    std::vector<float> inputMean, inputScale;
    std::vector<float> hiddenWeight, hiddenBias;
    std::vector<float> outputWeight, outputBias;
};
} // namespace nts::ml
