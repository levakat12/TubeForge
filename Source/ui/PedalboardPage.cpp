#include "PedalboardPage.h"
#include "CapturePicker.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>
#include <cmath>

namespace theme = tf::theme;

namespace
{
using PedalControl = TubeForgeAudioProcessor::PedalControl;

/// Fallback labels, used only by a model that names none of its own. Every built-in archetype
/// names all four, so in practice these are the shape of the row rather than its text.
constexpr std::array controlNames { "Drive", "Tone", "Level", "Mix", "Aux A", "Aux B" };
constexpr std::array controlIds { PedalControl::drive, PedalControl::tone, PedalControl::level,
                                  PedalControl::mix, PedalControl::auxA, PedalControl::auxB };
/** What the controls do. The first four mean roughly the same thing in every model, which is the
    reason there is one set of knobs rather than one per pedal -- but what that thing sounds like
    depends on the model, so each hint says which part of the box it is reaching for rather than
    promising a result.

    The last two are model-specific by definition, so their hint says only what they are and
    points at the label above them, which is where the model's own name for them appears.
*/
constexpr std::array controlHints {
    "How hard the pedal's clipping stage is driven. On a Boost this is mostly level into the "
    "amplifier; on a Fuzz it is the whole character. On a Neural capture it is the level going into "
    "the model, which a capture is genuinely sensitive to.",
    "A low-pass after the clipping. Rolls off the top of what the pedal added, which is what keeps a "
    "high-gain box from fizzing. Fully open at the top of its travel.",
    "Output trim, applied to the pedal's own signal before it is blended back. Use it to match "
    "levels so switching the pedal in does not just sound louder.",
    "How much of the pedal is blended back against the dry signal. At zero the slot is exactly "
    "transparent, whatever else it is set to.",
    "A voicing control belonging to this particular pedal -- the label above says what it is. "
    "Hidden entirely on models that do not have one.",
    "A second voicing control belonging to this particular pedal. Hidden on models without one."
};
static_assert(controlNames.size() == controlIds.size(),
              "each pedal control needs a matching display name");
static_assert(controlNames.size() == controlHints.size(),
              "each pedal control needs a hint describing what it does");

/// The page's standing explanation. Held here because `refresh` swaps it for a warning when the
/// board is heavier than the performance tier is meant to carry, and has to put it back.
constexpr auto baseHelpText =
    "Pedals run in order, left to right, into the amplifier. A slot set to Empty is not "
    "bypassed -- it is absent, and a board of four empty slots costs nothing. Click a "
    "pedal to browse the rest by what they are for; choosing a capture takes you straight "
    "to the capture library.";

/** What each model does, in one line. Shown under the controls, because knobs called Drive,
    Tone, Level and Mix say nothing on their own about what the box between them is.

    Taken from the model table, which is also what the picker shows when the pedal is being
    chosen. Two copies would be two chances to describe one pedal two different ways.
*/
juce::String hintFor(int model)
{
    return juce::String(std::string(nts::pedals::pedalModel(model).blurb));
}
} // namespace

PedalboardPage::PedalboardPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    static_assert(controlNames.size() == controlCount,
                  "the control name list and the per-slot control array must stay the same length");

    tf::ui::configureLabel(help, baseHelpText, 11.0f, false, theme::textTertiary);
    help.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(help);

    for (std::size_t slot = 0; slot < slots.size(); ++slot)
    {
        auto card = std::make_unique<SlotControls>(slot);
        auto& controls = *card;

        if (auto* kindParameter = processor.getParameters().getParameter(
                TubeForgeAudioProcessor::pedalParameterId(slot, PedalControl::kind)))
        {
            controls.kind = std::make_unique<tf::ui::GearChip>(*kindParameter, "Pedal");
            controls.kind->buildCatalogue = [this, slot] { return buildPedalCatalogue(slot); };
            controls.kind->overlayHost = this;
            // A stompbox is taller than it is wide, so its swatch has to be too.
            controls.kind->faceAspect = 0.62f;
            controls.kind->setTooltip("What this slot is. Empty takes the slot out of the chain "
                                      "entirely rather than running it at unity. Return or Space "
                                      "opens the list; Escape closes it unchanged.");
            controls.kind->onChosen = [this, slot](int chosenKind) { kindChosen(slot, chosenKind); };
        }
        controls.bypass.setTooltip("Switches the pedal out without losing its settings or its "
                                   "loaded capture. Fades rather than steps, so it is safe "
                                   "to use mid-phrase.");
        controls.loadModel.setTooltip("Load a converted capture directory, as produced by "
                                      "nts-nam-import from a .nam file.");

        for (std::size_t control = 0; control < controlCount; ++control)
        {
            tf::ui::configureLabel(controls.labels[control], controlNames[control], 11.0f, false,
                                   theme::textSecondary);
            tf::ui::configureSlider(controls.sliders[control]);
            controls.sliders[control].setTooltip(controlHints[control]);
            controls.labels[control].setTooltip(controlHints[control]);
        }
        tf::ui::configureLabel(controls.status, {}, 10.5f, false, theme::textTertiary);
        controls.status.setJustificationType(juce::Justification::topLeft);

        addAndMakeVisible(controls.panel);
        if (controls.kind != nullptr) controls.panel.addAndMakeVisible(*controls.kind);
        for (auto* component : std::initializer_list<juce::Component*> {
                 &controls.bypass, &controls.loadModel, &controls.status })
            controls.panel.addAndMakeVisible(*component);
        for (std::size_t control = 0; control < controlCount; ++control)
        {
            controls.panel.addAndMakeVisible(controls.labels[control]);
            controls.panel.addAndMakeVisible(controls.sliders[control]);
        }

        controls.bypassAttachment =
            std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
                processor.getParameters(),
                TubeForgeAudioProcessor::pedalParameterId(slot, PedalControl::bypass),
                controls.bypass);
        for (std::size_t control = 0; control < controlCount; ++control)
            controls.sliderAttachments[control] =
                std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                    processor.getParameters(),
                    TubeForgeAudioProcessor::pedalParameterId(slot, controlIds[control]),
                    controls.sliders[control]);

        controls.loadModel.onClick = [this, slot] { chooseModel(slot); };

        slots[slot] = std::move(card);
    }
}

