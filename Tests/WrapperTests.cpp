#include "TestHarness.h"
#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <memory>

namespace
{
void testProcessorStateAndAudio(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 32);
    processor.prepareToPlay(48000.0, 32);

    juce::AudioBuffer<float> buffer(2, 32);
    juce::MidiBuffer midi;
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        buffer.setSample(0, sample, 0.1f);
        buffer.setSample(1, sample, -0.1f);
    }
    processor.processBlock(buffer, midi);

    auto finite = true;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            finite = finite && std::isfinite(buffer.getSample(channel, sample));
    tests.expect(finite, "JUCE wrapper processes a valid block");

    juce::MemoryBlock before;
    processor.getStateInformation(before);
    constexpr char invalidState[] = "{broken-state";
    processor.setStateInformation(invalidState, static_cast<int>(sizeof(invalidState) - 1));
    juce::MemoryBlock after;
    processor.getStateInformation(after);
    tests.expectEqual(before, after, "invalid wrapper state leaves the last known good state active");
}

void testEditorLifecycle(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    for (int iteration = 0; iteration < 250; ++iteration)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        tests.expect(editor != nullptr, "editor can be created repeatedly");
        if (editor == nullptr)
            break;
        editor->setSize(620 + (iteration % 20), 440 + (iteration % 20));
    }
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    TestHarness tests;
    testProcessorStateAndAudio(tests);
    testEditorLifecycle(tests);
    return tests.result();
}
