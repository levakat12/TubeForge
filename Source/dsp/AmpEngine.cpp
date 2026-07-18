#include "AmpEngine.h"

#include <algorithm>
#include <cmath>

namespace tubeforge::dsp
{
void AmpEngine::ChannelState::reset() noexcept
{
    inputHighPass.reset();
    bass.reset();
    mid.reset();
    treble.reset();
    presence.reset();
    dcBlocker.reset();
    model.reset();
}

void AmpEngine::prepare(double newSampleRate, int maximumBlockSize, int numberOfChannels)
{
    juce::ignoreUnused(maximumBlockSize);
    sampleRate = std::max(8000.0, newSampleRate);
    activeChannels = std::clamp(numberOfChannels, 1, maximumChannels);

    constexpr auto smoothingSeconds = 0.02;
    inputGain.reset(sampleRate, smoothingSeconds);
    driveGain.reset(sampleRate, smoothingSeconds);
    mix.reset(sampleRate, smoothingSeconds);
    outputGain.reset(sampleRate, smoothingSeconds);

    inputGain.setCurrentAndTargetValue(decibelsToGain(parameters.inputDb));
    driveGain.setCurrentAndTargetValue(decibelsToGain(parameters.driveDb));
    mix.setCurrentAndTargetValue(parameters.mix);
    outputGain.setCurrentAndTargetValue(decibelsToGain(parameters.outputDb));

    for (auto& channel : channels)
    {
        channel.inputHighPass.setHighPass(sampleRate, 25.0f);
        channel.dcBlocker.setHighPass(sampleRate, 12.0f);
        channel.reset();
    }

    updateToneFilters();
    prepared = true;
}

void AmpEngine::reset() noexcept
{
    for (auto& channel : channels)
        channel.reset();
}

void AmpEngine::setParameters(const AmpParameters& newParameters) noexcept
{
    parameters = newParameters;
    parameters.mix = std::clamp(parameters.mix, 0.0f, 1.0f);

    inputGain.setTargetValue(decibelsToGain(parameters.inputDb));
    driveGain.setTargetValue(decibelsToGain(parameters.driveDb));
    mix.setTargetValue(parameters.mix);
    outputGain.setTargetValue(decibelsToGain(parameters.outputDb));

    if (prepared)
        updateToneFilters();
}

void AmpEngine::process(juce::AudioBuffer<float>& buffer) noexcept
{
    if (! prepared)
        return;

    const auto channelCount = std::min({ buffer.getNumChannels(), activeChannels, maximumChannels });
    const auto sampleCount = buffer.getNumSamples();

    std::array<float*, maximumChannels> writePointers {};
    for (int channel = 0; channel < channelCount; ++channel)
        writePointers[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);

    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto currentInputGain = inputGain.getNextValue();
        const auto currentDriveGain = driveGain.getNextValue();
        const auto currentMix = mix.getNextValue();
        const auto currentOutputGain = outputGain.getNextValue();

        for (int channel = 0; channel < channelCount; ++channel)
        {
            auto& state = channels[static_cast<std::size_t>(channel)];
            auto& sampleValue = writePointers[static_cast<std::size_t>(channel)][sample];
            const auto dry = sampleValue;
            auto wet = state.inputHighPass.process(dry * currentInputGain);
            wet = state.model.process(wet, currentDriveGain);
            wet = state.dcBlocker.process(wet);
            wet = state.bass.process(wet);
            wet = state.mid.process(wet);
            wet = state.treble.process(wet);
            wet = state.presence.process(wet);

            const auto blended = dry + currentMix * (wet - dry);
            sampleValue = std::tanh(blended * currentOutputGain);
        }
    }
}

void AmpEngine::updateToneFilters() noexcept
{
    for (auto& channel : channels)
    {
        channel.bass.setLowShelf(sampleRate, 140.0f, parameters.bassDb);
        channel.mid.setPeak(sampleRate, 750.0f, 0.75f, parameters.midDb);
        channel.treble.setHighShelf(sampleRate, 3200.0f, parameters.trebleDb);
        channel.presence.setPeak(sampleRate, 6000.0f, 0.7f, parameters.presenceDb);
    }
}

float AmpEngine::decibelsToGain(float decibels) noexcept
{
    return std::pow(10.0f, decibels / 20.0f);
}
} // namespace tubeforge::dsp
