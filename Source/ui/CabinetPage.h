#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <array>
#include <memory>

/// Cabinet responses and the controls that blend them.
///
/// Two slots, each either the built-in response or one loaded from a file. Loading happens off
/// the message thread and fades in, so a response can be swapped while playing.
class CabinetPage final : public ModulePage
{
public:
    explicit CabinetPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;
    void setPerformanceLimits(int maximumOversamplingFactor, bool singleCabinet) override;

private:
    /// One cabinet slot's controls. Two of these, differing only in which slot they drive.
    struct SlotControls
    {
        juce::Label caption;
        juce::TextButton load { "Load response" };
        juce::TextButton clear { "Use built-in" };
        juce::Label status;
    };

    void chooseImpulse(int slot);

    tf::ui::SectionPanel slotPanel { "Cabinet responses" };
    tf::ui::SectionPanel alignPanel { "Cabinet section" };
    std::array<SlotControls, 2> slots;

    juce::ToggleButton cabinetEnabled { "Cabinet section active" };
    juce::Label alignmentCaption, blendCaption, widthCaption;
    juce::Slider alignment, blend, width;
    /// What a mix bus will do to the stereo image. See CabinetPage::refresh.
    juce::Label monoFold;
    juce::Label help;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabinetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> alignmentAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> blendAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> widthAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CabinetPage)
};
