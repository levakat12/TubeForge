#include "nts/dsp/Simd.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define NTS_HAS_SSE2 1
#else
#define NTS_HAS_SSE2 0
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define NTS_CAN_DETECT_AVX2 1
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#include <immintrin.h>
#define NTS_CAN_DETECT_AVX2 1
#else
#define NTS_CAN_DETECT_AVX2 0
#endif

namespace nts::dsp
{
bool simdAvailable() noexcept { return NTS_HAS_SSE2 != 0; }

void multiplyGainScalar(float* samples, std::size_t count, float gain) noexcept
{
    volatile float* scalarSamples = samples;
    for (std::size_t index = 0; index < count; ++index)
        scalarSamples[index] = scalarSamples[index] * gain;
}

void multiplyGainSimd(float* samples, std::size_t count, float gain) noexcept
{
#if NTS_HAS_SSE2
    const auto gains = _mm_set1_ps(gain);
    std::size_t index {};
    for (; index + 4 <= count; index += 4)
    {
        const auto values = _mm_loadu_ps(samples + index);
        _mm_storeu_ps(samples + index, _mm_mul_ps(values, gains));
    }
    for (; index < count; ++index)
        samples[index] *= gain;
#else
    multiplyGainScalar(samples, count, gain);
#endif
}

float dotProductScalar(const float* left, const float* right, std::size_t count) noexcept
{
    auto sum = 0.0f;
    for (std::size_t index = 0; index < count; ++index) sum += left[index] * right[index];
    return sum;
}

float dotProductSse2(const float* left, const float* right, std::size_t count) noexcept
{
#if NTS_HAS_SSE2
    auto lane0 = _mm_setzero_ps(), lane1 = _mm_setzero_ps();
    auto lane2 = _mm_setzero_ps(), lane3 = _mm_setzero_ps();
    std::size_t index {};
    for (; index + 16 <= count; index += 16)
    {
        lane0 = _mm_add_ps(lane0, _mm_mul_ps(_mm_loadu_ps(left + index), _mm_loadu_ps(right + index)));
        lane1 = _mm_add_ps(lane1, _mm_mul_ps(_mm_loadu_ps(left + index + 4), _mm_loadu_ps(right + index + 4)));
        lane2 = _mm_add_ps(lane2, _mm_mul_ps(_mm_loadu_ps(left + index + 8), _mm_loadu_ps(right + index + 8)));
        lane3 = _mm_add_ps(lane3, _mm_mul_ps(_mm_loadu_ps(left + index + 12), _mm_loadu_ps(right + index + 12)));
    }
    for (; index + 4 <= count; index += 4)
        lane0 = _mm_add_ps(lane0, _mm_mul_ps(_mm_loadu_ps(left + index), _mm_loadu_ps(right + index)));

    auto sum = _mm_add_ps(_mm_add_ps(lane0, lane1), _mm_add_ps(lane2, lane3));
    // Horizontal reduction without haddps, which is SSE3 -- SSE2 is the floor for this build.
    auto shuffled = _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 0, 1));
    sum = _mm_add_ps(sum, shuffled);
    shuffled = _mm_movehl_ps(shuffled, sum);
    sum = _mm_add_ss(sum, shuffled);
    auto result = _mm_cvtss_f32(sum);

    for (; index < count; ++index) result += left[index] * right[index];
    return result;
#else
    return dotProductScalar(left, right, count);
#endif
}

namespace
{
/** Whether the AVX2 kernel can be run here.

    Three questions, and getting only the first two right is the classic way to ship a crash. The
    CPU has to implement AVX2; it also has to report OSXSAVE, meaning the OS enabled the extended
    state; and XCR0 has to show the OS is actually *preserving* the upper halves of the YMM
    registers across a context switch. An OS that does not will corrupt them silently rather than
    fault, so the XGETBV check is not belt-and-braces -- it is the only one of the three that
    catches a working CPU under an operating system that cannot use it.
*/
bool detectAvx2() noexcept
{
#if NTS_CAN_DETECT_AVX2
#if defined(_MSC_VER)
    int registers[4] {};
    __cpuid(registers, 0);
    if (registers[0] < 7) return false;
    __cpuid(registers, 1);
    constexpr int osxsaveBit = 1 << 27, avxBit = 1 << 28;
    if ((registers[2] & osxsaveBit) == 0 || (registers[2] & avxBit) == 0) return false;
    // Bits 1 and 2 of XCR0: XMM and YMM state saved by the OS.
    if ((_xgetbv(0) & 0x6) != 0x6) return false;
    __cpuidex(registers, 7, 0);
    constexpr int avx2Bit = 1 << 5;
    return (registers[1] & avx2Bit) != 0;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
#endif
#else
    return false;
#endif
}

/// Whether the AVX2 translation unit was actually built with the wider instruction set. Without
/// it the symbol exists but is a scalar stand-in, so dispatching to it would be a pessimisation.
constexpr bool avx2KernelCompiled()
{
#if defined(NTS_BUILD_AVX2)
    return true;
#else
    return false;
#endif
}

using DotProductFunction = float (*)(const float*, const float*, std::size_t) noexcept;

bool avx2Usable() noexcept
{
    // Resolved once. Both halves matter: a machine that supports AVX2 is no use if this build
    // has no AVX2 kernel in it, and a build that has one is no use on a machine without it.
    static const bool usable = avx2KernelCompiled() && detectAvx2();
    return usable;
}

DotProductFunction resolveDotProduct() noexcept
{
    return avx2Usable() ? &dotProductAvx2 : &dotProductSse2;
}

// Initialised before main, so the audio thread never races the resolution.
DotProductFunction dotProductDispatch = resolveDotProduct();
SimdPath currentPath = SimdPath::automatic;
} // namespace

bool avx2Available() noexcept { return avx2Usable(); }

SimdPath setSimdPath(SimdPath path) noexcept
{
    if (path == SimdPath::avx2 && ! avx2Usable()) path = SimdPath::sse2;
    currentPath = path;
    dotProductDispatch = path == SimdPath::sse2 ? &dotProductSse2
                       : path == SimdPath::avx2 ? &dotProductAvx2
                                                : resolveDotProduct();
    return currentPath;
}

SimdPath activeSimdPath() noexcept { return currentPath; }

float dotProductWide(const float* left, const float* right, std::size_t count) noexcept
{
    return dotProductDispatch(left, right, count);
}

} // namespace nts::dsp
