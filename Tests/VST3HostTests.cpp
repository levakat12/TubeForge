#include "TestHarness.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <memory>

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
    instance->setNonRealtime(true);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    buffer.clear();
    buffer.setSample(0, 0, 0.25f);
    buffer.setSample(1, 0, -0.25f);
    instance->processBlock(buffer, midi);
    tests.expect(std::isfinite(buffer.getSample(0, 0)) && std::isfinite(buffer.getSample(1, 0)),
                 "hosted VST3 processes audio without invalid samples");
    tests.expect(instance->getLatencySamples() >= 0, "hosted VST3 reports a valid latency");

    auto parameters = instance->getParameters();
    if (! parameters.isEmpty())
    {
        parameters[0]->beginChangeGesture(); parameters[0]->setValueNotifyingHost(0.72f); parameters[0]->endChangeGesture();
        buffer.clear(); buffer.setSample(0, 0, 0.1f); buffer.setSample(1, 0, 0.1f);
        instance->processBlock(buffer, midi);
        tests.expect(std::isfinite(buffer.getSample(0, 0)), "host automation processes safely");
    }

    juce::MemoryBlock savedState; instance->getStateInformation(savedState);
    tests.expect(savedState.getSize() > 0, "hosted VST3 serializes state");
    auto second = format.createInstanceFromDescription(*descriptions[0], 48000.0, 64, error);
    tests.expect(second != nullptr, "host creates a second simultaneous VST3 instance");
    if (second != nullptr)
    {
        second->setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
        second->prepareToPlay(48000.0, 64); juce::AudioBuffer<float> secondBuffer(2, 64); secondBuffer.clear();
        secondBuffer.setSample(0, 0, 0.2f); second->processBlock(secondBuffer, midi);
        tests.expect(std::isfinite(secondBuffer.getSample(0, 0)), "restored state and multi-instance processing are stable");
        second->releaseResources();
    }

    for (const auto [sampleRate, blockSize] : { std::pair { 44100.0, 32 }, std::pair { 96000.0, 257 },
                                                std::pair { 192000.0, 512 } })
    {
        auto rateInstance = format.createInstanceFromDescription(*descriptions[0], sampleRate, blockSize, error);
        tests.expect(rateInstance != nullptr, "host instantiates supported sample-rate and buffer combination");
        if (rateInstance == nullptr) continue;
        rateInstance->prepareToPlay(sampleRate, blockSize);
        juce::AudioBuffer<float> rateBuffer(2, blockSize); rateBuffer.clear(); rateBuffer.setSample(0, 0, 0.1f);
        rateInstance->processBlock(rateBuffer, midi);
        tests.expect(std::isfinite(rateBuffer.getSample(0, 0)), "offline sample-rate matrix processes finite audio");
        rateInstance->releaseResources();
    }

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
