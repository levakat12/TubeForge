#pragma once

namespace nts::diagnostics
{
struct LatencyBudget
{
    int hostSamples {};
    int oversamplingSamples {};
    int convolutionSamples {};
    int neuralSamples {};
    int lookaheadSamples {};
    int resamplingSamples {};

    [[nodiscard]] constexpr int processingSamples() const noexcept
    {
        return oversamplingSamples + convolutionSamples + neuralSamples + lookaheadSamples + resamplingSamples;
    }

    [[nodiscard]] constexpr int totalSamples() const noexcept
    {
        return hostSamples + processingSamples();
    }
};
} // namespace nts::diagnostics
