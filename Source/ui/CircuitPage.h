#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <nts/circuit/CircuitTypes.h>

#include <array>
#include <memory>
#include <vector>

/// Draws the compiled circuit: the node chain, its live telemetry, and the tone stack's
/// frequency response.
class CircuitSchematicView final : public juce::Component
{
public:
    void setSnapshot(std::vector<nts::circuit::NodeTelemetry> telemetry, juce::String status,
                     nts::circuit::CircuitGraphDescription graph);
    void paint(juce::Graphics& graphics) override;
private:
    std::vector<nts::circuit::NodeTelemetry> stages;
    juce::String statusText;
    nts::circuit::CircuitGraphDescription circuit;
};

/// Choosing the real components the physical engine builds the amp from.
class CircuitPage final : public ModulePage
{
public:
    explicit CircuitPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;

private:
    static constexpr int selectorCount = 6;

    std::array<juce::Label, selectorCount> captions;
    juce::ComboBox preampTube;
    juce::ComboBox powerTube;
    juce::ComboBox powerTopology;
    juce::ComboBox toneStack;
    juce::ComboBox backend;
    juce::ComboBox cabinetStyle;
    CircuitSchematicView schematic;

    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>,
               selectorCount> attachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CircuitPage)
};
