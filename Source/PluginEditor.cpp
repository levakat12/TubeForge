#include "PluginEditor.h"
#include "PluginProcessor.h"

TubeForgeAudioProcessorEditor::TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& processor)
    : juce::GenericAudioProcessorEditor(processor)
{
    setSize(520, 540);
}
