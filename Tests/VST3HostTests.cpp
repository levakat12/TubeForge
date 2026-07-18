#include "TestHarness.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

int main(int argumentCount, char** arguments)
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    TestHarness tests;

    if (argumentCount < 2)
    {
        tests.expect(false, "VST3 bundle path argument is required");
        return tests.result();
    }

    const juce::String bundlePath = juce::String::fromUTF8(arguments[1]);
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundlePath);
    tests.expect(! descriptions.isEmpty(), "JUCE host scans the TubeForge VST3 bundle");
    if (descriptions.isEmpty())
        return tests.result();

    juce::String error;
    auto instance = format.createInstanceFromDescription(*descriptions[0], 48000.0, 64, error);
    tests.expect(instance != nullptr, "JUCE host instantiates the TubeForge VST3: " + error.toStdString());
    if (instance == nullptr)
        return tests.result();

    instance->prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    buffer.clear();
    buffer.setSample(0, 0, 0.25f);
    buffer.setSample(1, 0, -0.25f);
    instance->processBlock(buffer, midi);
    tests.expect(std::isfinite(buffer.getSample(0, 0)) && std::isfinite(buffer.getSample(1, 0)),
                 "hosted VST3 processes audio without invalid samples");

    for (int iteration = 0; iteration < 25; ++iteration)
    {
        auto* editor = instance->createEditorAndMakeActive();
        tests.expect(editor != nullptr, "hosted VST3 editor opens repeatedly");
        if (editor == nullptr)
            break;
        delete editor;
    }
    instance->releaseResources();
    return tests.result();
}
