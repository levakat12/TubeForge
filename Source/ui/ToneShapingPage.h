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
    void refresh() override;
    void setPerformanceLimits(int maximumOversamplingFactor, bool singleCabinet) override;

    static constexpr int controlCount = 23;

private:
    juce::Label topologyCaption;
    juce::Label oversamplingCaption;
    juce::ComboBox topologySelector;
    juce::ComboBox oversamplingSelector;
    juce::ToggleButton gateEnabled { "Noise gate" };
    juce::ToggleButton loudnessMatch { "Loudness match" };

    std::array<tf::ui::SectionPanel, 5> groups { tf::ui::SectionPanel { "Preamp" },
                                                 tf::ui::SectionPanel { "Filter & feel" },
                                                 tf::ui::SectionPanel { "Power & cabinet" },
                                                 tf::ui::SectionPanel { "Bass bi-amp" },
                                                 tf::ui::SectionPanel { "Noise gate" } };
    std::array<juce::Label, controlCount> labels;
    std::array<juce::Slider, controlCount> sliders;

    /** Refills the topology list for the current instrument, keeping item ids equal to topology
        index + 1, and reselects whatever the parameter currently holds.

        Called on construction and whenever `refresh` sees the instrument move.
    */
    void rebuildTopologyList();
    /// The instrument the list was last built for, so `refresh` only rebuilds when it changes.
    int listedInstrument { -1 };

    /* No `ComboBoxAttachment` for the topology, and the reason is a trap rather than a preference.

       `juce::ComboBoxParameterAttachment` maps the box's **selected position** to the parameter
       value -- not its item id. That is fine for a list that always shows every choice and
       silently wrong for one that does not: hiding the bass-native voicings from a guitarist
       would leave "Sagging Rectifier" at position 5 writing 5 in one instrument and something
       else in the other, and nothing would report it.

       So this box is driven by hand, with the item id carrying the topology index. Ids are stable
       identifiers; positions are not, and the difference only shows up once something is filtered.
    */
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> oversamplingAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> gateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loudnessMatchAttachment;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneShapingPage)
};
