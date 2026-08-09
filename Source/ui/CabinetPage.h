#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <array>
#include <memory>

/** The cabinet's magnitude response: slot A, slot B, and what the section as a whole does.

    Every other control on this page changes a filter, and without this the only way to find out
    what any of them did was to play through them. Two microphone positions that read as "25%" and
    "65%" are six decibels apart at 3 kHz, which is a thing a picture says in one glance and a
    number does not say at all.

    The curves are pushed in rather than pulled: they cost a transform to compute for a loaded
    impulse response, so the processor recomputes them when a response changes and this draws
    whatever it was last handed.
*/
class CabinetResponseView final : public juce::Component
{
public:
    void setCurves(std::vector<float> slotA, std::vector<float> slotB, std::vector<float> sum,
                   bool secondMuted);
    void paint(juce::Graphics& graphics) override;

private:
    std::vector<float> first, second, summed;
    bool secondIsMuted {};
};

/// The speaker, and the two microphones on it.
///
/// Two slots, each either the built-in response or one loaded from a file, each with the level,
/// placement, polarity and delay a microphone in front of a cabinet actually has. Loading happens
/// off the message thread and fades in, so a response can be swapped while playing.
///
/// The stage this drives sits after whichever engine is selected -- traditional, neural capture or
/// physical circuit -- so everything on this page applies to all three. That was not true while the
/// cabinet was a member of the traditional amplifier.
class CabinetPage final : public ModulePage,
                          private juce::FileDragAndDropTarget
{
public:
    explicit CabinetPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;
    /// Draws the drop highlight around whichever slot a dragged file is over.
    void paintOverChildren(juce::Graphics& graphics) override;
    void setPerformanceLimits(int maximumOversamplingFactor, bool singleCabinet) override;

private:
    /// One cabinet slot's controls. Two of these, differing only in which slot they drive.
    struct SlotControls
    {
        juce::Label caption;
        juce::TextButton load { "Browse" };
        /// The file dialog, for a response outside the browser's folder.
        juce::TextButton browse { "File" };
        juce::TextButton clear { "Use built-in" };
        /// Mutes the *other* slot, so a blend can be judged one microphone at a time. Held in the
        /// page rather than as a parameter: it is a listening action, not part of the rig, and a
        /// solo saved into a project would come back as a cabinet with one side missing.
        juce::TextButton solo { "Solo" };
        juce::TextButton save { "Export" };
        juce::Label status;
        /// How a *loaded* response is prepared. The mirror image of the model controls below:
        /// these appear only while a file is loaded, because that is the only time they do
        /// anything.
        juce::Label lengthCaption, normalisationCaption;
        juce::ComboBox length, normalisation;
        juce::ToggleButton minimumPhase { "Minimum phase" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lengthAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> normalisationAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> minimumPhaseAttachment;
        /// The built-in model's controls. Hidden while a user response is loaded, because a
        /// cabinet chooser that does nothing is worse than one that is not there.
        juce::Label modelCaption, micCaption, positionCaption, distanceCaption;
        juce::ComboBox model, microphone;
        juce::Slider position, distance;
        juce::Label levelCaption, panCaption, delayCaption;
        juce::Slider level, pan, delay;
        juce::ToggleButton phase { "Invert polarity" };
        juce::ToggleButton mute { "Mute" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modelAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> micAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> positionAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> distanceAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> levelAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> phaseAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttachment;
    };

    void chooseImpulse(int slot);
    void layOutSlot(juce::Rectangle<int> area, SlotControls& controls);
    /// Writes the measured lag into whichever slot's delay wants it. See
    /// TubeForgeAudioProcessor::suggestedCabinetAlignment.
    void applySuggestedAlignment();
    void toggleSolo(int slot);
    void runCabinetMatch();
    void exportResponse(int slot);
    /* Dropping a file on a slot loads it there.

       Which slot is decided by where the pointer is, which is the only answer that does not need
       explaining: the two panels are side by side and the one under the cursor is the one being
       aimed at. A drop anywhere else is refused rather than guessed at. */
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    /// Slot under a point in this component's coordinates, or -1.
    [[nodiscard]] int slotAt(juce::Point<int> position) const;
    /// The slot a drag is currently over, for the highlight. -1 when none.
    int dragTargetSlot { -1 };

    std::array<tf::ui::SectionPanel, 2> slotPanels { tf::ui::SectionPanel { "Response A" },
                                                     tf::ui::SectionPanel { "Response B" } };
    tf::ui::SectionPanel blendPanel { "Cabinet section" };
    tf::ui::SectionPanel voicingPanel { "Voicing and output" };
    tf::ui::SectionPanel matchPanel { "Cabinet Match" };
    std::array<SlotControls, 2> slots;
    /// -1 for nothing soloed, otherwise the slot being listened to on its own.
    int soloedSlot { -1 };

    juce::ToggleButton cabinetEnabled { "Cabinet section active" };
    /* There is deliberately no separate alignment slider here.

       `cabinetAlignment` is slot B's delay and always was; giving the section its own control for
       it as well would be two knobs on one parameter, which is the kind of duplication that has
       people wondering which of the two is the real one. Slot B's Delay field is that control, and
       Align automatically writes into whichever slot's delay the measurement says is early. */
    juce::Label blendCaption, widthCaption;
    juce::Slider blend, width;
    juce::TextButton autoAlign { "Align automatically" };
    juce::Label alignmentReading;
    /** Fits the cabinet to the song the rig was matched from. See
        TubeForgeAudioProcessor::matchCabinetToReference, which spells out what the result is and
        is not -- the wording on this page has to keep saying so, because the tempting reading of
        the button is a claim the feature cannot make. */
    juce::TextButton matchCabinet { "Match to the song" };
    juce::Label matchCaption, matchReading;
    juce::Slider matchDepth;
    juce::Label lowCutCaption, highCutCaption, diBlendCaption, outputTrimCaption;
    juce::Slider lowCut, highCut, diBlend, outputTrim;
    /// What a mix bus will do to the stereo image. See CabinetPage::refresh.
    juce::Label monoFold;
    CabinetResponseView response;
    juce::Label help;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabinetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> blendAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> widthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lowCutAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> highCutAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> diBlendAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputTrimAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CabinetPage)
};
