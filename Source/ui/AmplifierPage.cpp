#include "AmplifierPage.h"
#include "../PluginProcessor.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr std::array knobIds { "gain", "bass", "mid", "treble", "presence", "resonance", "master" };
constexpr std::array knobNames { "Gain", "Bass", "Mid", "Treble", "Presence", "Resonance", "Master" };
/// What each knob does, in the order above. Says what turning it changes, not what it is called.
constexpr std::array knobHints {
    "How hard the preamp is driven. The main control over how clean or distorted the amp is, and the "
    "first thing to set.",
    "Low end, in the tone stack after the distortion. Cuts and boosts what is already there rather "
    "than changing how the amp breaks up.",
    "Midrange. Most of a guitar's body and cut lives here -- scooping it sounds heavy alone and "
    "vanishes in a mix; pushing it cuts through.",
    "Top end of the tone stack. Adds definition and pick attack; too much on a high-gain setting "
    "turns into fizz.",
    "A high shelf around 2.6 kHz in the power section, after the tone stack. Presence bites where "
    "Treble brightens -- reach for it when the amp sounds dull but not dark.",
    "Low-end resonance in the power section, the interaction between the output stage and the "
    "speaker. Adds thump and looseness down low.",
    "Power-amp level. On a real amp this is where power-stage saturation comes from, so it changes "
    "the tone as well as the volume, not just how loud it is."
};
static_assert(knobIds.size() == knobNames.size(), "each amp knob needs a matching display name");
static_assert(knobIds.size() == knobHints.size(), "each amp knob needs a hint describing what it does");
} // namespace

AmplifierPage::AmplifierPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    tf::ui::configureFieldCaption(instrumentCaption, "Instrument");
    tf::ui::populateFromParameter(instrumentSelector, processor.getParameters(), "instrument");
    tf::ui::configureFieldCaption(voicingCaption, "Voicing");
    tf::ui::configureFieldCaption(panelSwitchCaption, "Panel switch");
    addAndMakeVisible(panelSwitchCaption);
    addAndMakeVisible(panelSwitchSelector);
    addAndMakeVisible(instrumentCaption);
    addAndMakeVisible(instrumentSelector);
    addAndMakeVisible(voicingCaption);
    addAndMakeVisible(cabinetEnabled);

    if (auto* topology = processor.getParameters().getParameter("topology"))
    {
        voicing = std::make_unique<tf::ui::GearChip>(*topology, "Voicing");
        voicing->buildCatalogue = [this] { return buildVoicingCatalogue(); };
        // Opened over the page rather than the whole editor: the shell's own chrome -- preset
        // rail, input and output trims -- stays reachable, and the panel folds back into a chip
        // that is in the same coordinate space it grew from.
        voicing->overlayHost = this;
        addAndMakeVisible(*voicing);
    }

    for (std::size_t index = 0; index < knobs.size(); ++index)
    {
        // The colour here is a placeholder: applyFaceplateStyle repaints the lettering in
        // whatever the current voicing's control panel calls for before the page is shown.
        tf::ui::configureLabel(knobLabels[index], juce::String(knobNames[index]).toUpperCase(),
                               10.0f, true, juce::Colours::white);
        knobLabels[index].setJustificationType(juce::Justification::centred);
        tf::ui::configureKnob(knobs[index], true);
        knobs[index].setTooltip(knobHints[index]);
        knobLabels[index].setTooltip(knobHints[index]);
        addAndMakeVisible(knobLabels[index]);
        addAndMakeVisible(knobs[index]);
    }

    instrumentAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "instrument", instrumentSelector);
    cabinetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "cabinet", cabinetEnabled);
    for (std::size_t index = 0; index < knobs.size(); ++index)
        knobAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getParameters(), knobIds[index], knobs[index]));

    rebuildPanelSwitches();
    panelSwitchSelector.onChange = [this]
    {
        const auto selected = panelSwitchSelector.getSelectedId();
        if (selected <= 0) return;
        if (auto* parameter = processor.getParameters().getParameter("panelSwitch"))
        {
            const auto normalised = parameter->convertTo0to1(static_cast<float>(selected - 1));
            if (std::abs(parameter->getValue() - normalised) > 1.0e-6f)
            {
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost(normalised);
                parameter->endChangeGesture();
            }
        }
    };

    // The art covers as many voicings as the parameter offers choices. If someone adds a
    // topology and stops there, the extra choice silently reuses the first livery, so say so
    // here rather than leaving it to be noticed by eye.
    jassert(tf::ui::faceplateStyleCount()
            == static_cast<std::size_t>(processor.getParameters()
                                            .getParameter("topology")->getAllValueStrings().size()));
    applyFaceplateStyle();
}

