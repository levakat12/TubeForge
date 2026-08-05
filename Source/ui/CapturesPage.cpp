#include "CapturesPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>
#include <cmath>

namespace theme = tf::theme;

namespace
{
constexpr int rowHeight = 42;
/// The pedal Kind choice is built from PedalKind, so this cannot drift from it.
constexpr int neuralPedalKind = static_cast<int>(nts::pedals::PedalKind::neuralCapture);
/// "Neural capture" in the engine-mode choice list: Traditional, Neural capture, Physical circuit.
constexpr int neuralEngineMode = 1;
} // namespace

CapturesPage::CapturesPage(TubeForgeAudioProcessor& processorToUse) : ModulePage(processorToUse)
{
    tf::ui::configureLabel(
        help,
        "Import a .zip, a .nam, or a folder of either: every capture inside is converted and "
        "added here. Conversion is keyed by each capture's own digest, so re-importing an "
        "archive you already have costs nothing and cannot create duplicates. Load a capture "
        "from here, or from the load buttons on the Pedals and Neural Capture pages.",
        11.0f, false, theme::textTertiary);
    help.setJustificationType(juce::Justification::topLeft);

    tf::ui::configureFieldCaption(tierCaption, "Tier");
    tier.addItemList({ "Standard (full quality)", "Lite (about a third of the cost)" }, 1);
    tier.setSelectedId(1, juce::dontSendNotification);
    tier.setTooltip("Corpus captures hold two independently trained models. Standard is the "
                    "full-quality one; lite costs roughly a third as much to run.");
    tf::ui::configureLabel(importStatus, "No captures imported yet", 11.0f, false, theme::textSecondary);
    importButton.setTooltip("Reads the archive in the plug-in -- no command line, no interpreter.");
    cancelImport.setTooltip("Stops after the capture currently being converted.");

    search.setTextToShowWhenEmpty("Search captures", theme::textTertiary);
    search.setFont(theme::font(12.0f, false));
    search.onTextChange = [this] { rebuild(); };
    for (std::size_t index = 0; index < filters.size(); ++index)
    {
        filters[index].setClickingTogglesState(true);
        filters[index].setRadioGroupId(0x7ca9);
        filters[index].onClick = [this, index]
        {
            filter = static_cast<Filter>(index);
            rebuild();
        };
    }
    filters[0].setToggleState(true, juce::dontSendNotification);

    list.setRowHeight(rowHeight);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setOutlineThickness(0);

    tf::ui::configureFieldCaption(destinationCaption, "Load into");
    destination.addItem("Amp engine", 1);
    for (std::size_t slot = 0; slot < nts::pedals::slotCount; ++slot)
        destination.addItem("Pedal " + juce::String(static_cast<int>(slot) + 1),
                            static_cast<int>(slot) + 2);
    destination.setSelectedId(1, juce::dontSendNotification);
    tf::ui::configureLabel(detail, "Select a capture to see what it is.", 10.5f, false,
                           theme::textTertiary);
    detail.setJustificationType(juce::Justification::centredLeft);
    remove.setTooltip("Deletes the converted artifact. The original .nam or .zip is untouched.");

    addAndMakeVisible(help);
    addAndMakeVisible(importPanel);
    addAndMakeVisible(listPanel);
    for (auto* component : std::initializer_list<juce::Component*> {
             &importButton, &cancelImport, &tierCaption, &tier, &importStatus, &importProgress })
        importPanel.addAndMakeVisible(*component);
    for (auto* component : std::initializer_list<juce::Component*> {
             &search, &list, &destinationCaption, &destination, &send, &remove, &detail })
        listPanel.addAndMakeVisible(*component);
    for (auto& button : filters) listPanel.addAndMakeVisible(button);

    importButton.onClick = [this] { chooseImportSource(); };
    cancelImport.onClick = [this] { processor.cancelCaptureImport(); };
    send.onClick = [this] { sendSelectedTo(destination.getSelectedId() - 2); };
    remove.onClick = [this] { deleteSelected(); };

    rebuild();
}

CapturesPage::~CapturesPage() = default;

