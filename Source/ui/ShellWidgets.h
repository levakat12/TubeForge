#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

/// Chrome that belongs to the editor shell rather than to any one module page.
namespace tf::ui
{
/// One entry in the left gear browser: name, one-line description, and an accent bar that
/// lights up when its page is showing.
class GearBrowserItem final : public juce::Button
{
public:
    GearBrowserItem(const juce::String& name, juce::String description);
    void paintButton(juce::Graphics& graphics, bool isMouseOver, bool isButtonDown) override;
private:
    juce::String tag;
};

/// The always-visible strip along the bottom showing where the signal goes. It highlights the
/// three stages the engine mode actually swaps out.
class StudioSignalChain final : public juce::Component
{
public:
    void setEngineMode(int mode);
    void paint(juce::Graphics& graphics) override;
private:
    int activeEngineMode {};
};

/// Input and output peak meters for the right-hand rail.
class StudioLevelMeter final : public juce::Component
{
public:
    void setLevels(float input, float output);
    void paint(juce::Graphics& graphics) override;
private:
    float inputLevel {};
    float outputLevel {};
};
} // namespace tf::ui
