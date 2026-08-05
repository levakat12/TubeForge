// The AVX2 half of the dot-product kernel, and nothing else.
//
// This translation unit is compiled with a wider instruction set than the rest of the engine, so
// it must contain no code that can run before the CPU has been checked. One function, called only
// through the pointer that Simd.cpp resolves after testing CPUID and the OS's YMM support -- no
// static initialisers, no globals, nothing with a constructor. Adding anything else here risks the
// compiler emitting a wide instruction on a path that executes at load time, which on a pre-AVX2
// machine is an illegal-instruction crash before main.

#include "nts/dsp/Simd.h"

#if defined(__AVX2__) || defined(NTS_BUILD_AVX2)
#include <immintrin.h>
#define NTS_HAS_AVX2_KERNEL 1
#else
#define NTS_HAS_AVX2_KERNEL 0
#endif

namespace nts::dsp
{
#if NTS_HAS_AVX2_KERNEL
float dotProductAvx2(const float* left, const float* right, std::size_t count) noexcept
{
    // Eight-wide against SSE2's four, and fused multiply-add, so each iteration does twice the
    // lanes in one instruction instead of two. Four accumulators for the same reason as the SSE2
    // kernel: the loop is throughput-bound only when several partial sums are in flight.
    auto lane0 = _mm256_setzero_ps(), lane1 = _mm256_setzero_ps();
    auto lane2 = _mm256_setzero_ps(), lane3 = _mm256_setzero_ps();
    std::size_t index {};
    for (; index + 32 <= count; index += 32)
    {
        lane0 = _mm256_fmadd_ps(_mm256_loadu_ps(left + index), _mm256_loadu_ps(right + index), lane0);
        lane1 = _mm256_fmadd_ps(_mm256_loadu_ps(left + index + 8), _mm256_loadu_ps(right + index + 8), lane1);
        lane2 = _mm256_fmadd_ps(_mm256_loadu_ps(left + index + 16), _mm256_loadu_ps(right + index + 16), lane2);
        lane3 = _mm256_fmadd_ps(_mm256_loadu_ps(left + index + 24), _mm256_loadu_ps(right + index + 24), lane3);
    }
    for (; index + 8 <= count; index += 8)
        lane0 = _mm256_fmadd_ps(_mm256_loadu_ps(left + index), _mm256_loadu_ps(right + index), lane0);

    const auto wide = _mm256_add_ps(_mm256_add_ps(lane0, lane1), _mm256_add_ps(lane2, lane3));
    auto narrow = _mm_add_ps(_mm256_castps256_ps128(wide), _mm256_extractf128_ps(wide, 1));
    auto shuffled = _mm_shuffle_ps(narrow, narrow, _MM_SHUFFLE(2, 3, 0, 1));
    narrow = _mm_add_ps(narrow, shuffled);
    shuffled = _mm_movehl_ps(shuffled, narrow);
    narrow = _mm_add_ss(narrow, shuffled);
    auto result = _mm_cvtss_f32(narrow);

    for (; index < count; ++index) result += left[index] * right[index];
    return result;
}
#else
// Built without the wider instruction set. The symbol still has to exist so Simd.cpp links, but
// it can never be selected: resolveDotProduct only reaches it when the runtime check passes, and
// that check is compiled out alongside this.
float dotProductAvx2(const float* left, const float* right, std::size_t count) noexcept
{
    return dotProductScalar(left, right, count);
}
#endif
} // namespace nts::dsp
