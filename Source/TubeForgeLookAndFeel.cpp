#include "TubeForgeLookAndFeel.h"
#include "TubeForgeTheme.h"

#include <algorithm>
#include <cmath>

namespace theme = tf::theme;

TubeForgeLookAndFeel::TubeForgeLookAndFeel()
{
    setColour(juce::ResizableWindow::backgroundColourId, theme::backdropBottom);
    setColour(juce::Label::textColourId, theme::textPrimary);
    setColour(juce::TextButton::textColourOffId, theme::textSecondary);
    setColour(juce::TextButton::textColourOnId, theme::textPrimary);
    setColour(juce::ToggleButton::tickColourId, theme::accent);
    setColour(juce::ToggleButton::textColourId, theme::textSecondary);
    setColour(juce::ComboBox::textColourId, theme::textPrimary);
    setColour(juce::ComboBox::backgroundColourId, theme::panel);
    setColour(juce::ComboBox::arrowColourId, theme::textSecondary);
    setColour(juce::PopupMenu::backgroundColourId, theme::panelRaised);
    setColour(juce::PopupMenu::textColourId, theme::textPrimary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, theme::accentWash);
    setColour(juce::PopupMenu::highlightedTextColourId, theme::textPrimary);
    setColour(juce::TextEditor::backgroundColourId, theme::sunken);
    setColour(juce::TextEditor::textColourId, theme::textPrimary);
    setColour(juce::TextEditor::highlightColourId, theme::accent.withAlpha(0.30f));
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::CaretComponent::caretColourId, theme::accent);
    setColour(juce::Slider::textBoxTextColourId, theme::textPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxHighlightColourId, theme::accent.withAlpha(0.30f));
    setColour(juce::TooltipWindow::backgroundColourId, theme::panelRaised);
    setColour(juce::TooltipWindow::textColourId, theme::textPrimary);
    setColour(juce::TooltipWindow::outlineColourId, theme::hairline);
    setColour(juce::AlertWindow::backgroundColourId, theme::panel);
    setColour(juce::AlertWindow::textColourId, theme::textPrimary);
    setColour(juce::AlertWindow::outlineColourId, theme::hairline);
}

void TubeForgeLookAndFeel::drawRotarySlider(juce::Graphics& graphics, int x, int y, int width, int height,
                                            float sliderPosition, float rotaryStartAngle,
                                            float rotaryEndAngle, juce::Slider& slider)
{
    juce::ignoreUnused(slider);
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(3.0f);
    const auto size = std::min(bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const auto arcRadius = size * 0.46f;
    const auto capRadius = size * 0.33f;
    const auto angle = rotaryStartAngle + sliderPosition * (rotaryEndAngle - rotaryStartAngle);
    const auto stroke = juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    graphics.setColour(juce::Colours::black.withAlpha(0.55f));
    graphics.strokePath(track, stroke);

    if (sliderPosition > 0.002f)
    {
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
        graphics.setColour(theme::accent.withAlpha(0.20f));
        graphics.strokePath(value, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
        graphics.setColour(theme::accent);
        graphics.strokePath(value, stroke);
    }

    const auto cap = juce::Rectangle<float>(capRadius * 2.0f, capRadius * 2.0f).withCentre(centre);
    graphics.setColour(juce::Colours::black.withAlpha(0.50f));
    graphics.fillEllipse(cap.translated(0.0f, 2.5f).expanded(1.5f));
    graphics.setGradientFill(juce::ColourGradient(juce::Colour(0xff3c424c), cap.getCentreX(), cap.getY(),
                                                  juce::Colour(0xff12151a), cap.getCentreX(), cap.getBottom(), false));
    graphics.fillEllipse(cap);
    graphics.setColour(juce::Colours::white.withAlpha(0.11f));
    graphics.drawEllipse(cap.reduced(0.5f), 1.0f);
    // A soft highlight across the top of the cap; without it the knob reads as a flat disc.
    graphics.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.13f), cap.getCentreX(),
                                                  cap.getY(), juce::Colours::transparentWhite, cap.getCentreX(),
                                                  cap.getCentreY(), false));
    graphics.fillEllipse(cap.reduced(capRadius * 0.20f).withTrimmedBottom(capRadius * 0.7f));

    const auto direction = juce::Point<float>(std::sin(angle), -std::cos(angle));
    graphics.setColour(theme::accent.brighter(0.45f));
    graphics.drawLine({ centre + direction * (capRadius * 0.32f),
                        centre + direction * (capRadius * 0.86f) }, 2.4f);
}

