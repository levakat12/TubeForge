#include "ProfileLibraryPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>

namespace theme = tf::theme;

ProfileLibraryPage::ProfileLibraryPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    search.setTextToShowWhenEmpty("Search name, author or tag", theme::textTertiary);
    rigName.setTextToShowWhenEmpty("Profile name", theme::textTertiary);
    author.setTextToShowWhenEmpty("Author", theme::textTertiary);
    rigName.setText("My TubeForge Rig", false);
    author.setText("Local User", false);
    instrument.addItemList({ "All instruments", "Guitar", "Bass" }, 1);
    instrument.setSelectedId(1, juce::dontSendNotification);
    list.setTextWhenNoChoicesAvailable("No profiles installed");
    list.setTextWhenNothingSelected("No profiles installed");
    tf::ui::configureLabel(details, "Select a profile to see who made it and what it needs.",
                           11.5f, false, theme::textSecondary);
    details.setJustificationType(juce::Justification::topLeft);

    addAndMakeVisible(browsePanel);
    addAndMakeVisible(detailPanel);
    addAndMakeVisible(sharePanel);
    for (auto* component : std::initializer_list<juce::Component*> { &search, &instrument,
             &favoritesOnly, &refreshProfiles, &list, &loadProfile, &favorite, &importProfile })
        browsePanel.addAndMakeVisible(*component);
    detailPanel.addAndMakeVisible(details);
    for (auto* component : std::initializer_list<juce::Component*> { &rigName, &author, &exportProfile })
        sharePanel.addAndMakeVisible(*component);

    search.onTextChange = [this] { refreshBrowser(); };
    instrument.onChange = [this] { refreshBrowser(); };
    favoritesOnly.onClick = [this] { refreshBrowser(); };
    refreshProfiles.onClick = [this]
    {
        const auto result = processor.refreshTonePackages();
        if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Profile refresh failed", result.getErrorMessage());
        refreshBrowser();
    };
    list.onChange = [this]
    {
        const auto index = list.getSelectedId() - 1;
        if (index < 0 || index >= static_cast<int>(visibleProfiles.size())) return;
        const auto& profile = visibleProfiles[static_cast<std::size_t>(index)];
        favorite.setToggleState(profile.favorite, juce::dontSendNotification);
        auto warning = profile.warnings.empty() ? juce::String {}
                                                : "\nWarning: " + juce::String(profile.warnings.front());
        details.setText(juce::String(profile.manifest.name) + " by " + juce::String(profile.manifest.author)
            + "\n" + juce::String(profile.manifest.instrument) + "   /   "
            + juce::String(profile.manifest.qualityTier)
            + "   /   runtime " + juce::String(profile.manifest.minimumRuntime) + " or newer"
            + (profile.signatureVerified ? "   /   trusted signature" : "   /   local or unsigned")
            + warning, juce::dontSendNotification);
    };
    favorite.onClick = [this]
    {
        const auto index = list.getSelectedId() - 1;
        if (index < 0 || index >= static_cast<int>(visibleProfiles.size())) return;
        const auto result = processor.setTonePackageFavorite(
            visibleProfiles[static_cast<std::size_t>(index)].manifest.packageId,
            favorite.getToggleState());
        if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Favorite failed", result.getErrorMessage());
        refreshBrowser();
    };
    loadProfile.onClick = [this] { loadProfileAtIndex(list.getSelectedId() - 1); };
    importProfile.onClick = [this] { chooseImport(); };
    exportProfile.onClick = [this] { chooseExportDestination(); };

    refreshBrowser();
}

