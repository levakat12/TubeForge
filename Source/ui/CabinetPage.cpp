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
    tf::ui::configureFieldCaption(blendCaption, "A / B blend");
    tf::ui::configureSlider(blend);
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

    cabinetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "cabinet", cabinetEnabled);
    alignmentAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "cabinetAlignment", alignment);
    blendAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "cabinetBlend", blend);
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