juce::Rectangle<int> AmplifierPage::faceplateArea() const
{
    return getLocalBounds().withTrimmedTop(chipRowHeight + 10);
}

int AmplifierPage::choiceIndex(const juce::String& parameterId) const
{
    const auto* value = processor.getParameters().getRawParameterValue(parameterId);
    if (value == nullptr) return 0;
    return static_cast<int>(std::lround(value->load(std::memory_order_relaxed)));
}

void AmplifierPage::applyFaceplateStyle()
{
    paintedTopology = choiceIndex("topology");
    paintedInstrument = choiceIndex("instrument");
    faceplate.setStyle(tf::ui::faceplateStyle(paintedTopology, paintedInstrument));

    const auto lettering = faceplate.style().panelText;
    for (auto& label : knobLabels) label.setColour(juce::Label::textColourId, lettering);
    for (auto& knob : knobs)
        knob.setColour(juce::Slider::textBoxTextColourId, lettering.withAlpha(0.88f));
    // The chip watches the voicing parameter but cannot see the instrument, and a bass rig
    // wears a different cabinet -- so tell it rather than waiting for it to notice.
    if (voicing != nullptr) voicing->invalidate();
    repaint();
}

tf::ui::GearCatalogue AmplifierPage::buildVoicingCatalogue()
{
    tf::ui::GearCatalogue catalogue;
    catalogue.heading = "Choose an amplifier";
    for (std::size_t index = 0; index < tf::ui::ampCharacterCount; ++index)
        catalogue.categories.push_back(
            tf::ui::ampCharacterName(static_cast<tf::ui::AmpCharacter>(index)));

    // The instrument is read once here rather than captured per tile: a bass rig wears a
    // different cabinet, and the faces in the picker should be the ones the user will get.
    const auto instrument = choiceIndex("instrument");
    for (std::size_t index = 0; index < tf::ui::faceplateStyleCount(); ++index)
    {
        /* Bass-native voicings are hidden from a guitarist, and the *view* is what filters.

           The choice parameter still offers all thirteen, unchanged, because filtering the
           parameter would make a host automation lane's meaning depend on another parameter's
           value -- which is the one thing the topology enum's own comment forbids. What is safe
           here is that `GearTile::parameterIndex` carries the value it writes, so skipping a tile
           moves nothing: the eighth tile still writes 7 whether or not the sixth was drawn.

           They still answer for guitar (`makeOriginalPreset` gives every one of them a guitar
           reading), so a host or an old project that selects one gets a real amplifier. This only
           stops them being *offered* to somebody who has no use for them.
        */
        if (instrument == 0 && nts::amp::topologyAffinity(static_cast<nts::amp::Topology>(index))
                                   == nts::amp::TopologyAffinity::bass)
            continue;
        const auto topology = static_cast<int>(index);
        const auto style = tf::ui::faceplateStyle(topology, instrument);
        catalogue.tiles.push_back(
            { topology, style.badge, style.blurb, static_cast<int>(style.character),
              [this, topology, instrument](juce::Graphics& graphics, juce::Rectangle<float> area)
              { thumbnails.paint(graphics, area, topology, instrument); } });
    }
    return catalogue;
}

void AmplifierPage::rebuildPanelSwitches()
{
    const auto topology = choiceIndex("topology");
    listedTopology = topology;
    panelSwitchSelector.clear(juce::dontSendNotification);
    auto offered = 0;
    for (std::size_t index = 0; index < nts::amp::panelSwitchCount; ++index)
    {
        const auto value = static_cast<nts::amp::PanelSwitch>(index);
        if (! nts::amp::panelSwitchAppliesTo(static_cast<nts::amp::Topology>(topology), value)) continue;
        // Id is the switch value plus one, because zero means "nothing selected" to a ComboBox.
        panelSwitchSelector.addItem(juce::String(std::string(nts::amp::panelSwitchName(value))),
                                    static_cast<int>(index) + 1);
        ++offered;
    }
    /* Hidden entirely on a voicing with no switches, rather than shown holding "Standard".

       Every voicing offers `none`, so the list is never empty -- a box with one inert entry on it
       would be a control that does nothing on eleven of thirteen amplifiers, which reads as broken
       rather than as absent. Two is the first count that means anything. */
    const auto hasSwitches = offered > 1;
    panelSwitchCaption.setVisible(hasSwitches);
    panelSwitchSelector.setVisible(hasSwitches);
    panelSwitchSelector.setSelectedId(choiceIndex("panelSwitch") + 1, juce::dontSendNotification);
    resized();
}

