#include "CapturePicker.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>

namespace theme = tf::theme;

namespace
{
constexpr int rowHeight = 40;

/// Matches a capture against a search box. Deliberately looks at every field the row shows, so
/// what the user can read is what they can search for.
bool matches(const nts::nam::LibraryEntry& entry, const juce::String& filter)
{
    if (filter.isEmpty()) return true;
    const auto haystack = juce::String(entry.displayName) + " " + juce::String(entry.gearMake) + " "
                        + juce::String(entry.gearModel) + " " + juce::String(entry.toneType) + " "
                        + juce::String(nts::nam::gearKindName(entry.gear)) + " "
                        + juce::String(entry.source);
    return haystack.containsIgnoreCase(filter);
}
} // namespace

CapturePicker::CapturePicker(TubeForgeAudioProcessor& processorToUse,
                             nts::nam::GearKind destinationKind, juce::String heading)
    : processor(processorToUse), destination(destinationKind), title(std::move(heading))
{
    // Opaque and hit-testing: this sits over a page whose controls must not be reachable while
    // it is up, and letting a click through to a slider underneath would be worse than modal.
    setOpaque(false);
    setInterceptsMouseClicks(true, true);

    search.setTextToShowWhenEmpty("Search captures", theme::textTertiary);
    search.setFont(theme::font(12.0f, false));
    search.onTextChange = [this] { reload(); };

    list.setRowHeight(rowHeight);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setOutlineThickness(0);

    tf::ui::configureLabel(status, {}, 10.5f, false, theme::textTertiary);
    status.setJustificationType(juce::Justification::centredLeft);

    importButton.setTooltip("Read a .zip, a .nam, or a folder of either, and convert every "
                            "capture it holds into the library.");
    loadSelected.setTooltip("Load the highlighted capture here.");

    for (auto* component : std::initializer_list<juce::Component*> {
             &search, &list, &importButton, &loadSelected, &close, &status })
        addAndMakeVisible(*component);

    importButton.onClick = [this] { chooseImportSource(); };
    loadSelected.onClick = [this] { chooseSelected(); };
    close.onClick = [this] { dismiss(); };

    reload();
    startTimerHz(8);
}

CapturePicker::~CapturePicker() = default;

void CapturePicker::openOver(juce::Component& parent, TubeForgeAudioProcessor& processor,
                             nts::nam::GearKind destination, juce::String heading,
                             std::function<void(juce::File)> chosen)
{
    auto picker = std::make_unique<CapturePicker>(processor, destination, std::move(heading));
    picker->onChosen = std::move(chosen);
    auto& reference = *picker;
    parent.addAndMakeVisible(picker.release());
    reference.setBounds(parent.getLocalBounds());
    reference.toFront(true);
}

void CapturePicker::dismiss()
{
    // Deletes itself, because making every caller own a pointer it must remember to null would
    // be four chances to leave a dangling one.
    //
    // Deferred, though, and that part is not optional: this runs from a Button's onClick, and
    // JUCE keeps touching the button -- and therefore this component -- after the callback
    // returns. Deleting synchronously is a use-after-free that survives most of the time, which
    // is the worst kind. The flag stops a second click during the wait from queueing it twice.
    if (dismissing) return;
    dismissing = true;
    stopTimer();
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<CapturePicker>(this)]
        {
            auto* picker = safe.getComponent();
            if (picker == nullptr) return;
            if (auto* parent = picker->getParentComponent()) parent->removeChildComponent(picker);
            delete picker;
        });
}

void CapturePicker::paint(juce::Graphics& graphics)
{
    // The page underneath stays visible but is clearly not the thing being used.
    graphics.fillAll(theme::backdropBottom.withAlpha(0.92f));
    const auto panel = getLocalBounds().reduced(24).toFloat();
    theme::stage(graphics, panel, 8.0f);
    theme::caption(graphics, getLocalBounds().reduced(40, 0).withY(40).withHeight(14), title,
                   theme::textSecondary);
}