void TubeForgeLookAndFeel::drawLinearSlider(juce::Graphics& graphics, int x, int y, int width, int height,
                                            float sliderPosition, float minSliderPos, float maxSliderPos,
                                            juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        juce::LookAndFeel_V4::drawLinearSlider(graphics, x, y, width, height, sliderPosition,
                                               minSliderPos, maxSliderPos, style, slider);
        return;
    }
    const auto centreY = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const auto track = juce::Rectangle<float>(static_cast<float>(x), centreY - 2.5f,
                                              static_cast<float>(width), 5.0f);
    theme::well(graphics, track, 2.5f);
    const auto thumbX = std::clamp(sliderPosition, track.getX(), track.getRight());
    if (thumbX > track.getX() + 1.0f)
    {
        graphics.setColour(theme::accent);
        graphics.fillRoundedRectangle(track.reduced(1.0f).withRight(thumbX), 2.0f);
    }
    graphics.setColour(theme::accent.withAlpha(0.22f));
    graphics.fillEllipse(juce::Rectangle<float>(20.0f, 20.0f).withCentre({ thumbX, centreY }));
    graphics.setColour(juce::Colours::black.withAlpha(0.5f));
    graphics.fillEllipse(juce::Rectangle<float>(13.0f, 13.0f).withCentre({ thumbX, centreY + 1.5f }));
    graphics.setGradientFill(juce::ColourGradient(juce::Colour(0xffe9edf3), thumbX, centreY - 6.0f,
                                                  juce::Colour(0xff9aa2ae), thumbX, centreY + 6.0f, false));
    graphics.fillEllipse(juce::Rectangle<float>(12.0f, 12.0f).withCentre({ thumbX, centreY }));
}

void TubeForgeLookAndFeel::drawButtonBackground(juce::Graphics& graphics, juce::Button& button,
                                                const juce::Colour&, bool isMouseOver, bool isButtonDown)
{
    auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    theme::glass(graphics, area, 6.0f, isMouseOver);
    if (isButtonDown)
    {
        graphics.setColour(juce::Colours::black.withAlpha(0.25f));
        graphics.fillRoundedRectangle(area, 6.0f);
    }
    if (button.getToggleState())
    {
        graphics.setColour(theme::accentWash);
        graphics.fillRoundedRectangle(area, 6.0f);
    }
    graphics.setColour(button.getToggleState() ? theme::accent.withAlpha(0.65f)
                       : isMouseOver ? theme::accent.withAlpha(0.35f)
                                     : juce::Colours::transparentBlack);
    graphics.drawRoundedRectangle(area, 6.0f, 1.0f);
}

void TubeForgeLookAndFeel::drawToggleButton(juce::Graphics& graphics, juce::ToggleButton& button,
                                            bool isMouseOver, bool)
{
    auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    const auto active = button.getToggleState();
    const auto tint = button.findColour(juce::ToggleButton::tickColourId);
    theme::glass(graphics, area, 6.0f, isMouseOver);
    if (active)
    {
        graphics.setColour(tint.withAlpha(0.14f));
        graphics.fillRoundedRectangle(area, 6.0f);
    }
    graphics.setColour(active ? tint.withAlpha(0.60f) : juce::Colours::white.withAlpha(0.05f));
    graphics.drawRoundedRectangle(area, 6.0f, 1.0f);

    // A physical-looking switch reads as on/off from across the room; a tick box does not.
    auto inside = button.getLocalBounds().reduced(8, 0);
    auto switchArea = juce::Rectangle<float>(22.0f, 12.0f)
        .withCentre({ static_cast<float>(inside.getX()) + 11.0f, static_cast<float>(inside.getCentreY()) });
    theme::well(graphics, switchArea, 6.0f);
    if (active)
    {
        graphics.setColour(tint.withAlpha(0.35f));
        graphics.fillRoundedRectangle(switchArea, 6.0f);
    }
    const auto knobX = active ? switchArea.getRight() - 6.0f : switchArea.getX() + 6.0f;
    graphics.setColour(active ? tint : theme::textTertiary);
    graphics.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre({ knobX, switchArea.getCentreY() }));

    graphics.setColour(active ? theme::textPrimary : theme::textSecondary);
    graphics.setFont(theme::font(10.5f, active));
    graphics.drawFittedText(button.getButtonText(), inside.withTrimmedLeft(30), juce::Justification::centredLeft, 2);
}

