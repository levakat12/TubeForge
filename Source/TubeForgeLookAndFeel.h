#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/// The editor's single look-and-feel. Every control in every page is drawn through this, so
/// a page cannot accidentally style itself differently from its neighbours.
class TubeForgeLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    TubeForgeLookAndFeel();
    void drawRotarySlider(juce::Graphics& graphics, int x, int y, int width, int height,
                          float sliderPosition, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider& slider) override;
    void drawLinearSlider(juce::Graphics& graphics, int x, int y, int width, int height,
                          float sliderPosition, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool isMouseOver, bool isButtonDown) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool isMouseOver,
                          bool isButtonDown) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonWidth, int buttonHeight,
                      juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawPopupMenuBackgroundWithOptions(juce::Graphics&, int width, int height,
                                            const juce::PopupMenu::Options&) override;
    void drawPopupMenuItemWithOptions(juce::Graphics&, const juce::Rectangle<int>& area,
                                      bool isHighlighted, const juce::PopupMenu::Item&,
                                      const juce::PopupMenu::Options&) override;
    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;
};
