#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

/// Small pieces every module page builds from. Kept here rather than duplicated per page so
/// a caption on the Circuit page can never drift out of step with one on the Amp page.
namespace tf::ui
{
void configureLabel(juce::Label& label, const juce::String& text, float height, bool bold,
                    juce::Colour colour);

/// The standard field caption: tracked small capitals above the control it names.
void configureFieldCaption(juce::Label& label, const juce::String& text);

/// A horizontal parameter slider with its value on the right.
void configureSlider(juce::Slider& slider);

/// A rotary knob with its value underneath. `large` is for the amp faceplate row; the
/// smaller variant is used for the shell's input and output trims.
void configureKnob(juce::Slider& slider, bool large);

/// Lays a caption above its control inside one cell, so every labelled field in the editor
/// lines up on the same two baselines.
void layOutField(juce::Rectangle<int> cell, juce::Label& caption, juce::Component& control);

/// Fills a box from its own choice parameter. A hand-written copy of the choices drifts the
/// moment the parameter gains an entry, which is how the "Auto" oversampling mode once went
/// missing from the UI.
void populateFromParameter(juce::ComboBox& box, juce::AudioProcessorValueTreeState& state,
                           const juce::String& parameterId);

/// A titled glass card. Controls are laid out inside contentArea() so a page never has to
/// know how tall the heading is.
class SectionPanel final : public juce::Component
{
public:
    explicit SectionPanel(juce::String heading) : title(std::move(heading)) {}
    void paint(juce::Graphics& graphics) override;
    [[nodiscard]] juce::Rectangle<int> contentArea() const;
private:
    juce::String title;
};
} // namespace tf::ui
