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
    theme::glass(graphics, getLocalBounds().toFloat().reduced(0.5f), 9.0f);
    theme::caption(graphics, getLocalBounds().reduced(13, 0).removeFromTop(26), title,
                   theme::textSecondary);
}

juce::Rectangle<int> SectionPanel::contentArea() const
{
    return getLocalBounds().reduced(13, 10).withTrimmedTop(20);
}
} // namespace tf::ui
