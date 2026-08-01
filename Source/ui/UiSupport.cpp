#include "UiSupport.h"
#include "../TubeForgeTheme.h"

namespace theme = tf::theme;

namespace tf::ui
{
void configureLabel(juce::Label& label, const juce::String& text, float height, bool bold,
                    juce::Colour colour)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(theme::font(height, bold));
    label.setColour(juce::Label::textColourId, colour);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setInterceptsMouseClicks(false, false);
}

void configureFieldCaption(juce::Label& label, const juce::String& text)
{
    configureLabel(label, text.toUpperCase(), 8.5f, true, theme::textTertiary);
}

void configureSlider(juce::Slider& slider)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    slider.setPopupDisplayEnabled(false, false, nullptr);
    slider.setTooltip("Drag to adjust; double-click the value to type an exact setting");
}

void configureKnob(juce::Slider& slider, bool large)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                               juce::MathConstants<float>::pi * 2.75f, true);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, large ? 72 : 62, large ? 19 : 17);
    slider.setPopupDisplayEnabled(true, false, nullptr);
    slider.setTooltip("Drag up/down or left/right; double-click the value to type an exact setting");
}

void layOutField(juce::Rectangle<int> cell, juce::Label& caption, juce::Component& control)
{
    caption.setBounds(cell.removeFromTop(13));
    control.setBounds(cell.removeFromTop(28));
}

void populateFromParameter(juce::ComboBox& box, juce::AudioProcessorValueTreeState& state,
                           const juce::String& parameterId)
{
    if (const auto* choice = dynamic_cast<const juce::AudioParameterChoice*>(
            state.getParameter(parameterId)))
        box.addItemList(choice->choices, 1);
}

void SectionPanel::paint(juce::Graphics& graphics)
{
    theme::glass(graphics, getLocalBounds().toFloat().reduced(0.5f), 7.0f);
    auto heading = getLocalBounds().reduced(14, 0).removeFromTop(28);
    theme::caption(graphics, heading, title, theme::textSecondary);
    // A rule under the heading rather than a second panel around the controls: it separates
    // the title from its contents without adding another edge to the window.
    theme::rule(graphics, heading.withHeight(1).withY(heading.getBottom() - 2), 0.8f);
}

juce::Rectangle<int> SectionPanel::contentArea() const
{
    return getLocalBounds().reduced(14, 10).withTrimmedTop(22);
}
} // namespace tf::ui
