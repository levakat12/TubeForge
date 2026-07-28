#include "TestHarness.h"
#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <algorithm>
#include <memory>

namespace
{
void testProcessorStateAndAudio(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 32);
    processor.prepareToPlay(48000.0, 32);
    tests.expectEqual(processor.getLatencySamples(), 0,
                      "wrapper defaults to zero algorithmic latency at 1x oversampling");

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

    tests.expect(static_cast<juce::AudioProcessor&>(processor).getParameters().size() >= 28,
                 "wrapper exposes simple and advanced traditional-amp controls");
    auto allAutomatable = true;
    for (auto* parameter : static_cast<juce::AudioProcessor&>(processor).getParameters())
        allAutomatable = allAutomatable && parameter != nullptr && parameter->isAutomatable();
    tests.expect(allAutomatable, "all traditional-amp controls are host-automatable");

    auto* gain = processor.getParameters().getParameter("gain");
    tests.expect(gain != nullptr, "amp gain parameter exists");
    if (gain != nullptr)
    {
        gain->setValueNotifyingHost(gain->convertTo0to1(8.25f));
        juce::MemoryBlock ampState;
        processor.getStateInformation(ampState);
        gain->setValueNotifyingHost(gain->convertTo0to1(1.0f));
        processor.setStateInformation(ampState.getData(), static_cast<int>(ampState.getSize()));
        tests.expectNear(gain->convertFrom0to1(gain->getValue()), 8.25, 0.01,
                         "project state restores amp controls");
    }

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

void testPhysicalCircuitEditing(TestHarness& tests)
{
    TubeForgeAudioProcessor processor;
    const auto select = [&processor](const char* id, float choice)
    {
        if (auto* parameter = processor.getParameters().getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(choice));
    };
    select("circuitPreampTube", 0.0f);
    select("circuitPowerTube", 0.0f);
    select("circuitPowerTopology", 0.0f);
    select("circuitToneStack", 2.0f);
    select("circuitBackend", 1.0f);
    select("circuitCabinetStyle", 2.0f);

    const auto edited = processor.circuitGraphSnapshot();
    const auto triode = std::find_if(edited.nodes.begin(), edited.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    const auto power = std::find_if(edited.nodes.begin(), edited.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::powerStage; });
    const auto tone = std::find_if(edited.nodes.begin(), edited.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::toneStack; });
    const auto cabinet = std::find_if(edited.nodes.begin(), edited.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::cabinet; });
    tests.expect(triode != edited.nodes.end() && triode->modelId == "tube.12au7.v1"
                 && triode->backend == nts::circuit::ModelBackend::numerical,
                 "preamp selector switches both tube definition and solver backend");
    tests.expect(power != edited.nodes.end() && power->modelId == "power.6v6.v1"
                 && nts::circuit::parameterValue(*power, "topology", -1.0f) == 0.0f,
                 "power selectors switch tube family and single-ended topology");
    tests.expect(tone != edited.nodes.end() && tone->modelId == "passive.fmv.bass.v1",
                 "tone-stack selector switches the electrical network");
    tests.expect(cabinet != edited.nodes.end() && cabinet->modelId == "cabinet.bass-sealed.v1",
                 "cabinet selector switches the active cabinet part");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    select("circuitPreampTube", 2.0f);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    const auto restored = processor.circuitGraphSnapshot();
    const auto restoredTriode = std::find_if(restored.nodes.begin(), restored.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    tests.expect(restoredTriode != restored.nodes.end() && restoredTriode->modelId == "tube.12au7.v1",
                 "project state restores the complete physical-circuit selection");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    TestHarness tests;
    testProcessorStateAndAudio(tests);
    testPhysicalCircuitEditing(tests);
    testEditorLifecycle(tests);
    return tests.result();
}
