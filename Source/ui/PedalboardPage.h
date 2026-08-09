#pragma once

#include "GearPicker.h"
#include "ModulePage.h"
#include "PedalArt.h"
#include "UiSupport.h"

#include <nts/pedals/PedalBoard.h>

#include <array>
#include <memory>

/// The pedals in front of the amplifier.
///
/// Four slots in signal order, left to right, each either empty, one of the built-in digital
/// pedals, or a Neural Amp Modeler capture loaded from disk. Every slot defaults to None,
/// which is not a neutral effect but no effect at all -- the page's default state is a rig of
/// amplifier and cabinet, and leaving it alone is a supported way to use it.
class PedalboardPage final : public ModulePage
{
public:
    explicit PedalboardPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;

private:
    /// Drive, tone, level, mix, and two model-specific voicing controls. A model names the ones
    /// it has; the rest are hidden rather than shown doing nothing.
    static constexpr std::size_t controlCount = nts::pedals::controlCount;

    /// One slot's card. Held by pointer because the panel it is built around needs its
    /// heading at construction, and the heading is the slot number.
    struct SlotControls
    {
        explicit SlotControls(std::size_t index)
            : panel("Pedal " + juce::String(static_cast<int>(index) + 1))
        {
        }

        tf::ui::SectionPanel panel;
        /// Held by pointer because it needs its parameter at construction, which the page has
        /// and this struct does not.
        std::unique_ptr<tf::ui::GearChip> kind;
        juce::ToggleButton bypass { "Bypass" };
        std::array<juce::Label, controlCount> labels;
        std::array<juce::Slider, controlCount> sliders;
        juce::TextButton loadModel { "Load capture" };
        juce::Label status;

        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
        std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,
                   controlCount> sliderAttachments;
    };

    void chooseModel(std::size_t slot);
    /// Lays out one card. Split out of `resized` because the visible control set changes with
    /// the model, so the rows have to re-flow on a refresh and not only on a resize.
    void layOutSlot(std::size_t slot);
    /// The slot's model index, read back from the parameter that owns it.
    [[nodiscard]] int modelOf(std::size_t slot) const;
    /// The kinds as tiles, shelved by what each is for. Shared across all four slots -- the
    /// list is the same everywhere, only the parameter it writes differs.
    [[nodiscard]] tf::ui::GearCatalogue buildPedalCatalogue(std::size_t slot);
    /// Runs after a kind has been written, for the work that follows from the choice rather
    /// than being it -- currently, sending a fresh capture slot straight to the capture picker.
    void kindChosen(std::size_t slot, int chosenKind);

    juce::Label help;
    /// Shared by every slot's chip and by the picker's tiles, so the seven faces are rendered
    /// once for the whole page rather than once per slot.
    tf::ui::PedalThumbnails thumbnails;
    std::array<std::unique_ptr<SlotControls>, nts::pedals::slotCount> slots;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalboardPage)
};
