#pragma once

#include "Analysis.h"
#include "Common.h"

#include <array>
#include <atomic>
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace nts::dsp
{
struct ImpulseResponse
{
    double sampleRate { 48000.0 };
    std::vector<std::vector<float>> channels;
};

struct ImpulsePreparationOptions
{
    std::size_t outputChannels { 2 };
    float trimThresholdDb { -80.0f };
    bool removeDc { true };
    bool normalizePeak { true };
    float normalizationDb { -1.0f };
};

[[nodiscard]] ImpulseResponse prepareImpulseResponse(const ImpulseResponse& decoded,
                                                      double targetSampleRate,
                                                      const ImpulsePreparationOptions& options);

/** Time-domain FIR convolution, invariant to how the input is cut into blocks.

    The delay line is stored **doubled**: every incoming sample is written twice, at `position`
    and at `position + maximumLength`. That costs one extra buffer and buys the thing that
    matters -- the taps a given output needs always occupy one contiguous, forward-running span,
    so the inner loop is a plain dot product with no wraparound test and no reverse walk. A ring
    read with a branch per tap is neither vectorisable nor prefetchable, and this is the most
    expensive loop in the plug-in.

    The impulse is stored **time-reversed** for the same reason: it makes the span and the
    coefficients run in the same direction, so the loop reduces to dsp::dotProduct.

    Accumulation is float rather than double. Over a 4096-tap response the difference against a
    double accumulator measures below -140 dBFS, which is far under the noise floor of anything
    this convolves, and halves the work per SIMD lane.
*/
class DirectConvolver
{
public:
    void prepare(std::size_t maximumImpulseLength, std::size_t channels);
    void reset() noexcept;
    bool loadImpulse(std::span<const float> left, std::span<const float> right = {});
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::size_t impulseLength() const noexcept { return activeLength; }

private:
    std::size_t maximumLength {};
    std::size_t activeLength {};
    std::size_t configuredChannels {};
    std::size_t writePosition {};
    /// Time-reversed, one `maximumLength` region per channel.
    std::vector<float> impulse;
    /// Doubled, one `2 * maximumLength` region per channel.
    std::vector<float> history;
};

/** Uniform-partitioned FFT convolution. Zero latency, but **fixed block size only**.

    Every process call must be handed exactly the block size passed to prepare. The algorithm
    advances its spectrum history and shifts its overlap by one whole block per call, so a
    shorter call desynchronises both: measured against direct convolution over the same
    signal, full blocks agree to 5e-7 while mixed short blocks diverge by 0.41.

    That is not a fixable oversight -- a uniform partition scheme needs a fixed hop, and
    buffering to provide one would cost the block of latency this class exists to avoid. So
    the constraint is enforced rather than papered over: a short block is refused outright,
    which fails loudly instead of quietly producing wrong audio.

    For a path that cannot promise fixed-size blocks -- anything fed directly by a host -- use
    CrossfadingDirectConvolver, which is time-domain and invariant to how the input is cut up.
*/
class PartitionedConvolver
{
public:
    void prepare(std::size_t blockSize, std::size_t maximumImpulseLength, std::size_t channels);
    /// The block size this instance requires, so a caller can check before committing to it.
    [[nodiscard]] std::size_t requiredBlockSize() const noexcept { return blockSize; }
    void reset() noexcept;
    bool loadImpulse(std::span<const float> left, std::span<const float> right = {});
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] std::size_t latencySamples() const noexcept { return 0; }
    [[nodiscard]] std::size_t partitions() const noexcept { return activePartitions; }

private:
    Fft fft;
    std::size_t blockSize {};
    std::size_t fftSize {};
    std::size_t maximumPartitions {};
    std::size_t activePartitions {};
    std::size_t configuredChannels {};
    std::size_t spectrumPosition {};
    std::vector<std::complex<float>> filters;
    std::vector<std::complex<float>> inputSpectra;
    std::vector<std::complex<float>> work;
    std::vector<std::complex<float>> accumulator;
    std::vector<float> overlap;
};

