#include "TunerPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <nts/dsp/PitchDetector.h>

#include <cmath>

namespace theme = tf::theme;

TunerPage::TunerPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    tf::ui::configureLabel(noteLabel, "--", 54.0f, true, theme::textPrimary);
    noteLabel.setJustificationType(juce::Justification::centred);
    tf::ui::configureLabel(centsLabel, "no signal", 14.0f, false, theme::textSecondary);
    centsLabel.setJustificationType(juce::Justification::centred);
    tf::ui::configureLabel(frequencyLabel, "", 12.0f, false, theme::textTertiary);
    frequencyLabel.setJustificationType(juce::Justification::centred);
    tf::ui::configureLabel(hint,
                           "Tracks the input before the amplifier, so the reading does not change "
                           "with gain or cabinet. Muting silences the output only -- tuning keeps "
                           "working while it is quiet.",
                           11.0f, false, theme::textTertiary);
    hint.setJustificationType(juce::Justification::topLeft);

    addAndMakeVisible(meterPanel);
    addAndMakeVisible(hint);
    for (auto* component : std::initializer_list<juce::Component*> {
             &noteLabel, &centsLabel, &frequencyLabel, &muteWhileTuning })
        meterPanel.addAndMakeVisible(*component);

    muteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "tunerMute", muteWhileTuning);
}

juce::Rectangle<int> TunerPage::needleArea() const
{
    auto content = meterPanel.contentArea();
    content.removeFromTop(96);
    return content.removeFromTop(46).reduced(6, 0);
}

void TunerPage::resized()
{
    auto area = getLocalBounds();
    meterPanel.setBounds(area.removeFromTop(250));
    area.removeFromTop(12);
    hint.setBounds(area.removeFromTop(52).withTrimmedLeft(13));

    auto content = meterPanel.contentArea();
    noteLabel.setBounds(content.removeFromTop(66));
    centsLabel.setBounds(content.removeFromTop(24));
    content.removeFromTop(52);   // needle, painted rather than a component
    frequencyLabel.setBounds(content.removeFromTop(20));
    content.removeFromTop(8);
    muteWhileTuning.setBounds(content.removeFromTop(28).withWidth(std::min(content.getWidth(), 340)));
}

void TunerPage::paint(juce::Graphics& graphics)
{
    const auto bounds = needleArea().toFloat();
    if (bounds.isEmpty()) return;

    const auto centre = bounds.getCentreX();
    graphics.setColour(theme::hairline);
    graphics.fillRoundedRectangle(bounds.withHeight(4.0f).withY(bounds.getCentreY() - 2.0f), 2.0f);

    // Centre mark, so "in tune" has a position to be at rather than only a colour.
    graphics.setColour(theme::textTertiary);
    graphics.fillRect(centre - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());

    if (! voiced) return;

    const auto clamped = juce::jlimit(-needleRangeCents, needleRangeCents, cents);
    const auto position = centre + (clamped / needleRangeCents) * (bounds.getWidth() * 0.5f - 6.0f);
    graphics.setColour(std::abs(cents) <= inTuneCents ? theme::good : theme::warn);
    graphics.fillRoundedRectangle(position - 3.0f, bounds.getY(), 6.0f, bounds.getHeight(), 3.0f);
}

void TunerPage::refresh()
{
    const auto reading = processor.tunerReading();
    midiNote = reading.midiNote;
    cents = reading.cents;
    voiced = reading.voiced;

    if (! voiced)
    {
        noteLabel.setText("--", juce::dontSendNotification);
        noteLabel.setColour(juce::Label::textColourId, theme::textTertiary);
        centsLabel.setText("no signal", juce::dontSendNotification);
        frequencyLabel.setText("", juce::dontSendNotification);
        repaint();
        return;
    }

    noteLabel.setText(nts::dsp::noteName(midiNote), juce::dontSendNotification);
    const auto inTune = std::abs(cents) <= inTuneCents;
    noteLabel.setColour(juce::Label::textColourId, inTune ? theme::good : theme::textPrimary);
    centsLabel.setText(inTune ? juce::String("in tune")
                              : juce::String(cents > 0.0f ? "+" : "") + juce::String(cents, 1) + " cents "
                                    + (cents > 0.0f ? "sharp" : "flat"),
                       juce::dontSendNotification);
    centsLabel.setColour(juce::Label::textColourId, inTune ? theme::good : theme::warn);
    frequencyLabel.setText(juce::String(reading.frequencyHz, 2) + " Hz", juce::dontSendNotification);
    repaint();
}
