#include "AmplifierPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>

namespace theme = tf::theme;

namespace
{
constexpr std::array knobIds { "gain", "bass", "mid", "treble", "presence", "resonance", "master" };
constexpr std::array knobNames { "Gain", "Bass", "Mid", "Treble", "Presence", "Resonance", "Master" };
static_assert(knobIds.size() == knobNames.size(), "each amp knob needs a matching display name");
} // namespace

AmplifierPage::AmplifierPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse), faceplate(processorToUse.ampFaceplateArtwork())
{
    tf::ui::configureFieldCaption(instrumentCaption, "Instrument");
    tf::ui::populateFromParameter(instrumentSelector, processor.getParameters(), "instrument");
    addAndMakeVisible(instrumentCaption);
    addAndMakeVisible(instrumentSelector);
    addAndMakeVisible(cabinetEnabled);

    for (std::size_t index = 0; index < knobs.size(); ++index)
    {
        tf::ui::configureLabel(knobLabels[index], juce::String(knobNames[index]).toUpperCase(),
                               10.0f, true, juce::Colour(0xfff0dcc0));
        knobLabels[index].setJustificationType(juce::Justification::centred);
        tf::ui::configureKnob(knobs[index], true);
        addAndMakeVisible(knobLabels[index]);
        addAndMakeVisible(knobs[index]);
    }

    instrumentAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "instrument", instrumentSelector);
    cabinetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "cabinet", cabinetEnabled);
    for (std::size_t index = 0; index < knobs.size(); ++index)
        knobAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getParameters(), knobIds[index], knobs[index]));
}

juce::Rectangle<int> AmplifierPage::faceplateArea() const
{
    return getLocalBounds().withTrimmedTop(chipRowHeight + 10);
}

void AmplifierPage::resized()
{
    auto chip = getLocalBounds().removeFromTop(chipRowHeight);
    chipBounds = chip;
    tf::ui::layOutField(chip.removeFromLeft(160), instrumentCaption, instrumentSelector);
    chip.removeFromLeft(12);
    cabinetEnabled.setBounds(chip.removeFromLeft(130).withTrimmedTop(13).withHeight(28));

    const auto face = faceplateArea().toFloat();
    const auto width = face.getWidth();
    const auto height = face.getHeight();
    // The art is a grille above a blank control panel that runs from roughly 62% to 93% of
    // its height. Anchoring the knob row to those proportions -- and drawing the art with its
    // aspect preserved -- keeps the knobs on the panel at every window size.
    auto deck = juce::Rectangle<int>(juce::roundToInt(face.getX() + width * 0.055f),
                                     juce::roundToInt(face.getY() + height * 0.618f),
                                     juce::roundToInt(width * 0.890f),
                                     juce::roundToInt(height * 0.312f));
    constexpr int groupGap = 16;
    const auto columns = static_cast<float>(knobs.size());
    const auto columnWidth = (static_cast<float>(deck.getWidth())
                              - groupGap * static_cast<float>(groupSizes.size() - 1)) / columns;
    auto x = static_cast<float>(deck.getX());
    std::size_t knob = 0;
    for (std::size_t group = 0; group < groupSizes.size(); ++group)
    {
        for (int inGroup = 0; inGroup < groupSizes[group]; ++inGroup, ++knob)
        {
            auto cell = juce::Rectangle<float>(x, static_cast<float>(deck.getY()), columnWidth,
                                               static_cast<float>(deck.getHeight())).toNearestInt();
            knobLabels[knob].setBounds(cell.removeFromTop(15));
            knobs[knob].setBounds(cell.reduced(3, 0));
            x += columnWidth;
        }
        if (group + 1 < groupSizes.size())
        {
            dividerX[group] = juce::roundToInt(x + groupGap * 0.5f);
            x += groupGap;
        }
    }
}

void AmplifierPage::paint(juce::Graphics& graphics)
{
    auto area = faceplateArea().toFloat();
    juce::Path clip;
    clip.addRoundedRectangle(area, 9.0f);
    graphics.saveState();
    graphics.reduceClipRegion(clip);

    if (faceplate.isValid())
        // fillDestination, not stretchToFit: the art is far wider than the page, and stretching
        // it squashes the grille and the corner brackets. Cropping the sides instead keeps the
        // vertical proportions the knob layout is anchored to exactly right.
        graphics.drawImage(faceplate, area, juce::RectanglePlacement::fillDestination);
    else
    {
        graphics.setColour(theme::panel);
        graphics.fillRect(area);
    }
    // The photograph is warm and busy; a scrim keeps the knobs and text legible on top of it.
    graphics.setColour(juce::Colours::black.withAlpha(0.32f));
    graphics.fillRect(area);
    graphics.restoreState();

    for (const auto x : dividerX)
    {
        if (x <= 0) continue;
        const auto top = static_cast<float>(knobs.front().getY());
        const auto bottom = static_cast<float>(knobs.front().getBottom());
        const auto middle = (top + bottom) * 0.5f;
        graphics.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.0f),
                                                      static_cast<float>(x), top,
                                                      juce::Colours::white.withAlpha(0.10f),
                                                      static_cast<float>(x), middle, false));
        graphics.fillRect(juce::Rectangle<float>(static_cast<float>(x), top, 1.0f, middle - top));
        graphics.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.10f),
                                                      static_cast<float>(x), middle,
                                                      juce::Colours::white.withAlpha(0.0f),
                                                      static_cast<float>(x), bottom, false));
        graphics.fillRect(juce::Rectangle<float>(static_cast<float>(x), middle, 1.0f, bottom - middle));
    }
}