/** Two direct convolvers with an atomically requested crossfade between them.

    Mirrors CrossfadingConvolver's contract -- stage into the inactive side from any thread,
    then ask the audio thread to fade across -- but stays in the time domain.

    That is deliberate, not an oversight. PartitionedConvolver, and so CrossfadingConvolver
    with it, is only correct when every process call is handed exactly the block size it was
    prepared with: it advances its spectrum history and shifts its overlap by a whole block
    regardless of how many samples it actually received. Hosts hand over short blocks
    routinely -- the last block of an offline render, a buffer-size change mid-session -- and
    the error is not subtle. Measured against direct convolution over the same signal, full
    blocks agree to 5e-7 while mixed short blocks diverge by 0.41.

    So anything on a path that cannot promise fixed-size blocks belongs here instead. The
    cost is the direct convolver's O(taps) per sample, doubled for the length of a fade.
*/
class CrossfadingDirectConvolver
{
public:
    void prepare(std::size_t maximumImpulseLength, std::size_t channels, std::size_t maximumBlockSize);
    void reset() noexcept;
    /** Loads the side that is not currently sounding. Safe to call from a worker thread;
        returns false if a swap is already staged or running, in which case the caller should
        retry rather than overwrite a response the audio thread is about to fade into.
    */
    bool loadInactiveImpulse(std::span<const float> left, std::span<const float> right = {});
    void requestSwap(std::size_t crossfadeSamples) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] bool isCrossfading() const noexcept
    {
        return crossfadeActive.load(std::memory_order_acquire);
    }
    /** True while a response has been staged but the audio thread has not yet picked it up.

        Distinct from isCrossfading: between requestSwap and the next process call the fade has
        not started, so the crossfade flag is still clear even though a swap is owed. A caller
        deciding whether it may skip this convolver has to see both, or it can skip the block
        that was supposed to begin the fade and strand the staged response.
    */
    [[nodiscard]] bool isSwapPending() const noexcept
    {
        return swapRequested.load(std::memory_order_acquire);
    }
    /** Length of the response currently sounding, for tail reporting. */
    [[nodiscard]] std::size_t impulseLength() const noexcept
    {
        return convolvers[activeIndex.load(std::memory_order_acquire)].impulseLength();
    }

private:
    DirectConvolver convolvers[2];
    std::atomic<int> activeIndex {};
    std::atomic<bool> swapRequested {};
    std::atomic<bool> crossfadeActive {};
    std::size_t fadeLength { 1 };
    std::size_t crossfadeRemaining {};
    std::size_t configuredChannels {};
    std::size_t maximumBlockSize {};
    std::vector<float> oldBuffer;
    std::vector<float> newBuffer;
};

class CrossfadingConvolver
{
public:
    void prepare(std::size_t blockSize, std::size_t maximumImpulseLength, std::size_t channels);
    bool loadInactiveImpulse(std::span<const float> left, std::span<const float> right = {});
    void requestSwap(std::size_t crossfadeSamples) noexcept;
    void reset() noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] bool isCrossfading() const noexcept
    {
        return crossfadeActive.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool isSwapPending() const noexcept
    {
        return swapRequested.load(std::memory_order_acquire);
    }

private:
    PartitionedConvolver convolvers[2];
    std::atomic<int> activeIndex {};
    std::atomic<bool> swapRequested {};
    std::atomic<bool> crossfadeActive {};
    std::size_t fadeLength { 1 };
    std::size_t crossfadeRemaining {};
    std::size_t configuredChannels {};
    std::size_t maximumBlockSize {};
    std::vector<float> oldBuffer;
    std::vector<float> newBuffer;
};

/** Partitioned FFT convolution behind a fixed-size buffer, usable from a variable-block host.

    PartitionedConvolver is O(log N) per sample where the direct convolver is O(N), which at a
    few thousand taps is the difference between a cabinet impulse costing a few percent of the
    callback and costing a quarter of it. What has kept it unusable is its contract: it advances
    its spectrum history by exactly one block per call, so a host that varies its block size --
    which is every host, at least at the end of an offline render -- desynchronises it.

    This wraps it in the buffering that contract needs. Input accumulates until a whole partition
    is available, the partition is convolved, and output is drained from a matching buffer. The
    price is exactly one partition of latency, which is why this is a separate class rather than
    a fix to the other one: for a short impulse the direct convolver is both faster and free of
    latency, and there is no arrangement in which one of these two is always right.

    The caller must add `latencySamples` to whatever it reports upstream.
*/
class BufferedCrossfadingConvolver
{
public:
    void prepare(std::size_t partitionSize, std::size_t maximumImpulseLength, std::size_t channels);
    void reset() noexcept;
    bool loadInactiveImpulse(std::span<const float> left, std::span<const float> right = {});
    void requestSwap(std::size_t crossfadeSamples) noexcept;
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    [[nodiscard]] bool isCrossfading() const noexcept { return convolver.isCrossfading(); }
    [[nodiscard]] bool isSwapPending() const noexcept { return convolver.isSwapPending(); }
    [[nodiscard]] std::size_t impulseLength() const noexcept { return activeLength; }
    /// One whole partition, always. The buffering is what the wrapped algorithm requires.
    [[nodiscard]] std::size_t latencySamples() const noexcept { return partition; }

private:
    CrossfadingConvolver convolver;
    std::size_t partition {};
    std::size_t configuredChannels {};
    std::size_t activeLength {};
    /// Shared across channels: they are filled and drained in lockstep, so one counter is right.
    std::size_t fill {};
    std::vector<float> inputBuffer;
    std::vector<float> outputBuffer;
    std::array<float*, maximumChannels> partitionPointers {};
};

} // namespace nts::dsp