void CapturesPage::resized()
{
    auto area = getLocalBounds();
    help.setBounds(area.removeFromTop(48));
    area.removeFromTop(8);
    importPanel.setBounds(area.removeFromTop(96));
    area.removeFromTop(10);
    listPanel.setBounds(area);

    auto importArea = importPanel.contentArea();
    auto top = importArea.removeFromTop(30);
    importButton.setBounds(top.removeFromLeft(170));
    top.removeFromLeft(10);
    cancelImport.setBounds(top.removeFromLeft(90));
    top.removeFromLeft(18);
    tf::ui::layOutField(top.removeFromLeft(230).withHeight(44).withY(top.getY() - 12), tierCaption, tier);
    importArea.removeFromTop(8);
    importProgress.setBounds(importArea.removeFromTop(4));
    importArea.removeFromTop(4);
    importStatus.setBounds(importArea);

    auto listArea = listPanel.contentArea();
    auto filterRow = listArea.removeFromTop(28);
    search.setBounds(filterRow.removeFromLeft(std::min(filterRow.getWidth() - 330, 300)));
    filterRow.removeFromLeft(16);
    for (auto& button : filters)
    {
        button.setBounds(filterRow.removeFromLeft(100));
        filterRow.removeFromLeft(6);
    }
    listArea.removeFromTop(8);

    auto footer = listArea.removeFromBottom(30);
    remove.setBounds(footer.removeFromRight(90));
    footer.removeFromRight(10);
    send.setBounds(footer.removeFromRight(110));
    footer.removeFromRight(8);
    destination.setBounds(footer.removeFromRight(140));
    footer.removeFromRight(8);
    destinationCaption.setBounds(footer.removeFromRight(70));
    footer.removeFromRight(12);
    detail.setBounds(footer);
    listArea.removeFromBottom(8);
    list.setBounds(listArea);
}

void CapturesPage::refresh()
{
    const auto running = processor.captureImportRunning();
    importButton.setEnabled(! running);
    cancelImport.setEnabled(running);
    tier.setEnabled(! running);
    importProgress.setLevel(running ? processor.captureImportProgress() : 0.0f);
    importStatus.setText(processor.captureImportStatusText(), juce::dontSendNotification);

    // The import worker adds captures as it goes, and the list should fill while it runs rather
    // than appearing all at once at the end.
    if (processor.captureLibrary().entries().size() != lastLibrarySize) rebuild();
}

int CapturesPage::getNumRows() { return static_cast<int>(shown.size()); }

void CapturesPage::paintListBoxItem(int row, juce::Graphics& graphics, int width, int height,
                                    bool selected)
{
    if (row < 0 || row >= static_cast<int>(shown.size())) return;
    const auto& entry = shown[static_cast<std::size_t>(row)];
    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(4, 3);
    if (selected)
    {
        graphics.setColour(theme::accent.withAlpha(0.16f));
        graphics.fillRoundedRectangle(bounds.toFloat(), 4.0f);
    }
    bounds = bounds.reduced(10, 3);

    graphics.setColour(theme::textPrimary);
    graphics.setFont(theme::font(12.0f, false));
    graphics.drawText(juce::String(entry.displayName), bounds.removeFromTop(17),
                      juce::Justification::centredLeft, true);
    graphics.setColour(theme::textTertiary);
    graphics.setFont(theme::font(10.0f, false));
    graphics.drawText(entry.summary(), bounds, juce::Justification::centredLeft, true);
}

void CapturesPage::selectedRowsChanged(int)
{
    const auto* entry = selectedEntry();
    if (entry == nullptr)
    {
        detail.setText("Select a capture to see what it is.", juce::dontSendNotification);
        return;
    }
    juce::String text = entry->summary();
    if (! entry->modeledBy.empty()) text += "  /  by " + juce::String(entry->modeledBy);
    if (! entry->source.empty()) text += "  /  from " + juce::String(entry->source);
    detail.setText(text, juce::dontSendNotification);
}

const nts::nam::LibraryEntry* CapturesPage::selectedEntry() const
{
    const auto row = list.getSelectedRow();
    return row >= 0 && row < static_cast<int>(shown.size()) ? &shown[static_cast<std::size_t>(row)]
                                                            : nullptr;
}

