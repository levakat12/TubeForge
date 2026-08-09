#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/// Chrome that belongs to the editor shell rather than to any one module page.
namespace tf::ui
{
/// Every icon the shell can draw. They are line drawings built from paths rather than image
/// assets so they stay crisp at any window size and pick up their colour from the state of
/// the button that holds them.
enum class Glyph
{
    amplifier,
    pedalboard,
    toneShaping,
    cabinet,
    neuralCapture,
    captures,
    toneAssistant,
    profileLibrary,
    circuit,
    toneAnalyzer,
    songMatch,
    save,
    open,
    browse,
    previous,
    next,
    settings,
    /// A circular arrow: put this back to how it started.
    reset
};

/// Builds `glyph` as a stroked path fitted to `area`, centred and square.
[[nodiscard]] juce::Path glyphPath(Glyph glyph, juce::Rectangle<float> area);

/// A borderless icon button. Used both for the module navigation across the top of the window
/// and for the small preset actions; `marksActive` is what tells the two apart, by underlining
/// the icon whose page is currently showing.
class IconButton final : public juce::Button
{
public:
    IconButton(Glyph glyphToDraw, const juce::String& name, bool marksActive = false);
    void paintButton(juce::Graphics& graphics, bool isMouseOver, bool isButtonDown) override;

private:
    Glyph glyph;
    bool underlinesActive;
};

/// The status-bar breadcrumb showing where the signal goes. The three stages the engine mode
/// actually swaps out are lit; the fixed ones stay quiet.
class StudioSignalChain final : public juce::Component
{
public:
    void setEngineMode(int mode);
    void paint(juce::Graphics& graphics) override;

private:
    int activeEngineMode {};
};

/// A thin horizontal peak bar that sits under the input or output trim.
class LevelBar final : public juce::Component
{
public:
    void setLevel(float newLevel);
    void paint(juce::Graphics& graphics) override;

private:
    float level {};
};
} // namespace tf::ui
