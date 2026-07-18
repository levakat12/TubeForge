#pragma once

#include <atomic>

namespace nts::audio
{
struct RuntimeParameters
{
    std::atomic<float> inputGainDb { 0.0f };
    std::atomic<float> outputGainDb { 0.0f };
    std::atomic<bool> bypass { false };
};

struct ParameterSnapshot
{
    float inputGainDb {};
    float outputGainDb {};
    bool bypass {};
};

inline ParameterSnapshot snapshot(const RuntimeParameters& parameters) noexcept
{
    return {
        parameters.inputGainDb.load(std::memory_order_relaxed),
        parameters.outputGainDb.load(std::memory_order_relaxed),
        parameters.bypass.load(std::memory_order_relaxed)
    };
}
} // namespace nts::audio
