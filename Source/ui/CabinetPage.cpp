#include "CabinetPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

namespace theme = tf::theme;

CabinetPage::CabinetPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    static constexpr std::array captions { "Response A", "Response B" };
    for (int slot = 0; slot < 2; ++slot)
    {
        auto& controls = slots[static_cast<std::size_t>(slot)];
        tf::ui::configureFieldCaption(controls.caption, captions[static_cast<std::size_t>(slot)]);
        tf::ui::configureLabel(controls.status, "Built-in cabinet response", 12.0f, false,
                               theme::textSecondary);
        slotPanel.addAndMakeVisible(controls.caption);
        slotPanel.addAndMakeVisible(controls.load);
        slotPanel.addAndMakeVisible(controls.clear);
        slotPanel.addAndMakeVisible(controls.status);
        controls.load.onClick = [this, slot] { chooseImpulse(slot); };
        controls.clear.onClick = [this, slot] { processor.clearCabinetIr(slot); };
    }

    tf::ui::configureFieldCaption(alignmentCaption, "Alignment (samples)");
    tf::ui::configureSlider(alignment);
    alignment.setTooltip("Delays response B relative to A. This is the phase relationship between "
                         "two mic positions on a real cabinet -- a few samples changes the tone a "
                         "lot, and it can hollow the sum out. With width up it becomes a delay "
                         "between the channels, so watch the mono-fold reading below.");
    tf::ui::configureFieldCaption(blendCaption, "A / B blend");
    tf::ui::configureSlider(blend);
    blend.setTooltip("Mixes the two cabinet responses. At either end only one convolver runs, which "
                     "is also half the cabinet's CPU cost.");
    tf::ui::configureFieldCaption(widthCaption, "Stereo width (A left / B right)");
    tf::ui::configureSlider(width);
    width.setTooltip("Sends response A to the left and B to the right instead of summing them: one "
                     "guitar through two different rigs, which is how a wide rhythm sound is "
                     "actually made. At zero the output is unchanged from blend alone.");
    tf::ui::configureLabel(monoFold, "Mono fold: --", 11.0f, false, theme::textSecondary);
    tf::ui::configureLabel(help,
                           "Responses are decoded, resampled to the current rate, trimmed and "
                           "normalised before they are used, then faded in -- so a cabinet can be "
                           "swapped while you play. The file path is saved with the project.",
                           11.0f, false, theme::textTertiary);
    help.setJustificationType(juce::Justification::topLeft);

    addAndMakeVisible(slotPanel);
    addAndMakeVisible(alignPanel);
    addAndMakeVisible(help);
    alignPanel.addAndMakeVisible(cabinetEnabled);
    alignPanel.addAndMakeVisible(alignmentCaption);
    alignPanel.addAndMakeVisible(alignment);
    alignPanel.addAndMakeVisible(blendCaption);
    alignPanel.addAndMakeVisible(blend);
    alignPanel.addAndMakeVisible(widthCaption);
    alignPanel.addAndMakeVisible(width);
    alignPanel.addAndMakeVisible(monoFold);

    widthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "cabinetWidth", width);
    cabinetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "cabinet", cabinetEnabled);
    alignmentAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "cabinetAlignment", alignment);
    blendAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "cabinetBlend", blend);
}

void CabinetPage::setPerformanceLimits(int, bool singleCabinet)
{
    // Eco runs one convolution, so blending a second cabinet in is not something the engine will
    // honour. Disabled rather than moved: the stored value is untouched and comes straight back
    // when the tier is raised.
    blend.setEnabled(! singleCabinet);
    blendCaption.setText(singleCabinet ? "A / B blend (Eco: A only)" : "A / B blend",
                         juce::dontSendNotification);
    // Width needs both convolvers engaged whatever the blend says, so on the one-convolution tier
    // it is not merely ineffective but actively contrary to what the tier is for.
    width.setEnabled(! singleCabinet);
    widthCaption.setText(singleCabinet ? "Stereo width (Eco: off)"
                                       : "Stereo width (A left / B right)",
                         juce::dontSendNotification);
}

