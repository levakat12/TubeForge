#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class TubeForgeAudioProcessor;

class TubeForgeAudioProcessorEditor final : public juce::GenericAudioProcessorEditor
{
public:
    explicit TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& processor);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TubeForgeAudioProcessorEditor)
};
