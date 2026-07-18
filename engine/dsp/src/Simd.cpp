#include "nts/dsp/Simd.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define NTS_HAS_SSE2 1
#else
#define NTS_HAS_SSE2 0
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
} // namespace nts::dsp
