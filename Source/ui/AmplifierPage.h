#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <array>
#include <memory>
#include <vector>

/// The amplifier faceplate: the seven controls a player actually reaches for, drawn on the
/// amp art. Owns the art, the knob layout and the group dividers together, so the dividers
/// can never drift away from the knobs they separate.
class AmplifierPage final : public ModulePage
{
public:
    explicit AmplifierPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void paint(juce::Graphics& graphics) override;

private:
    [[nodiscard]] juce::Rectangle<int> faceplateArea() const;

    static constexpr int chipRowHeight = 41;
    /// Column counts of the four control groups: drive, tone stack, voicing, output.
    static constexpr std::array groupSizes { 1, 3, 2, 1 };

    juce::Image faceplate;
    juce::Rectangle<int> chipBounds;
    std::array<int, groupSizes.size() - 1> dividerX {};

    juce::Label instrumentCaption;
    juce::ComboBox instrumentSelector;
    juce::ToggleButton cabinetEnabled { "Cabinet" };
    std::array<juce::Label, 7> knobLabels;
    std::array<juce::Slider, 7> knobs;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> instrumentAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabinetAttachment;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> knobAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AmplifierPage)
};
