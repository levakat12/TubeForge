#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <array>
#include <memory>
#include <vector>

/// The nineteen fine controls, grouped by what they do rather than by the order they happen
/// to sit in the parameter list.
class ToneShapingPage final : public ModulePage
{
public:
    explicit ToneShapingPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void setPerformanceLimits(int maximumOversamplingFactor, bool singleCabinet) override;

    static constexpr int controlCount = 19;

private:
    juce::Label topologyCaption;
    juce::Label oversamplingCaption;
    juce::ComboBox topologySelector;
    juce::ComboBox oversamplingSelector;
    juce::ToggleButton gateEnabled { "Noise gate" };
    juce::ToggleButton loudnessMatch { "Loudness match" };

    std::array<tf::ui::SectionPanel, 4> groups { tf::ui::SectionPanel { "Preamp" },
                                                 tf::ui::SectionPanel { "Filter & feel" },
                                                 tf::ui::SectionPanel { "Power & cabinet" },
                                                 tf::ui::SectionPanel { "Noise gate" } };
    std::array<juce::Label, controlCount> labels;
    std::array<juce::Slider, controlCount> sliders;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> topologyAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> oversamplingAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> gateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loudnessMatchAttachment;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneShapingPage)
};
