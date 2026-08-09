#include "CabinetPicker.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

namespace theme = tf::theme;

CabinetPicker::CabinetPicker(TubeForgeAudioProcessor& processorToUse, int slot)
    : processor(processorToUse), targetSlot(slot)
{
    setOpaque(false);
    // Interceptsn every click, so a press that misses a control does not reach the page behind
    // this and move a knob the user cannot see.
    setInterceptsMouseClicks(true, true);

    search.setTextToShowWhenEmpty("Search responses", theme::textTertiary);
    search.onTextChange = [this] { rebuild(); };
    addAndMakeVisible(search);

    folderRail.onChange = [this]
    {
        activeFolderIndex = folderRail.getSelectedItemIndex() - 1;
        showingRecents = false;
        recentsButton.setToggleState(false, juce::dontSendNotification);
        rebuild();
    };
    addAndMakeVisible(folderRail);

    recentsButton.setClickingTogglesState(true);
    recentsButton.onClick = [this]
    {
        showingRecents = recentsButton.getToggleState();
        rebuild();
    };
    recentsButton.setTooltip("The responses you have used most recently, newest first. Kept by "
                             "path, so one on a drive that is not mounted comes back when it is.");
    addAndMakeVisible(recentsButton);

    favouriteButton.onClick = [this]
    {
        const auto row = list.getSelectedRow();
        if (row < 0 || row >= static_cast<int>(visible.size())) return;
        const auto file = visible[static_cast<std::size_t>(row)].file;
        processor.cabinetLibrary().setFavourite(file, ! processor.cabinetLibrary().isFavourite(file));
        rebuild();
    };
    favouriteButton.setTooltip("Pins the selected response to the top of the list. Stored with your "
                              "preferences rather than with the project: it is a statement about "
                              "your shelf, not about this rig.");
    addAndMakeVisible(favouriteButton);

    rootButton.onClick = [this] { chooseRoot(); };
    rootButton.setTooltip("The folder your impulse responses live in. Scanned rather than imported: "
                          "the folder you already keep them in is the library, so nothing is "
                          "copied and nothing goes stale when you rename a file.");
    addAndMakeVisible(rootButton);

    close.onClick = [this] { dismiss(); };
    addAndMakeVisible(close);

    tf::ui::configureLabel(status, "", 11.0f, false, theme::textTertiary);
    status.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(status);

    list.setRowHeight(26);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(list);

    rebuild();
}

CabinetPicker::~CabinetPicker() = default;

void CabinetPicker::openOver(juce::Component& parent, TubeForgeAudioProcessor& processor, int slot)
{
    auto picker = std::make_unique<CabinetPicker>(processor, slot);
    picker->setBounds(parent.getLocalBounds());
    parent.addAndMakeVisible(picker.release());
}

void CabinetPicker::dismiss()
{
    // Deferred: this runs from inside a button's own handler, and JUCE keeps touching a component
    // after the handler returns. Deleting synchronously is a use-after-free that survives most of
    // the time, which is the worst kind.
    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<CabinetPicker>(this)]
    {
        if (safeThis != nullptr) delete safeThis.getComponent();
    });
}

void CabinetPicker::chooseRoot()
{
    fileChooser = std::make_unique<juce::FileChooser>("Choose your impulse response folder",
                                                      processor.cabinetLibrary().rootDirectory());
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = juce::Component::SafePointer<CabinetPicker>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.cabinetLibrary().setRootDirectory(chooser.getResult());
            safeThis->rebuild();
        });
}

void CabinetPicker::rebuild()
{
    auto& library = processor.cabinetLibrary();
    const auto folders = library.folders();

    // Rebuilt only when the folder list actually changed, or the box would fight the selection
    // every time the search text moved.
    if (folderRail.getNumItems() != static_cast<int>(folders.size()) + 1)
    {
        folderRail.clear(juce::dontSendNotification);
        folderRail.addItem("All folders", 1);
        for (std::size_t index = 0; index < folders.size(); ++index)
            folderRail.addItem(folders[index], static_cast<int>(index) + 2);
        folderRail.setSelectedItemIndex(activeFolderIndex + 1, juce::dontSendNotification);
    }
    activeFolder = activeFolderIndex >= 0 && activeFolderIndex < static_cast<int>(folders.size())
        ? folders[static_cast<std::size_t>(activeFolderIndex)] : juce::String();

    const auto query = search.getText().trim();
    visible.clear();
    const auto source = showingRecents ? library.recents() : library.entries();
    for (const auto& entry : source)
    {
        if (! showingRecents && activeFolder.isNotEmpty() && entry.folder != activeFolder) continue;
        // Matched against the folder as well as the name, because pack authors put the useful
        // part in whichever of the two they felt like.
        if (query.isNotEmpty() && ! entry.name.containsIgnoreCase(query)
            && ! entry.folder.containsIgnoreCase(query)) continue;
        visible.push_back(entry);
    }

    if (library.rootDirectory() == juce::File {})
        status.setText("Choose the folder your impulse responses live in to browse them.",
                       juce::dontSendNotification);
    else if (library.truncated())
        // Said rather than implied: a truncated scan that looks complete is worse than a short one.
        status.setText(juce::String(static_cast<int>(visible.size())) + " shown; the scan stopped "
                       "early because that folder is very large or very deep.",
                       juce::dontSendNotification);
    else
        status.setText(juce::String(static_cast<int>(visible.size())) + " of "
                           + juce::String(static_cast<int>(library.entries().size()))
                           + " responses in " + library.rootDirectory().getFileName(),
                       juce::dontSendNotification);

    list.updateContent();
    list.repaint();
}

