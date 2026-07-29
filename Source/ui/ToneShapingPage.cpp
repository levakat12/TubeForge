#include "ToneShapingPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

namespace theme = tf::theme;

namespace
{
constexpr std::array controlIds { "stage1", "stage2", "stage3", "stage4", "bias", "lowCut", "highCut",
                                  "sag", "feedback", "crossover", "cleanBlend", "cabinetAlignment",
                                  "tightness", "pickEmphasis",
                                  "gateThreshold", "gateDepth", "gateAttack", "gateHold", "gateRelease" };
constexpr std::array controlNames { "Stage 1 gain", "Stage 2 gain", "Stage 3 gain", "Stage 4 gain",
                                    "Bias", "Pre low cut", "Pre high cut", "Sag", "Feedback",
                                    "Bass crossover", "Clean blend", "Cab alignment", "Tightness",
                                    "Pick emphasis",
                                    "Threshold", "Depth", "Attack", "Hold", "Release" };
static_assert(controlIds.size() == ToneShapingPage::controlCount,
              "the parameter list and the control array must stay the same length");
static_assert(controlIds.size() == controlNames.size(),
              "each control needs a matching display name");

/// Which controls belong to which card. -1 pads a group shorter than the longest.
constexpr std::array<std::array<int, 5>, 4> groupMembers { { { 0, 1, 2, 3, 4 },
                                                             { 5, 6, 12, 13, -1 },
                                                             { 7, 8, 9, 10, 11 },
                                                             { 14, 15, 16, 17, 18 } } };
} // namespace

ToneShapingPage::ToneShapingPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    tf::ui::configureFieldCaption(topologyCaption, "Topology");
    tf::ui::configureFieldCaption(oversamplingCaption, "Oversampling");
    tf::ui::populateFromParameter(topologySelector, processor.getParameters(), "topology");
    tf::ui::populateFromParameter(oversamplingSelector, processor.getParameters(), "oversampling");
    for (auto* component : std::initializer_list<juce::Component*> { &topologyCaption,
             &oversamplingCaption, &topologySelector, &oversamplingSelector, &gateEnabled })
        addAndMakeVisible(*component);

    for (std::size_t index = 0; index < sliders.size(); ++index)
    {
        tf::ui::configureLabel(labels[index], controlNames[index], 11.0f, false, theme::textSecondary);
        tf::ui::configureSlider(sliders[index]);
    }
    for (std::size_t group = 0; group < groups.size(); ++group)
    {
        addAndMakeVisible(groups[group]);
        for (const auto member : groupMembers[group])
        {
            if (member < 0) continue;
            groups[group].addAndMakeVisible(labels[static_cast<std::size_t>(member)]);
            groups[group].addAndMakeVisible(sliders[static_cast<std::size_t>(member)]);
        }
    }

    topologyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "topology", topologySelector);
    oversamplingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "oversampling", oversamplingSelector);
    gateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "gateEnabled", gateEnabled);
    for (std::size_t index = 0; index < sliders.size(); ++index)
        sliderAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getParameters(), controlIds[index], sliders[index]));
}

void ToneShapingPage::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(44);
    tf::ui::layOutField(top.removeFromLeft(200).reduced(0, 1), topologyCaption, topologySelector);
    top.removeFromLeft(12);
    tf::ui::layOutField(top.removeFromLeft(160).reduced(0, 1), oversamplingCaption, oversamplingSelector);
    top.removeFromLeft(12);
    gateEnabled.setBounds(top.removeFromLeft(140).withTrimmedTop(13).withHeight(28));
    area.removeFromTop(10);

    const auto groupWidth = area.getWidth() / 2;
    const auto groupHeight = area.getHeight() / 2;
    for (std::size_t group = 0; group < groups.size(); ++group)
    {
        const auto column = static_cast<int>(group % 2);
        const auto row = static_cast<int>(group / 2);
        groups[group].setBounds(juce::Rectangle<int>(area.getX() + column * groupWidth,
                                                     area.getY() + row * groupHeight,
                                                     groupWidth, groupHeight).reduced(5));
        auto content = groups[group].contentArea();
        const auto rows = static_cast<int>(groupMembers[group].size());
        const auto rowHeight = content.getHeight() / rows;
        for (const auto member : groupMembers[group])
        {
            auto cell = content.removeFromTop(rowHeight);
            if (member < 0) continue;
            labels[static_cast<std::size_t>(member)].setBounds(cell.removeFromLeft(108));
            sliders[static_cast<std::size_t>(member)].setBounds(cell);
        }
    }
}