void CapturePicker::resized()
{
    auto area = getLocalBounds().reduced(40);
    area.removeFromTop(28);
    auto top = area.removeFromTop(30);
    search.setBounds(top.removeFromLeft(std::min(top.getWidth() - 300, 320)));
    close.setBounds(top.removeFromRight(80));
    top.removeFromRight(8);
    importButton.setBounds(top.removeFromRight(170));
    area.removeFromTop(10);

    auto footer = area.removeFromBottom(30);
    loadSelected.setBounds(footer.removeFromRight(140));
    footer.removeFromRight(12);
    status.setBounds(footer);
    area.removeFromBottom(8);
    list.setBounds(area);
}

int CapturePicker::getNumRows() { return static_cast<int>(shown.size()); }

void CapturePicker::paintListBoxItem(int row, juce::Graphics& graphics, int width, int height,
                                     bool selected)
{
    if (row < 0 || row >= static_cast<int>(shown.size())) return;
    const auto& entry = shown[static_cast<std::size_t>(row)];
    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(6, 3);
    if (selected)
    {
        graphics.setColour(theme::accent.withAlpha(0.16f));
        graphics.fillRoundedRectangle(bounds.toFloat(), 4.0f);
    }
    bounds = bounds.reduced(8, 2);

    const auto mismatched = nts::nam::CaptureLibrary::isMismatch(entry.gear, destination);
    graphics.setColour(mismatched ? theme::warn : theme::textPrimary);
    graphics.setFont(theme::font(12.0f, false));
    graphics.drawText(juce::String(entry.displayName), bounds.removeFromTop(16),
                      juce::Justification::centredLeft, true);

    // The mismatch is named on the row rather than hidden behind a hover, because the whole
    // point of showing it is that the user notices before loading, not after.
    auto detail = entry.summary();
    if (mismatched)
        detail = juce::String("Labelled ") + nts::nam::gearKindName(entry.gear) + ", loading into "
               + juce::String(nts::nam::gearKindName(destination)).toLowerCase() + "  --  " + detail;
    graphics.setColour(mismatched ? theme::warn.withAlpha(0.75f) : theme::textTertiary);
    graphics.setFont(theme::font(10.0f, false));
    graphics.drawText(detail, bounds, juce::Justification::centredLeft, true);
}

void CapturePicker::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    list.selectRow(row);
    chooseSelected();
}

void CapturePicker::reload()
{
    const auto filter = search.getText().trim();
    auto ordered = processor.captureLibrary().orderedFor(destination);
    shown.clear();
    shown.reserve(ordered.size());
    for (auto& entry : ordered)
        if (matches(entry, filter)) shown.push_back(std::move(entry));

    lastLibrarySize = processor.captureLibrary().entries().size();
    lastFilter = filter;
    list.updateContent();
    list.repaint();

    const auto running = processor.captureImportRunning();
    loadSelected.setEnabled(! shown.empty());
    if (running)
    {
        status.setText(processor.captureImportStatusText(), juce::dontSendNotification);
    }
    else if (shown.empty())
    {
        status.setText(lastLibrarySize == 0
                           ? "No captures imported yet. Import a .zip from pedal_learning or "
                             "amp_learning to fill this list."
                           : "No capture matches that search.",
                       juce::dontSendNotification);
    }
    else
    {
        status.setText(juce::String(static_cast<int>(shown.size())) + " of "
                           + juce::String(static_cast<int>(lastLibrarySize)) + " captures",
                       juce::dontSendNotification);
    }
}

void CapturePicker::timerCallback()
{
    // An import running behind the picker adds captures to it as it goes, which is the whole
    // reason Import lives here rather than only on the Captures page.
    if (processor.captureImportRunning())
    {
        status.setText(processor.captureImportStatusText(), juce::dontSendNotification);
        importButton.setEnabled(false);
    }
    else
    {
        importButton.setEnabled(true);
    }
    if (processor.captureLibrary().entries().size() != lastLibrarySize) reload();
}

void CapturePicker::chooseSelected()
{
    const auto row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(shown.size())) return;
    const auto artifact = shown[static_cast<std::size_t>(row)].artifact;
    auto callback = onChosen;
    dismiss();
    if (callback) callback(artifact);
}

void CapturePicker::chooseImportSource()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose a capture archive, capture, or folder", juce::File {}, "*.zip;*.nam");
    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = juce::Component::SafePointer<CapturePicker>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestCaptureImport(chooser.getResult(), nts::nam::Tier::standard);
        });
}
