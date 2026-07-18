#include "dsp/AmpEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <iostream>

namespace
{
bool allFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (! std::isfinite(buffer.getSample(channel, sample)))
                return false;
    return true;
}

float renderRms(float driveDb)
{
    constexpr auto sampleRate = 48000.0;
    constexpr auto sampleCount = 4096;
    juce::AudioBuffer<float> buffer(2, sampleCount);

    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto phase = juce::MathConstants<double>::twoPi * 110.0 * sample / sampleRate;
        const auto value = static_cast<float>(0.08 * std::sin(phase));
        buffer.setSample(0, sample, value);
        buffer.setSample(1, sample, value);
    }

    tubeforge::dsp::AmpEngine engine;
    tubeforge::dsp::AmpParameters parameters;
    parameters.driveDb = driveDb;
    parameters.outputDb = -12.0f;
    engine.setParameters(parameters);
    engine.prepare(sampleRate, 512, 2);
    engine.process(buffer);

    if (! allFinite(buffer))
        return -1.0f;

    for (int sample = 0; sample < sampleCount; ++sample)
        if (std::abs(buffer.getSample(0, sample) - buffer.getSample(1, sample)) > 1.0e-6f)
            return -1.0f;

    return buffer.getRMSLevel(0, 1024, sampleCount - 1024);
}
} // namespace

int main()
{
    const auto cleanRms = renderRms(0.0f);
    const auto drivenRms = renderRms(30.0f);

    if (cleanRms <= 0.0f || drivenRms <= 0.0f)
    {
        std::cerr << "DSP produced invalid or silent output\n";
        return 1;
    }

    if (std::abs(cleanRms - drivenRms) < 1.0e-3f)
    {
        std::cerr << "Drive control did not materially change the output\n";
        return 1;
    }

    std::cout << "TubeForge DSP tests passed (clean RMS=" << cleanRms
              << ", driven RMS=" << drivenRms << ")\n";
    return 0;
}