void CabinetPage::resized()
{
    auto area = getLocalBounds();
    slotPanel.setBounds(area.removeFromTop(186));
    area.removeFromTop(10);
    alignPanel.setBounds(area.removeFromTop(150));
    area.removeFromTop(12);
    help.setBounds(area.removeFromTop(52).withTrimmedLeft(13));

    auto content = slotPanel.contentArea();
    for (auto& controls : slots)
    {
        auto row = content.removeFromTop(78);
        controls.caption.setBounds(row.removeFromTop(18));
        auto buttons = row.removeFromTop(30);
        controls.load.setBounds(buttons.removeFromLeft(150).withHeight(28));
        buttons.removeFromLeft(10);
        controls.clear.setBounds(buttons.removeFromLeft(130).withHeight(28));
        row.removeFromTop(4);
        controls.status.setBounds(row);
        content.removeFromTop(6);
    }

    auto controlArea = alignPanel.contentArea();
    cabinetEnabled.setBounds(controlArea.removeFromTop(30)
                                 .withWidth(std::min(controlArea.getWidth(), 340)));
    controlArea.removeFromTop(8);
    tf::ui::layOutField(controlArea.removeFromTop(40), blendCaption, blend);
    controlArea.removeFromTop(6);
    tf::ui::layOutField(controlArea.removeFromTop(40), widthCaption, width);
    controlArea.removeFromTop(2);
    monoFold.setBounds(controlArea.removeFromTop(16));
    controlArea.removeFromTop(4);
    tf::ui::layOutField(controlArea.removeFromTop(40), alignmentCaption, alignment);
}

void CabinetPage::refresh()
{
    for (int slot = 0; slot < 2; ++slot)
    {
        auto& controls = slots[static_cast<std::size_t>(slot)];
        const auto status = processor.cabinetIrStatusText(slot);
        controls.status.setText(status, juce::dontSendNotification);
        // A missing or unreadable response still sounds -- the built-in one takes over -- so it
        // is a warning rather than an error, but it has to be visible or the user is left
        // wondering why their cabinet is not the one they chose.
        const auto unresolved = status.startsWith("Missing:") || status.contains("Could not")
                             || status.contains("longer than");
        controls.status.setColour(juce::Label::textColourId,
                                  unresolved ? theme::warn : theme::textSecondary);
        controls.clear.setEnabled(processor.cabinetIrFile(slot) != juce::File {});
    }

    /* What a mix bus will do to the stereo image, which is the reading the width control needs
       next to it rather than in a manual.

       Two different responses split left and right decorrelate spectrally and fold to mono nearly
       intact. Cabinet alignment, once width is up, is an inter-channel delay -- a comb filter
       under summing, wide on speakers and hollow the instant anything sums it, and nothing the
       user hears while monitoring in stereo tells them so. -3 dB is the uncorrelated case and
       unremarkable; past -6 dB something is cancelling. */
    const auto foldDb = processor.cabinetMonoFoldDb();
    monoFold.setText("Mono fold: " + juce::String(foldDb, 1) + " dB"
                         + (foldDb < -6.0f ? " -- cancellation, check alignment" : ""),
                     juce::dontSendNotification);
    monoFold.setColour(juce::Label::textColourId,
                       foldDb < -6.0f ? theme::warn : theme::textSecondary);
}

void CabinetPage::chooseImpulse(int slot)
{
    fileChooser = std::make_unique<juce::FileChooser>("Choose a cabinet impulse response",
                                                      processor.cabinetIrFile(slot),
                                                      "*.wav;*.aiff;*.aif;*.flac");
    // Async: the modal variant blocks the message thread, which some hosts do not tolerate.
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<CabinetPage>(this), slot](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestCabinetIrLoad(slot, chooser.getResult());
        });
}