void AmplifierPage::refresh()
{
    /* Which of these seven the analyzer is still holding.

       Only the released ones are coloured -- see `ModulePage::markAutoMatch`. Re-applied every
       tick rather than on a change signal because the set moves from three directions at once
       (this page, the dialog, host automation), and setting a colour a control already has is
       a comparison rather than a repaint. */
    for (std::size_t index = 0; index < knobs.size(); ++index)
        markAutoMatch(knobs[index], processor.autoMatchReleased(knobIds[index]));

    if (choiceIndex("topology") != listedTopology) rebuildPanelSwitches();
    else if (panelSwitchSelector.getSelectedId() != choiceIndex("panelSwitch") + 1)
        panelSwitchSelector.setSelectedId(choiceIndex("panelSwitch") + 1, juce::dontSendNotification);

    // The voicing can also be changed from the tone page, from a preset recall, from a
    // recovered rig or from host automation, so this one only ever learns about a change by
    // looking. Cheap enough to check every tick.
    if (choiceIndex("topology") != paintedTopology || choiceIndex("instrument") != paintedInstrument)
        applyFaceplateStyle();
    if (voicing != nullptr) voicing->refresh();
}

void AmplifierPage::resized()
{
    auto chip = getLocalBounds().removeFromTop(chipRowHeight);
    chipBounds = chip;
    tf::ui::layOutField(chip.removeFromLeft(160), instrumentCaption, instrumentSelector);
    chip.removeFromLeft(12);
    if (voicing != nullptr)
    {
        tf::ui::layOutField(chip.removeFromLeft(230), voicingCaption, *voicing);
        chip.removeFromLeft(12);
    }
    if (panelSwitchSelector.isVisible())
    {
        tf::ui::layOutField(chip.removeFromLeft(160), panelSwitchCaption, panelSwitchSelector);
        chip.removeFromLeft(12);
    }
    cabinetEnabled.setBounds(chip.removeFromLeft(130).withTrimmedTop(13).withHeight(28));

    const auto face = faceplateArea().toFloat();
    const auto width = face.getWidth();
    const auto height = face.getHeight();
    // The art paints its control panel around this band rather than the other way round, so
    // the knobs land on the panel at every window size and aspect ratio.
    auto deck = juce::Rectangle<int>(juce::roundToInt(face.getX() + width * 0.055f),
                                     juce::roundToInt(face.getY() + height * tf::ui::FaceplateArt::deckTop),
                                     juce::roundToInt(width * 0.890f),
                                     juce::roundToInt(height * (tf::ui::FaceplateArt::deckBottom
                                                                - tf::ui::FaceplateArt::deckTop)));
    constexpr int groupGap = 16;
    const auto columns = static_cast<float>(knobs.size());
    const auto columnWidth = (static_cast<float>(deck.getWidth())
                              - groupGap * static_cast<float>(groupSizes.size() - 1)) / columns;
    auto x = static_cast<float>(deck.getX());
    std::size_t knob = 0;
    for (std::size_t group = 0; group < groupSizes.size(); ++group)
    {
        for (int inGroup = 0; inGroup < groupSizes[group]; ++inGroup, ++knob)
        {
            auto cell = juce::Rectangle<float>(x, static_cast<float>(deck.getY()), columnWidth,
                                               static_cast<float>(deck.getHeight())).toNearestInt();
            knobLabels[knob].setBounds(cell.removeFromTop(15));
            knobs[knob].setBounds(cell.reduced(3, 0));
            x += columnWidth;
        }
        if (group + 1 < groupSizes.size())
        {
            dividerX[group] = juce::roundToInt(x + groupGap * 0.5f);
            x += groupGap;
        }
    }
}

void AmplifierPage::paint(juce::Graphics& graphics)
{
    // Drawn to the area rather than cropped to fit it, and no scrim: the art keeps its own
    // control panel dark and the lettering takes its colour from the same style, so nothing
    // has to be dimmed to stay readable.
    faceplate.paint(graphics, faceplateArea().toFloat());

    for (const auto x : dividerX)
    {
        if (x <= 0) continue;
        const auto top = static_cast<float>(knobs.front().getY());
        const auto bottom = static_cast<float>(knobs.front().getBottom());
        const auto middle = (top + bottom) * 0.5f;
        graphics.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.0f),
                                                      static_cast<float>(x), top,
                                                      juce::Colours::white.withAlpha(0.10f),
                                                      static_cast<float>(x), middle, false));
        graphics.fillRect(juce::Rectangle<float>(static_cast<float>(x), top, 1.0f, middle - top));
        graphics.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.10f),
                                                      static_cast<float>(x), middle,
                                                      juce::Colours::white.withAlpha(0.0f),
                                                      static_cast<float>(x), bottom, false));
        graphics.fillRect(juce::Rectangle<float>(static_cast<float>(x), middle, 1.0f, bottom - middle));
    }
}
