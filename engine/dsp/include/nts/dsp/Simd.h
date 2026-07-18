#pragma once

#include <cstddef>

namespace nts::dsp
{
[[nodiscard]] bool simdAvailable() noexcept;
void multiplyGainScalar(float* samples, std::size_t count, float gain) noexcept;
void multiplyGainSimd(float* samples, std::size_t count, float gain) noexcept;
} // namespace nts::dsp