void PedalboardPage::resized()
{
    auto area = getLocalBounds();
    help.setBounds(area.removeFromTop(46));
    area.removeFromTop(8);

    // One column per slot, in signal order. The chain reads left to right, which is the only
    // arrangement that matches both the footer breadcrumb and a real board on a floor.
    const auto columnWidth = area.getWidth() / static_cast<int>(slots.size());
    for (std::size_t slot = 0; slot < slots.size(); ++slot)
    {
        slots[slot]->panel.setBounds(
            juce::Rectangle<int>(area.getX() + static_cast<int>(slot) * columnWidth,
                                 area.getY(), columnWidth, area.getHeight())
                .reduced(5));
        layOutSlot(slot);
    }
}

void PedalboardPage::layOutSlot(std::size_t slot)
{
    auto& controls = *slots[slot];
    auto content = controls.panel.contentArea();
    if (content.isEmpty()) return;

    // Taller than the combo it replaces: the swatch has to be big enough for a pedal to be
    // recognised by its colour and its knob count before its name is read.
    if (controls.kind != nullptr) controls.kind->setBounds(content.removeFromTop(42));
    content.removeFromTop(8);
    controls.bypass.setBounds(content.removeFromTop(26));
    content.removeFromTop(6);
    // Only the visible controls take a row, so a two-knob pedal does not leave four gaps.
    for (std::size_t control = 0; control < controlCount; ++control)
    {
        if (! controls.sliders[control].isVisible()) continue;
        auto row = content.removeFromTop(26);
        controls.labels[control].setBounds(row.removeFromLeft(58));
        controls.sliders[control].setBounds(row);
    }
    content.removeFromTop(8);
    controls.loadModel.setBounds(content.removeFromTop(26).withWidth(std::min(content.getWidth(), 140)));
    content.removeFromTop(6);
    controls.status.setBounds(content);
}

tf::ui::GearCatalogue PedalboardPage::buildPedalCatalogue(std::size_t slot)
{
    tf::ui::GearCatalogue catalogue;
    catalogue.heading = "Pedal " + juce::String(static_cast<int>(slot) + 1)
                      + "   /   choose what goes in this slot";
    // Sized to a stompbox rather than to a faceplate: roughly 2:3, so a shelf of them packs
    // across the panel instead of sitting in the middle of three very wide tiles.
    catalogue.tileWidth = 132;
    catalogue.tileFaceHeight = 168;
    for (std::size_t index = 0; index < tf::ui::pedalCharacterCount; ++index)
        catalogue.categories.push_back(
            tf::ui::pedalCharacterName(static_cast<tf::ui::PedalCharacter>(index)));

    // Driven from the engine's model table, which is what the choice parameter is built from
    // too, so a pedal appended there appears here without anyone remembering to come back.
    for (std::size_t index = 0; index < nts::pedals::modelCount(); ++index)
    {
        const auto kind = static_cast<int>(index);
        const auto& face = tf::ui::pedalFace(kind);
        const auto& model = nts::pedals::pedalModel(kind);
        // Empty is pinned above the shelves rather than filed under one. It is the default and
        // the way a slot is cleared, and burying the most-used entry a category deep would be a
        // regression over the combo box this replaces.
        const auto category = face.pinned ? -1 : static_cast<int>(face.character);
        // Name and blurb come from the model, not the face: the engine owns what a pedal is,
        // the face owns only what it looks like.
        catalogue.tiles.push_back(
            { kind, juce::String(std::string(model.name)), juce::String(std::string(model.blurb)),
              category,
              [this, kind](juce::Graphics& graphics, juce::Rectangle<float> area)
              { thumbnails.paint(graphics, area, kind); } });
    }
    return catalogue;
}

