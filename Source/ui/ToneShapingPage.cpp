#include "ToneShapingPage.h"

#include <array>
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
/** What each control does, in the order above.

    Written to say what moving it changes and when you would reach for it, rather than restating the
    label. A tooltip that reads "sets the sag" tells a reader who already knew, and nothing to the
    reader who did not.
*/
constexpr std::array controlHints {
    "Drive into the first preamp stage. The earliest stages set how the amp breaks up; pushing this "
    "one hard gives grit at low volume.",
    "Drive into the second preamp stage. Stacking gain across stages sounds different from taking it "
    "all in one -- more stages, more compression and sustain.",
    "Drive into the third preamp stage. Only present on higher-gain settings; adds saturation without "
    "making the front end fizzy.",
    "Drive into the fourth preamp stage. The most gain the amp offers, and the tightest low end when "
    "paired with a high Pre low cut.",
    "Shifts the clipping point off centre, so the waveform saturates asymmetrically. Adds even "
    "harmonics -- the thickness of a slightly cold-biased amp.",
    "High-pass ahead of the distortion. Raise it to stop low notes turning to mush; lower it for a "
    "fuller, looser bottom end.",
    "Low-pass ahead of the distortion. Lower it to keep pick attack from becoming fizz; raise it for "
    "more bite and air.",
    "How much the power supply sags under load. More sag gives a softer, more compressed attack that "
    "blooms; less keeps the transient sharp and immediate.",
    "Negative feedback around the power section. More feedback is tighter and cleaner; less is looser "
    "and more harmonically complex.",
    "Where the clean low path splits off from the driven path. Anything below this frequency stays "
    "clean, which keeps bass notes defined under high gain.",
    "How much of that clean low path is mixed back in. Useful on bass, or for keeping a palm-muted "
    "low string from disappearing.",
    "Delays cabinet B relative to A, in samples. This is the phase relationship between two mic "
    "positions -- small changes make large tonal ones, and it can hollow out the sum.",
    "Firmness of the front end. Higher is faster and more controlled; lower lets the amp move more "
    "under your hands.",
    "A peak around 2.8 kHz, which is where a pick hitting a string lives. Raise it for more attack, "
    "lower it if the playing sounds clicky.",
    "Level below which the gate starts closing. Set it just above the noise you want gone and below "
    "the quietest note you want kept.",
    "How far the gate ducks once closed. Around -15 dB removes the noise floor while leaving note "
    "tails intact; -90 dB is a hard mute that will cut sustain.",
    "How quickly the gate opens when you play. Fast keeps the pick attack; slow softens the front of "
    "every note.",
    "How long the gate stays open after the signal drops. Longer hold stops it chattering on decaying "
    "notes.",
    "How quickly the gate closes once hold expires. Longer sounds natural on ringing chords; shorter "
    "is tighter between staccato riffs."
};
static_assert(controlNames.size() == controlHints.size(),
              "every control needs a hint describing what it does");
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
             &oversamplingCaption, &topologySelector, &oversamplingSelector, &gateEnabled,
             &loudnessMatch })
        addAndMakeVisible(*component);

    for (std::size_t index = 0; index < sliders.size(); ++index)
    {
        tf::ui::configureLabel(labels[index], controlNames[index], 11.0f, false, theme::textSecondary);
        tf::ui::configureSlider(sliders[index]);
        // On the label as well as the slider: the label is the larger target and a reader hovering
        // to find out what something is called expects the explanation there too.
        sliders[index].setTooltip(controlHints[index]);
        labels[index].setTooltip(controlHints[index]);
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
    loudnessMatchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "loudnessMatch", loudnessMatch);
    for (std::size_t index = 0; index < sliders.size(); ++index)
        sliderAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getParameters(), controlIds[index], sliders[index]));
}

void ToneShapingPage::setPerformanceLimits(int maximumOversamplingFactor, bool)
{
    // The entries themselves rather than the whole control: the tier caps the factor, so the
    // settings still available stay selectable and only the ones it would override go grey.
    // Index 4 is "Auto", which remains valid at every tier -- it just resolves lower.
    constexpr std::array factors { 1, 2, 4, 8 };
    for (int index = 0; index < static_cast<int>(factors.size()); ++index)
        oversamplingSelector.setItemEnabled(index + 1, factors[static_cast<std::size_t>(index)]
                                                       <= maximumOversamplingFactor);
    const auto capped = maximumOversamplingFactor < 8;
    oversamplingCaption.setText(capped ? "Oversampling (capped)" : "Oversampling",
                                juce::dontSendNotification);
}

void ToneShapingPage::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(44);
    tf::ui::layOutField(top.removeFromLeft(200).reduced(0, 1), topologyCaption, topologySelector);
    top.removeFromLeft(12);
    tf::ui::layOutField(top.removeFromLeft(160).reduced(0, 1), oversamplingCaption, oversamplingSelector);
    top.removeFromLeft(12);
    gateEnabled.setBounds(top.removeFromLeft(120).withTrimmedTop(13).withHeight(28));
    loudnessMatch.setBounds(top.removeFromLeft(150).withTrimmedTop(13).withHeight(28));
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
