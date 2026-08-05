#pragma once

#include <cstddef>

namespace nts::dsp
{
[[nodiscard]] bool simdAvailable() noexcept;
void multiplyGainScalar(float* samples, std::size_t count, float gain) noexcept;
void multiplyGainSimd(float* samples, std::size_t count, float gain) noexcept;

/** Sum of products of two contiguous float spans.

    The shape every FIR and matrix-vector loop in the engine reduces to once its operands are
    laid out contiguously, which is what the ring-buffer rewrites in DirectConvolver and the
    Oversampler exist to arrange.

    Four independent accumulators, because a single one serialises on multiply-add latency and
    leaves most of the pipeline idle: the loop is throughput-bound, not dependency-bound, only
    when several partial sums are in flight at once. The partials are reduced in a fixed order,
    so the result is deterministic for a given count -- but it is *not* the same order as a
    scalar left-to-right accumulation, and differs from it in the last bits. Callers that were
    accumulating in double will see a slightly different result; callers needing the old
    behaviour should keep using dotProductScalar.
*/
[[nodiscard]] float dotProductScalar(const float* left, const float* right, std::size_t count) noexcept;
/// The SSE2 kernel, which is the floor: always present, always safe to call.
[[nodiscard]] float dotProductSse2(const float* left, const float* right, std::size_t count) noexcept;
/** The AVX2 kernel. **Only safe to call once the runtime check has passed** -- calling it on a
    machine without AVX2, or under an OS that does not preserve YMM state, is an illegal
    instruction. Reach it through dotProductWide, never directly.
*/
[[nodiscard]] float dotProductAvx2(const float* left, const float* right, std::size_t count) noexcept;

/** Which kernel dotProductWide dispatches to.

    `automatic` is what ships: the widest the CPU *and* the operating system actually support,
    resolved once at load. The two explicit values exist so a test can drive both paths on one
    machine and prove they agree -- without that, the SSE2 path is untested on any developer
    machine new enough to take the AVX2 one, which is precisely the path that has to keep working
    on the old hardware this engine targets.
*/
enum class SimdPath { automatic, sse2, avx2 };

/** Forces a kernel, for tests. Not audio-thread safe and not for production use: it rewrites the
    dispatch pointer without synchronisation. Selecting avx2 on a machine that lacks it is
    refused, and the call returns the path actually in force.
*/
SimdPath setSimdPath(SimdPath path) noexcept;
[[nodiscard]] SimdPath activeSimdPath() noexcept;
/// Whether this build and this machine could use the AVX2 kernel at all.
[[nodiscard]] bool avx2Available() noexcept;

/// Dispatches to the widest kernel available. This is the one callers should use.
[[nodiscard]] float dotProductWide(const float* left, const float* right, std::size_t count) noexcept;

/** Length below which the vector kernel cannot pay for itself.

    `dotProductWide` lives in its own translation unit, so calling it costs a real call that no
    amount of optimisation removes across a static library boundary, plus a horizontal reduction
    of four accumulators at the end. Both are fixed costs. Under roughly sixteen elements neither
    is amortised -- the four-accumulator main loop does not execute even once -- and the call is
    slower than simply doing the multiplies inline.

    This is not hypothetical, and the crossover was measured rather than guessed. The WaveNet
    trunk at three channels ran 26% *slower* through the out-of-line kernel than the strided
    scalar loop it replaced; at eight channels the same kernel was 1.5x faster. A first attempt
    at sixteen gave back the whole eight-channel win, so the threshold sits at eight -- two
    four-wide iterations and a reduction, which is where it starts paying.

    The layout fix in PackedWaveNetModel and the vector kernel are separate wins; only the
    second has a length below which it stops being one.
*/
inline constexpr std::size_t dotProductWideThreshold = 8;

[[nodiscard]] inline float dotProduct(const float* left, const float* right, std::size_t count) noexcept
{
    if (count < dotProductWideThreshold)
    {
        auto sum = 0.0f;
        for (std::size_t index = 0; index < count; ++index) sum += left[index] * right[index];
        return sum;
    }
    return dotProductWide(left, right, count);
}
} // namespace nts::dsp
