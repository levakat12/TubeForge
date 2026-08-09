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
    setColour(juce::ComboBox::backgroundColourId, theme::sunken);
    setColour(juce::ComboBox::arrowColourId, theme::textSecondary);
    setColour(juce::PopupMenu::backgroundColourId, theme::panelRaised);
    setColour(juce::PopupMenu::textColourId, theme::textPrimary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, theme::accentWash);
    setColour(juce::PopupMenu::highlightedTextColourId, theme::textPrimary);
    setColour(juce::TextEditor::backgroundColourId, theme::sunken);
    setColour(juce::TextEditor::textColourId, theme::textPrimary);
    setColour(juce::TextEditor::highlightColourId, theme::accent.withAlpha(0.22f));
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::CaretComponent::caretColourId, theme::accent);
    /* The slider fill is a colour id rather than a constant so a page can recolour one control
       without a second look and feel. Auto Match is the caller that needs it: a control the
       analyzer is holding and a control the user has taken back have to look different, and the
       difference has to be on the control itself rather than in a legend somewhere. */
    setColour(juce::Slider::rotarySliderFillColourId, theme::accent);
    setColour(juce::Slider::trackColourId, theme::accent);
    setColour(juce::Slider::textBoxTextColourId, theme::textPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxHighlightColourId, theme::accent.withAlpha(0.22f));
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
    const auto fill = slider.findColour(juce::Slider::rotarySliderFillColourId);
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(2.0f);
    const auto size = std::min(bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    // The cap is most of the knob and the arc rides just outside it, so the control reads as
    // one solid disc with a thin ring rather than as a dial floating inside a gauge.
    const auto capRadius = size * 0.40f;
    const auto arcRadius = size * 0.47f;
    const auto angle = rotaryStartAngle + sliderPosition * (rotaryEndAngle - rotaryStartAngle);
    const auto stroke = juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::butt);

    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    graphics.setColour(theme::hairline);
    graphics.strokePath(track, stroke);

    if (sliderPosition > 0.004f)
    {
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
        graphics.setColour(fill);
        graphics.strokePath(value, stroke);
    }

    const auto cap = juce::Rectangle<float>(capRadius * 2.0f, capRadius * 2.0f).withCentre(centre);
    graphics.setColour(theme::panelRaised);
    graphics.fillEllipse(cap);
    graphics.setColour(theme::hairline);
    graphics.drawEllipse(cap.reduced(0.5f), 1.0f);

    // A single pointer from the middle of the cap to its rim: the whole read-out of the knob.
    const auto direction = juce::Point<float>(std::sin(angle), -std::cos(angle));
    graphics.setColour(fill.brighter(0.25f));
    graphics.drawLine({ centre + direction * (capRadius * 0.30f),
                        centre + direction * (capRadius * 0.82f) }, 1.6f);
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
    const auto fill = slider.findColour(juce::Slider::trackColourId);
    const auto centreY = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const auto track = juce::Rectangle<float>(static_cast<float>(x), centreY - 1.5f,
                                              static_cast<float>(width), 3.0f);
    graphics.setColour(theme::hairline);
    graphics.fillRoundedRectangle(track, 1.5f);

    const auto thumbX = std::clamp(sliderPosition, track.getX(), track.getRight());
    if (thumbX > track.getX() + 0.5f)
    {
        graphics.setColour(fill.withAlpha(slider.isEnabled() ? 0.85f : 0.35f));
        graphics.fillRoundedRectangle(track.withRight(thumbX), 1.5f);
    }
    graphics.setColour(theme::sunken);
    graphics.fillEllipse(juce::Rectangle<float>(13.0f, 13.0f).withCentre({ thumbX, centreY }));
    graphics.setColour(slider.isMouseOverOrDragging() ? fill.brighter(0.3f) : fill);
    graphics.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre({ thumbX, centreY }));
}

void TubeForgeLookAndFeel::drawButtonBackground(juce::Graphics& graphics, juce::Button& button,
                                                const juce::Colour&, bool isMouseOver, bool isButtonDown)
{
    // Buttons carry no chrome of their own until you touch them. A page of twelve outlined
    // boxes is what made the old editor read as a form rather than as an instrument.
    auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    const auto active = button.getToggleState();

    if (active || isMouseOver || isButtonDown)
    {
        graphics.setColour(active ? theme::accentWash
                                  : juce::Colours::white.withAlpha(isButtonDown ? 0.09f : 0.05f));
        graphics.fillRoundedRectangle(area, 5.0f);
    }
    graphics.setColour(active ? theme::accent.withAlpha(0.45f)
                              : theme::hairline.withAlpha(isMouseOver ? 1.0f : 0.65f));
    graphics.drawRoundedRectangle(area, 5.0f, 1.0f);
}