void ProfileLibraryPage::resized()
{
    auto area = getLocalBounds();
    browsePanel.setBounds(area.removeFromTop(132));
    area.removeFromTop(10);
    sharePanel.setBounds(area.removeFromBottom(76));
    area.removeFromBottom(10);
    detailPanel.setBounds(area);

    auto browse = browsePanel.contentArea();
    auto filters = browse.removeFromTop(30);
    search.setBounds(filters.removeFromLeft(240).reduced(0, 1));
    filters.removeFromLeft(8);
    instrument.setBounds(filters.removeFromLeft(150).reduced(0, 1));
    filters.removeFromLeft(8);
    favoritesOnly.setBounds(filters.removeFromLeft(140).reduced(0, 1));
    refreshProfiles.setBounds(filters.removeFromRight(90).reduced(0, 1));
    browse.removeFromTop(10);
    auto selection = browse.removeFromTop(30);
    importProfile.setBounds(selection.removeFromRight(126).reduced(0, 1));
    selection.removeFromRight(8);
    favorite.setBounds(selection.removeFromRight(112).reduced(0, 1));
    selection.removeFromRight(8);
    loadProfile.setBounds(selection.removeFromRight(112).reduced(0, 1));
    selection.removeFromRight(8);
    list.setBounds(selection.reduced(0, 1));

    details.setBounds(detailPanel.contentArea());

    auto share = sharePanel.contentArea();
    rigName.setBounds(share.removeFromLeft(220).reduced(0, 1));
    share.removeFromLeft(8);
    author.setBounds(share.removeFromLeft(180).reduced(0, 1));
    share.removeFromLeft(8);
    exportProfile.setBounds(share.removeFromLeft(170).reduced(0, 1));
}

void ProfileLibraryPage::selectRelative(int delta)
{
    if (visibleProfiles.empty()) return;
    const auto count = static_cast<int>(visibleProfiles.size());
    const auto current = std::clamp(list.getSelectedId() - 1, 0, count - 1);
    loadProfileAtIndex(((current + delta) % count + count) % count);
}

void ProfileLibraryPage::loadProfileAtIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(visibleProfiles.size())) return;
    const auto& profile = visibleProfiles[static_cast<std::size_t>(index)];
    const auto name = juce::String(profile.manifest.name);
    const auto result = processor.applyTonePackage(profile.manifest.packageId);
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Could not load profile", result.getErrorMessage());
        return;
    }
    list.setSelectedId(index + 1, juce::sendNotificationSync);
    if (onRigLoaded) onRigLoaded(name);
    refreshBrowser();
}

void ProfileLibraryPage::refreshBrowser()
{
    nts::ecosystem::ProfileQuery query;
    query.searchText = search.getText().trim().toStdString();
    query.instrument = instrument.getSelectedId() == 2 ? "guitar"
                     : instrument.getSelectedId() == 3 ? "bass" : "";
    query.favoritesOnly = favoritesOnly.getToggleState();
    query.compatibleOnly = false;
    const auto previousId = list.getSelectedId() > 0
                         && list.getSelectedId() <= static_cast<int>(visibleProfiles.size())
        ? visibleProfiles[static_cast<std::size_t>(list.getSelectedId() - 1)].manifest.packageId
        : std::string {};
    visibleProfiles = processor.searchTonePackages(query);
    list.clear(juce::dontSendNotification);
    int selected {};
    for (std::size_t index = 0; index < visibleProfiles.size(); ++index)
    {
        const auto& value = visibleProfiles[index];
        list.addItem((value.favorite ? juce::String::fromUTF8("\xe2\x98\x85 ") : juce::String {})
                     + juce::String(value.manifest.name) + " - " + juce::String(value.manifest.author),
                     static_cast<int>(index + 1));
        if (value.manifest.packageId == previousId) selected = static_cast<int>(index + 1);
    }
    if (selected == 0 && ! visibleProfiles.empty()) selected = 1;
    list.setSelectedId(selected, juce::sendNotificationSync);
    if (visibleProfiles.empty())
    {
        favorite.setToggleState(false, juce::dontSendNotification);
        details.setText("No matching profiles. Import a .ntone folder, or export the rig you have now.",
                        juce::dontSendNotification);
    }

    const auto count = static_cast<int>(visibleProfiles.size());
    if (count != lastReportedCount)
    {
        lastReportedCount = count;
        if (onProfileCountChanged) onProfileCountChanged(count);
    }
}

void ProfileLibraryPage::chooseImport()
{
    fileChooser = std::make_unique<juce::FileChooser>("Choose a .ntone profile directory");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = juce::Component::SafePointer<ProfileLibraryPage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.importTonePackage(chooser.getResult());
            if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Import rejected", result.getErrorMessage());
            else safeThis->refreshBrowser();
        });
}

void ProfileLibraryPage::chooseExportDestination()
{
    fileChooser = std::make_unique<juce::FileChooser>("Export current rig as a .ntone profile",
                                                      juce::File {}, "*.ntone");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis = juce::Component::SafePointer<ProfileLibraryPage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.exportCurrentTonePackage(
                chooser.getResult(), safeThis->rigName.getText(), safeThis->author.getText());
            if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Export failed", result.getErrorMessage());
        });
}