void TubeForgeLookAndFeel::drawComboBox(juce::Graphics& graphics, int width, int height, bool isButtonDown,
                                        int buttonX, int buttonY, int buttonWidth, int buttonHeight,
                                        juce::ComboBox& box)
{
    juce::ignoreUnused(buttonX, buttonY, buttonWidth, buttonHeight);
    auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                       static_cast<float>(height)).reduced(0.5f);
    theme::glass(graphics, area, 6.0f, box.isMouseOver());
    if (isButtonDown || box.isPopupActive())
    {
        graphics.setColour(theme::accent.withAlpha(0.45f));
        graphics.drawRoundedRectangle(area, 6.0f, 1.0f);
    }
    juce::Path arrow;
    const auto x = area.getRight() - 15.0f;
    const auto y = area.getCentreY();
    arrow.startNewSubPath(x - 4.0f, y - 2.0f);
    arrow.lineTo(x, y + 2.5f);
    arrow.lineTo(x + 4.0f, y - 2.0f);
    graphics.setColour(box.findColour(juce::ComboBox::arrowColourId));
    graphics.strokePath(arrow, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
}

void TubeForgeLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(11, 1, box.getWidth() - 32, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

juce::Font TubeForgeLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return theme::font(12.0f);
}

juce::Font TubeForgeLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return theme::font(std::min(12.0f, static_cast<float>(buttonHeight) * 0.48f), true);
}

void TubeForgeLookAndFeel::drawPopupMenuBackgroundWithOptions(juce::Graphics& graphics, int width, int height,
                                                              const juce::PopupMenu::Options&)
{
    auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    graphics.setColour(theme::panelRaised);
    graphics.fillRoundedRectangle(area.reduced(0.5f), 7.0f);
    graphics.setColour(juce::Colours::white.withAlpha(0.08f));
    graphics.drawRoundedRectangle(area.reduced(0.5f), 7.0f, 1.0f);
}

void TubeForgeLookAndFeel::drawPopupMenuItemWithOptions(juce::Graphics& graphics, const juce::Rectangle<int>& area,
                                                        bool isHighlighted, const juce::PopupMenu::Item& item,
                                                        const juce::PopupMenu::Options&)
{
    if (item.isSeparator)
    {
        graphics.setColour(juce::Colours::white.withAlpha(0.07f));
        graphics.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }
    if (isHighlighted && item.isEnabled)
    {
        graphics.setColour(theme::accentWash);
        graphics.fillRoundedRectangle(area.reduced(4, 1).toFloat(), 4.0f);
    }
    if (item.isTicked)
    {
        graphics.setColour(theme::accent);
        graphics.fillRoundedRectangle(juce::Rectangle<float>(3.0f, 12.0f).withCentre(
            { static_cast<float>(area.getX()) + 8.0f, static_cast<float>(area.getCentreY()) }), 1.5f);
    }
    graphics.setColour(item.isEnabled ? (isHighlighted || item.isTicked ? theme::textPrimary : theme::textSecondary)
                                      : theme::textTertiary);
    graphics.setFont(theme::font(12.0f, item.isTicked));
    graphics.drawFittedText(item.text, area.reduced(16, 0), juce::Justification::centredLeft, 1);
}

void TubeForgeLookAndFeel::fillTextEditorBackground(juce::Graphics& graphics, int width, int height,
                                                    juce::TextEditor& editor)
{
    theme::well(graphics, juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                                 static_cast<float>(height)).reduced(0.5f), 6.0f);
    if (editor.hasKeyboardFocus(false))
    {
        graphics.setColour(theme::accent.withAlpha(0.45f));
        graphics.drawRoundedRectangle(juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                                             static_cast<float>(height)).reduced(0.5f), 6.0f, 1.0f);
    }
}

void TubeForgeLookAndFeel::drawTextEditorOutline(juce::Graphics&, int, int, juce::TextEditor&) {}
