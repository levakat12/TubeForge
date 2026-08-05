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

    tf::ui::configureFieldCaption(referenceCaption, "Reference A");
    tf::ui::configureSlider(reference);
    reference.setTooltip("Concert A, which every other note is derived from. Leave it at 440 Hz "
                         "unless you are matching something: 432 and 435 are common alternate "
                         "tunings, and 442 to 444 is for playing along with an orchestra or a record "
                         "cut slightly fast. The scale under the needle follows it.");

    addAndMakeVisible(meterPanel);
    addAndMakeVisible(hint);
    for (auto* component : std::initializer_list<juce::Component*> {
             &noteLabel, &centsLabel, &frequencyLabel, &muteWhileTuning,
             &referenceCaption, &reference })
        meterPanel.addAndMakeVisible(*component);

    muteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "tunerMute", muteWhileTuning);
    referenceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "tunerReference", reference);
}

juce::Rectangle<int> TunerPage::needleArea() const
{
    auto content = meterPanel.contentArea();
    content.removeFromTop(96);
    // The band itself, above the scale strip paint draws beneath it.
    return content.removeFromTop(38).reduced(6, 0);
}

void TunerPage::resized()
{
    auto area = getLocalBounds();
    meterPanel.setBounds(area.removeFromTop(300));
    area.removeFromTop(12);
    hint.setBounds(area.removeFromTop(52).withTrimmedLeft(13));

    auto content = meterPanel.contentArea();
    noteLabel.setBounds(content.removeFromTop(66));
    centsLabel.setBounds(content.removeFromTop(24));
    content.removeFromTop(62);   // needle and its Hz scale, painted rather than components
    frequencyLabel.setBounds(content.removeFromTop(20));
    content.removeFromTop(6);
    tf::ui::layOutField(content.removeFromTop(40), referenceCaption, reference);
    content.removeFromTop(6);
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

    /* A scale in Hz beneath the needle, the way a clip-on tuner marks one.

       The needle's axis is cents, because that is what "how far off" means musically and it is
       constant across the neck. Hz is what the user asked to see, and the two are not the same
       axis: a cent is about 0.06 Hz at the low E and about 0.19 Hz at the open high E, so a fixed
       Hz scale would be wrong everywhere except one note. Labelling the cents axis *in* Hz for the
       note currently being played is what makes both true at once -- the ticks stay evenly spaced
       and their values follow the note.

       Drawn only when there is a note to anchor it to. An unvoiced scale would be labelling
       nothing, and guessing a note to label would be worse than leaving it blank. */
    const auto travel = bounds.getWidth() * 0.5f - 6.0f;
    const auto scaleStrip = juce::Rectangle<float> { bounds.getX(), bounds.getBottom() + 2.0f,
                                                     bounds.getWidth(), 18.0f };
    if (voiced && targetHz > 0.0f)
    {
        graphics.setFont(juce::Font { juce::FontOptions { 10.0f } });
        // Five ticks: the two rails, the two quarter marks, and the centre. Enough to read a value
        // off, few enough that the labels do not collide at this width.
        for (const auto offsetCents : { -needleRangeCents, -needleRangeCents * 0.5f, 0.0f,
                                        needleRangeCents * 0.5f, needleRangeCents })
        {
            const auto x = centre + (offsetCents / needleRangeCents) * travel;
            const auto atCentre = offsetCents == 0.0f;
            graphics.setColour(atCentre ? theme::textSecondary : theme::hairline);
            graphics.fillRect(x - 0.5f, scaleStrip.getY(), 1.0f, atCentre ? 6.0f : 4.0f);
            // Equal temperament from the note's own frequency, so the label is the pitch the
            // needle would be pointing at rather than a linear approximation of it.
            const auto hz = targetHz * std::pow(2.0f, offsetCents / 1200.0f);
            graphics.setColour(atCentre ? theme::textSecondary : theme::textTertiary);
            graphics.drawText(juce::String(hz, hz < 100.0f ? 2 : 1),
                              juce::Rectangle<float> { x - 26.0f, scaleStrip.getY() + 6.0f,
                                                       52.0f, 12.0f },
                              juce::Justification::centred, false);
        }
    }

    if (! voiced) return;

    const auto clamped = juce::jlimit(-needleRangeCents, needleRangeCents, cents);
    const auto position = centre + (clamped / needleRangeCents) * travel;
    graphics.setColour(std::abs(cents) <= inTuneCents ? theme::good : theme::warn);
    graphics.fillRoundedRectangle(position - 3.0f, bounds.getY(), 6.0f, bounds.getHeight(), 3.0f);
}

void TunerPage::refresh()
{
    const auto reading = processor.tunerReading();
    midiNote = reading.midiNote;
    cents = reading.cents;
    voiced = reading.voiced;
    targetHz = reading.targetHz;
    measuredHz = reading.frequencyHz;

    if (! voiced)
    {
        noteLabel.setText("--", juce::dontSendNotification);
        noteLabel.setColour(juce::Label::textColourId, theme::textTertiary);
        centsLabel.setText("no signal", juce::dontSendNotification);
        // The reference still shown with no note playing: it is the one setting here that changes
        // what every future reading means, so it should not appear only once something is sounding.
        frequencyLabel.setText("A = " + juce::String(reading.referenceHz, 1) + " Hz",
                               juce::dontSendNotification);
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
    // Measured against target, plus the reference both were resolved against. The target is the
    // number a clip-on tuner leaves you to infer; showing it means "how far, in Hz" is readable
    // without arithmetic, which is the whole reason to want Hz rather than cents.
    frequencyLabel.setText(juce::String(reading.frequencyHz, 2) + " Hz  ->  "
                               + juce::String(targetHz, 2) + " Hz   (A = "
                               + juce::String(reading.referenceHz, 1) + ")",
                           juce::dontSendNotification);
    repaint();
}
