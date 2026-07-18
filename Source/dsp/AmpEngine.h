#pragma once

#include "Biquad.h"
#include "RecurrentAmpModel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace tubeforge::dsp
{
struct AmpParameters
{
    float inputDb { 0.0f };
    float driveDb { 12.0f };
    float bassDb { 0.0f };
    float midDb { 0.0f };
    float trebleDb { 0.0f };
    float presenceDb { 0.0f };
    float mix { 1.0f };
    float outputDb { -6.0f };
};

class AmpEngine
{
public:
    static constexpr int maximumChannels = 2;

    void prepare(double newSampleRate, int maximumBlockSize, int numberOfChannels);
    void reset() noexcept;
    void setParameters(const AmpParameters& newParameters) noexcept;
    void process(juce::AudioBuffer<float>& buffer) noexcept;

private:
    struct ChannelState
    {
        Biquad inputHighPass;
        Biquad bass;
        Biquad mid;
        Biquad treble;
        Biquad presence;
        Biquad dcBlocker;
        RecurrentAmpModel model;

        void reset() noexcept;
    };

    void updateToneFilters() noexcept;
    static float decibelsToGain(float decibels) noexcept;

    std::array<ChannelState, maximumChannels> channels;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> inputGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> driveGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outputGain;
    AmpParameters parameters;
    double sampleRate { 44100.0 };
    int activeChannels { maximumChannels };
    bool prepared { false };
};
} // namespace tubeforge::dsp
