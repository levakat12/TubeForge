#pragma once

#include "FaceplateArt.h"
#include "GearPicker.h"
#include "ModulePage.h"
#include "UiSupport.h"

#include <array>
#include <memory>
#include <vector>

/// The amplifier faceplate: the seven controls a player actually reaches for, drawn on the
/// amp art. Owns the art, the knob layout and the group dividers together, so the dividers
/// can never drift away from the knobs they separate.
class AmplifierPage final : public ModulePage
{
public:
    explicit AmplifierPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void paint(juce::Graphics& graphics) override;
    void refresh() override;

private:
    [[nodiscard]] juce::Rectangle<int> faceplateArea() const;
    [[nodiscard]] int choiceIndex(const juce::String& parameterId) const;
    /** Repaints the faceplate in the current voicing's livery, and recolours the lettering
        that sits on it.

        The captions and value read-outs belong to the art, not to the theme: they are printed
        on the control panel, and a panel is free to be warm brown or cold gunmetal. Pulling
        their colour from the style is what lets the art change without anything on it
        becoming unreadable.
    */
    void applyFaceplateStyle();
    /// The voicings, as tiles: one per topology, shelved by character, faces served from the
    /// thumbnail cache so a grid of them costs one render each rather than one per frame.
    [[nodiscard]] tf::ui::GearCatalogue buildVoicingCatalogue();

    static constexpr int chipRowHeight = 41;
    /// Column counts of the four control groups: drive, tone stack, voicing, output.
    static constexpr std::array groupSizes { 1, 3, 2, 1 };

    tf::ui::FaceplateArt faceplate;
    /// Shared by the chip and the picker's tiles, so opening the picker renders each voicing
    /// once and the chip's own face is already in the cache.
    tf::ui::FaceplateThumbnails thumbnails;
    /// What the art was last built for. Negative until the first `applyFaceplateStyle`.
    int paintedTopology { -1 };
    int paintedInstrument { -1 };
    juce::Rectangle<int> chipBounds;
    std::array<int, groupSizes.size() - 1> dividerX {};

    juce::Label instrumentCaption;
    juce::ComboBox instrumentSelector;
    juce::Label voicingCaption;
    std::unique_ptr<tf::ui::GearChip> voicing;
    juce::ToggleButton cabinetEnabled { "Cabinet" };
    std::array<juce::Label, 7> knobLabels;
    std::array<juce::Slider, 7> knobs;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> instrumentAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabinetAttachment;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> knobAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AmplifierPage)
};
