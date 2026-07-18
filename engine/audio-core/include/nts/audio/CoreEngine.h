#pragma once

#include "IAudioProcessor.h"
#include "LinearSmoother.h"
#include "MeterState.h"
#include "ProcessorConcept.h"
#include "RuntimeParameters.h"

#include <cstddef>

namespace nts::audio
{
class CoreEngine final : public IAudioProcessor
{
public:
    explicit CoreEngine(RuntimeParameters& runtimeParameters, MeterState& meterState) noexcept;

    void prepare(double sampleRate,
                 std::size_t maxBlockSize,
                 std::size_t numInputChannels,
                 std::size_t numOutputChannels) override;
    void reset() noexcept override;
    void process(AudioProcessContext& context) noexcept override;

    [[nodiscard]] double preparedSampleRate() const noexcept { return currentSampleRate; }
    [[nodiscard]] std::size_t preparedMaximumBlockSize() const noexcept { return maximumBlockSize; }

private:
    static float decibelsToGain(float decibels) noexcept;

    RuntimeParameters& parameters;
    MeterState& meters;
    LinearSmoother inputGain;
    LinearSmoother outputGain;
    double currentSampleRate { 44100.0 };
    std::size_t maximumBlockSize {};
    std::size_t inputChannels {};
    std::size_t outputChannels {};
    bool isPrepared {};
};

static_assert(AudioProcessor<CoreEngine>);
} // namespace nts::audio