void CapturesPage::rebuild()
{
    const auto text = search.getText().trim();
    const auto all = processor.captureLibrary().entries();
    shown.clear();
    shown.reserve(all.size());
    for (const auto& entry : all)
    {
        const auto amplifierish = entry.gear == nts::nam::GearKind::amp
                               || entry.gear == nts::nam::GearKind::fullRig;
        // An unlabelled capture is shown under every filter rather than hidden by all of them.
        const auto known = entry.gear != nts::nam::GearKind::unknown;
        if (filter == Filter::amplifiers && known && ! amplifierish) continue;
        if (filter == Filter::pedals && known && amplifierish) continue;
        if (text.isNotEmpty())
        {
            const auto haystack = juce::String(entry.displayName) + " " + juce::String(entry.gearMake)
                                + " " + juce::String(entry.gearModel) + " "
                                + juce::String(entry.toneType) + " " + juce::String(entry.source);
            if (! haystack.containsIgnoreCase(text)) continue;
        }
        shown.push_back(entry);
    }
    lastLibrarySize = all.size();
    lastSearch = text;
    list.updateContent();
    list.repaint();
    send.setEnabled(! shown.empty());
    remove.setEnabled(! shown.empty());
    selectedRowsChanged(list.getSelectedRow());
}

void CapturesPage::chooseImportSource()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose a capture archive, capture, or folder", juce::File {}, "*.zip;*.nam");
    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = juce::Component::SafePointer<CapturesPage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto tier = safeThis->tier.getSelectedId() == 2 ? nts::nam::Tier::lite
                                                                  : nts::nam::Tier::standard;
            safeThis->processor.requestCaptureImport(chooser.getResult(), tier);
        });
}

void CapturesPage::sendSelectedTo(int pedalSlot)
{
    const auto* entry = selectedEntry();
    if (entry == nullptr) return;

    const auto destinationKind = pedalSlot < 0 ? nts::nam::GearKind::amp : nts::nam::GearKind::pedal;
    const auto mismatched = nts::nam::CaptureLibrary::isMismatch(entry->gear, destinationKind);

    const auto setParameter = [this](const char* id, float value)
    {
        if (auto* parameter = processor.getParameters().getParameter(id))
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
            parameter->endChangeGesture();
        }
    };

    juce::String message;
    if (pedalSlot < 0)
    {
        processor.requestNeuralModelLoad(entry->artifact);
        // Switched for them: loading a capture into the amp engine and then not hearing it
        // because a different engine is selected is a worse surprise than the switch.
        setParameter("engineMode", static_cast<float>(neuralEngineMode));
        message = "Loaded into the amp engine, and the engine mode switched to Neural capture.";
    }
    else
    {
        processor.requestPedalModelLoad(pedalSlot, entry->artifact);
        setParameter(TubeForgeAudioProcessor::pedalParameterId(
                         static_cast<std::size_t>(pedalSlot),
                         TubeForgeAudioProcessor::PedalControl::kind),
                     static_cast<float>(neuralPedalKind));
        message = "Loaded into pedal " + juce::String(pedalSlot + 1)
                + ", and that slot switched to Neural capture.";
    }
    if (mismatched)
        message += "  This capture is labelled " + juce::String(nts::nam::gearKindName(entry->gear))
                 + ", which is not what this destination usually holds -- it will still play.";
    detail.setText(message, juce::dontSendNotification);
}

void CapturesPage::deleteSelected()
{
    const auto* entry = selectedEntry();
    if (entry == nullptr) return;
    const auto id = entry->id;
    const auto name = juce::String(entry->displayName);
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, "Delete capture",
        "Delete the converted artifact for \"" + name
            + "\"?\n\nThe original .nam or .zip it came from is not touched, so it can be "
              "imported again.",
        "Delete", "Keep", this,
        juce::ModalCallbackFunction::create(
            [safeThis = juce::Component::SafePointer<CapturesPage>(this), id](int result)
            {
                if (safeThis == nullptr || result == 0) return;
                std::string error;
                if (! safeThis->processor.captureLibrary().remove(id, error))
                    safeThis->detail.setText(juce::String(error), juce::dontSendNotification);
                safeThis->rebuild();
            }));
}