int CabinetPicker::getNumRows() { return static_cast<int>(visible.size()); }

void CabinetPicker::paintListBoxItem(int row, juce::Graphics& graphics, int width, int height,
                                     bool selected)
{
    if (row < 0 || row >= static_cast<int>(visible.size())) return;
    const auto& entry = visible[static_cast<std::size_t>(row)];
    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(4, 1);
    if (selected)
    {
        graphics.setColour(theme::accent.withAlpha(0.16f));
        graphics.fillRoundedRectangle(bounds.toFloat(), 4.0f);
    }
    bounds.reduce(8, 0);

    // The star occupies a fixed column whether or not it is filled, so the names below it line up
    // rather than shifting by a character when a response is favourited.
    auto star = bounds.removeFromLeft(16);
    graphics.setColour(entry.favourite ? theme::accent : theme::textTertiary.withAlpha(0.35f));
    graphics.setFont(theme::font(12.0f));
    graphics.drawText(entry.favourite ? "*" : "-", star, juce::Justification::centredLeft);

    graphics.setColour(theme::textPrimary);
    graphics.setFont(theme::font(12.0f));
    const auto folderWidth = entry.folder.isEmpty() ? 0 : std::min(bounds.getWidth() / 3, 200);
    graphics.drawText(entry.name, bounds.withTrimmedRight(folderWidth),
                      juce::Justification::centredLeft, true);
    if (folderWidth > 0)
    {
        graphics.setColour(theme::textTertiary);
        graphics.setFont(theme::font(10.5f));
        graphics.drawText(entry.folder, bounds.removeFromRight(folderWidth),
                          juce::Justification::centredRight, true);
    }
}

void CabinetPicker::selectedRowsChanged(int row) { audition(row); }

void CabinetPicker::listBoxItemClicked(int row, const juce::MouseEvent&) { audition(row); }

void CabinetPicker::audition(int row)
{
    if (row < 0 || row >= static_cast<int>(visible.size())) return;
    const auto& entry = visible[static_cast<std::size_t>(row)];
    /* Loaded on selection rather than on a Load button.

       A shelf of four hundred responses is something you listen through, and a load that costs an
       extra click per candidate turns auditioning into a chore nobody finishes. The load is
       already crossfaded and already off the message thread, so the cost of being wrong is a
       response you can hear and move on from. */
    processor.requestCabinetIrLoad(targetSlot, entry.file);
    processor.cabinetLibrary().noteUsed(entry.file);
    favouriteButton.setButtonText(processor.cabinetLibrary().isFavourite(entry.file)
                                      ? "Unfavourite" : "Favourite");
}

void CabinetPicker::paint(juce::Graphics& graphics)
{
    // Opaque enough to read against, translucent enough that the page behind is still visible --
    // which is what says this is a panel over the page rather than a different page.
    graphics.fillAll(theme::backdropBottom.withAlpha(0.94f));
    auto bounds = getLocalBounds().reduced(12).toFloat();
    theme::glass(graphics, bounds, 10.0f);
    graphics.setColour(theme::textPrimary);
    graphics.setFont(theme::font(13.0f, true));
    graphics.drawText("Impulse responses  /  loading into " + juce::String(targetSlot == 0 ? "A" : "B"),
                      bounds.reduced(18.0f, 14.0f).removeFromTop(20.0f).toNearestInt(),
                      juce::Justification::centredLeft);
}

void CabinetPicker::resized()
{
    auto area = getLocalBounds().reduced(12).reduced(18, 14);
    area.removeFromTop(26);
    auto controls = area.removeFromTop(28);
    search.setBounds(controls.removeFromLeft(std::min(controls.getWidth(), 260)));
    controls.removeFromLeft(8);
    folderRail.setBounds(controls.removeFromLeft(std::min(controls.getWidth(), 200)));
    controls.removeFromLeft(8);
    recentsButton.setBounds(controls.removeFromLeft(std::min(controls.getWidth(), 88)));
    controls.removeFromLeft(8);
    favouriteButton.setBounds(controls.removeFromLeft(std::min(controls.getWidth(), 104)));
    controls.removeFromLeft(8);
    rootButton.setBounds(controls.removeFromLeft(std::min(controls.getWidth(), 124)));
    close.setBounds(controls.removeFromRight(std::min(controls.getWidth(), 72)));

    area.removeFromTop(8);
    status.setBounds(area.removeFromBottom(18));
    area.removeFromBottom(6);
    list.setBounds(area);
}