void TubeForgeLookAndFeel::drawToggleButton(juce::Graphics& graphics, juce::ToggleButton& button,
                                            bool isMouseOver, bool)
{
    const auto active = button.getToggleState();
    const auto tint = button.findColour(juce::ToggleButton::tickColourId);
    auto inside = button.getLocalBounds().reduced(2, 0);

    // A physical-looking switch reads as on/off from across the room; a tick box does not.
    auto switchArea = juce::Rectangle<float>(26.0f, 13.0f)
        .withCentre({ static_cast<float>(inside.getX()) + 13.0f, static_cast<float>(inside.getCentreY()) });
    graphics.setColour(active ? tint.withAlpha(0.22f) : theme::sunken);
    graphics.fillRoundedRectangle(switchArea, 6.5f);
    graphics.setColour(active ? tint.withAlpha(0.55f) : theme::hairline);
    graphics.drawRoundedRectangle(switchArea.reduced(0.5f), 6.5f, 1.0f);

    const auto knobX = active ? switchArea.getRight() - 6.5f : switchArea.getX() + 6.5f;
    graphics.setColour(active ? tint : (isMouseOver ? theme::textSecondary : theme::textTertiary));
    graphics.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre({ knobX, switchArea.getCentreY() }));

    graphics.setColour(active ? theme::textPrimary : theme::textSecondary);
    graphics.setFont(theme::font(9.0f, true));
    theme::tracked(graphics, button.getButtonText().toUpperCase(), inside.withTrimmedLeft(34),
                   juce::Justification::centredLeft);
}

void TubeForgeLookAndFeel::drawComboBox(juce::Graphics& graphics, int width, int height, bool isButtonDown,
                                        int buttonX, int buttonY, int buttonWidth, int buttonHeight,
                                        juce::ComboBox& box)
{
    juce::ignoreUnused(buttonX, buttonY, buttonWidth, buttonHeight);
    auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                       static_cast<float>(height)).reduced(0.5f);
    graphics.setColour(theme::sunken);
    graphics.fillRoundedRectangle(area, 5.0f);
    const auto open = isButtonDown || box.isPopupActive();
    graphics.setColour(open ? theme::accent.withAlpha(0.45f)
                            : theme::hairline.withAlpha(box.isMouseOver() ? 1.0f : 0.7f));
    graphics.drawRoundedRectangle(area, 5.0f, 1.0f);

    juce::Path arrow;
    const auto x = area.getRight() - 14.0f;
    const auto y = area.getCentreY();
    arrow.startNewSubPath(x - 3.5f, y - 1.8f);
    arrow.lineTo(x, y + 2.0f);
    arrow.lineTo(x + 3.5f, y - 1.8f);
    graphics.setColour(box.findColour(juce::ComboBox::arrowColourId));
    graphics.strokePath(arrow, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
}

void TubeForgeLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(10, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

juce::Font TubeForgeLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return theme::font(11.5f);
}

juce::Font TubeForgeLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return theme::font(std::min(11.5f, static_cast<float>(buttonHeight) * 0.46f), true);
}

void TubeForgeLookAndFeel::drawPopupMenuBackgroundWithOptions(juce::Graphics& graphics, int width, int height,
                                                              const juce::PopupMenu::Options&)
{
    auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    graphics.setColour(theme::panelRaised);
    graphics.fillRoundedRectangle(area.reduced(0.5f), 6.0f);
    graphics.setColour(theme::hairline);
    graphics.drawRoundedRectangle(area.reduced(0.5f), 6.0f, 1.0f);
}

void TubeForgeLookAndFeel::drawPopupMenuItemWithOptions(juce::Graphics& graphics, const juce::Rectangle<int>& area,
                                                        bool isHighlighted, const juce::PopupMenu::Item& item,
                                                        const juce::PopupMenu::Options&)
{
    if (item.isSeparator)
    {
        theme::rule(graphics, area.reduced(10, 0).withHeight(1).withY(area.getCentreY()));
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
        graphics.fillRoundedRectangle(juce::Rectangle<float>(2.0f, 11.0f).withCentre(
            { static_cast<float>(area.getX()) + 9.0f, static_cast<float>(area.getCentreY()) }), 1.0f);
    }
    graphics.setColour(item.isEnabled ? (isHighlighted || item.isTicked ? theme::textPrimary : theme::textSecondary)
                                      : theme::textTertiary);
    graphics.setFont(theme::font(11.5f, item.isTicked));
    graphics.drawFittedText(item.text, area.reduced(16, 0), juce::Justification::centredLeft, 1);
}

void TubeForgeLookAndFeel::fillTextEditorBackground(juce::Graphics& graphics, int width, int height,
                                                    juce::TextEditor& editor)
{
    auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                       static_cast<float>(height)).reduced(0.5f);
    theme::well(graphics, area, 5.0f);
    if (editor.hasKeyboardFocus(false))
    {
        graphics.setColour(theme::accent.withAlpha(0.45f));
        graphics.drawRoundedRectangle(area, 5.0f, 1.0f);
    }
}

void TubeForgeLookAndFeel::drawTextEditorOutline(juce::Graphics&, int, int, juce::TextEditor&) {}
