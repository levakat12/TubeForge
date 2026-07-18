#include "nts/audio/CoreEngine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace nts::audio
{
CoreEngine::CoreEngine(RuntimeParameters& runtimeParameters, MeterState& meterState) noexcept
    : parameters(runtimeParameters), meters(meterState)
{
}

void CoreEngine::prepare(double sampleRate,
                         std::size_t maxBlockSize,
                         std::size_t numInputChannels,
                         std::size_t numOutputChannels)
{
    currentSampleRate = std::max(8000.0, sampleRate);
    maximumBlockSize = maxBlockSize;
    inputChannels = numInputChannels;
    outputChannels = numOutputChannels;

    constexpr auto smoothingSeconds = 0.02;
    inputGain.prepare(currentSampleRate, smoothingSeconds);
    outputGain.prepare(currentSampleRate, smoothingSeconds);

    const auto values = snapshot(parameters);
    inputGain.reset(decibelsToGain(values.inputGainDb));
    outputGain.reset(decibelsToGain(values.outputGainDb));
    isPrepared = true;
}

void CoreEngine::reset() noexcept
{
    const auto values = snapshot(parameters);
    inputGain.reset(decibelsToGain(values.inputGainDb));
    outputGain.reset(decibelsToGain(values.outputGainDb));
}

void CoreEngine::process(AudioProcessContext& context) noexcept
{
    if (! isPrepared)
        return;

    const auto values = snapshot(parameters);
    inputGain.setTarget(decibelsToGain(values.inputGainDb));
    outputGain.setTarget(decibelsToGain(values.outputGainDb));

    const auto readableChannels = std::min(inputChannels, context.inputs.size());
    const auto writableChannels = std::min(outputChannels, context.outputs.size());
    const auto copiedChannels = std::min(readableChannels, writableChannels);
    std::array<float, MeterState::maximumChannels> inputPeaks {};
    std::array<float, MeterState::maximumChannels> outputPeaks {};

    for (std::size_t sample = 0; sample < context.numSamples; ++sample)
    {
        const auto inGain = inputGain.next();
        const auto outGain = outputGain.next();
        const auto combinedGain = inGain * outGain;

        for (std::size_t channel = 0; channel < copiedChannels; ++channel)
        {
            const auto input = context.inputs[channel][sample];
            const auto output = values.bypass ? input : input * combinedGain;
            context.outputs[channel][sample] = output;

            if (channel < MeterState::maximumChannels)
            {
                inputPeaks[channel] = std::max(inputPeaks[channel], std::abs(input));
                outputPeaks[channel] = std::max(outputPeaks[channel], std::abs(output));
            }
        }

        for (std::size_t channel = copiedChannels; channel < writableChannels; ++channel)
            context.outputs[channel][sample] = 0.0f;
    }

    for (std::size_t channel = 0; channel < MeterState::maximumChannels; ++channel)
        meters.publish(channel, inputPeaks[channel], outputPeaks[channel]);
}

float CoreEngine::decibelsToGain(float decibels) noexcept
{
    return std::pow(10.0f, decibels / 20.0f);
}
} // namespace nts::audio