void PedalboardPage::kindChosen(std::size_t slot, int chosenKind)
{
    refresh();
    if (nts::pedals::pedalModel(chosenKind).engine != nts::pedals::PedalEngine::neural) return;

    // A capture slot with no model in it does nothing, and today that is only recoverable by
    // noticing a status line. Chaining straight into the capture picker removes the dead end.
    //
    // Delayed rather than immediate: the gear panel is still collapsing when this runs, and two
    // overlays over one page at once is a mess that also breaks the fold-back animation.
    juce::Timer::callAfterDelay(220,
        [safeThis = juce::Component::SafePointer<PedalboardPage>(this), slot]
        {
            if (safeThis == nullptr) return;
            safeThis->chooseModel(slot);
        });
}

void PedalboardPage::refresh()
{
    /* The cost line. Advisory and nothing else: a user who deliberately puts four expensive
       pedals in front of the amplifier is told the performance tier is not meant to carry it,
       and then gets exactly what they asked for. Quietly degrading something somebody chose is
       the worse failure, and it is the one a load-watching heuristic makes on its own. */
    const auto cost = processor.pedalboardCost();
    const auto budget = processor.tierLimits().pedalCostBudget;
    const auto over = budget > 0 && cost > budget;
    help.setText(over
        ? "This board is heavier than the current performance tier is meant to carry, so the "
          "audio callback may not keep up. Nothing has been changed -- raise the tier, bypass a "
          "slot, or ignore this if it plays cleanly."
        : baseHelpText,
        juce::dontSendNotification);
    help.setColour(juce::Label::textColourId, over ? theme::warn : theme::textTertiary);

    for (std::size_t slot = 0; slot < slots.size(); ++slot)
    {
        auto& controls = *slots[slot];
        const auto model = modelOf(slot);
        if (controls.kind != nullptr) controls.kind->refresh();
        const auto neural = nts::pedals::pedalModel(model).engine == nts::pedals::PedalEngine::neural;

        // Only a neural slot has a model to load, and only it has a status worth reporting;
        // for the rest the same line carries what the pedal actually does.
        controls.loadModel.setVisible(neural);
        controls.status.setText(neural ? processor.pedalModelStatusText(static_cast<int>(slot))
                                       : hintFor(model),
                                juce::dontSendNotification);

        /* Each model names the controls it has, and hides the ones it does not.

           A row of six knobs where two do nothing is worse than four: a control that is
           present and inert reads as broken rather than as absent. The empty slot is the one
           case where every control stays visible but dimmed -- there the whole slot is unused,
           which is a different thing from a knob this particular pedal never had. */
        const auto engaged = model != 0;
        const auto& names = nts::pedals::pedalModel(model).controls;
        for (std::size_t control = 0; control < controlCount; ++control)
        {
            const auto named = ! names[control].empty();
            const auto shown = engaged ? named : control < 4;
            controls.labels[control].setVisible(shown);
            controls.sliders[control].setVisible(shown);
            if (! shown) continue;
            controls.labels[control].setText(
                named ? juce::String(std::string(names[control])) : juce::String(controlNames[control]),
                juce::dontSendNotification);
            controls.sliders[control].setAlpha(engaged ? 1.0f : 0.45f);
            controls.labels[control].setAlpha(engaged ? 1.0f : 0.45f);
        }
        controls.bypass.setAlpha(engaged ? 1.0f : 0.45f);
        // The visible set just changed, so the rows have to be re-flowed rather than left with
        // gaps where the hidden ones were.
        layOutSlot(slot);
    }
}

int PedalboardPage::modelOf(std::size_t slot) const
{
    const auto* value = processor.getParameters().getRawParameterValue(
        TubeForgeAudioProcessor::pedalParameterId(slot, PedalControl::kind));
    if (value == nullptr) return 0;
    return std::clamp(static_cast<int>(std::lround(value->load(std::memory_order_relaxed))),
                      0, static_cast<int>(nts::pedals::modelCount()) - 1);
}

void PedalboardPage::chooseModel(std::size_t slot)
{
    // The picker lists what has been imported and can import more, so a `.zip` straight from a
    // capture pack reaches a pedal slot without leaving this page.
    CapturePicker::openOver(*this, processor, nts::nam::GearKind::pedal,
                            "PEDAL " + juce::String(static_cast<int>(slot) + 1) + "   /   CHOOSE A CAPTURE",
                            [safeThis = juce::Component::SafePointer<PedalboardPage>(this),
                             slot](juce::File artifact)
                            {
                                if (safeThis == nullptr) return;
                                safeThis->processor.requestPedalModelLoad(static_cast<int>(slot), artifact);
                            });
}
