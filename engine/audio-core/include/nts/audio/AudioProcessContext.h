#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace nts::audio
{
struct AudioProcessContext
{
    std::span<const float* const> inputs;
    std::span<float* const> outputs;
    std::size_t numSamples {};
    double sampleRate {};
    std::uint64_t absoluteSamplePosition {};
};
} // namespace nts::audio
