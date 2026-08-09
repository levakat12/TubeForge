#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <nts/ecosystem/TonePackage.h>

#include <functional>
#include <memory>
#include <vector>

/// Browsing, loading and sharing .ntone rig profiles.
///
/// This is the only page with anything to say to the outside world: the shell's preset display
/// and its previous/next arrows are driven from the library. That contract is deliberately
/// narrow -- one callback out, two small queries in -- so the shell never has to know how the
/// library stores or filters anything.
class ProfileLibraryPage final : public ModulePage
{
public:
    explicit ProfileLibraryPage(TubeForgeAudioProcessor& processor);

    void resized() override;

    /// Fired whenever a profile is loaded, with the name to show as the current rig.
    std::function<void(juce::String)> onRigLoaded;

    /// Fired when the number of loadable profiles changes, so the shell can enable or disable
    /// its previous/next arrows without polling.
    std::function<void(int)> onProfileCountChanged;

    /// Steps to the profile `delta` places away in the current filtered list and loads it.
    void selectRelative(int delta);

private:
    void refreshBrowser();
    void loadProfileAtIndex(int index);
    void chooseImport();
    void chooseExportDestination();

    tf::ui::SectionPanel browsePanel { "Browse" };
    tf::ui::SectionPanel detailPanel { "Selected profile" };
    tf::ui::SectionPanel sharePanel { "Share your rig" };
    juce::TextEditor search;
    juce::TextEditor rigName;
    juce::TextEditor author;
    juce::ComboBox instrument;
    juce::ComboBox list;
    juce::ToggleButton favoritesOnly { "Favorites only" };
    juce::ToggleButton favorite { "Favorite" };
    /** Whether an exported profile carries the impulse responses the slots have loaded.

        Off by default, and that is a rights decision rather than an oversight -- see
        `TubeForgeAudioProcessor::exportCurrentTonePackage`. Most impulse responses are commercial
        and licensed for use rather than redistribution, and the plug-in cannot read a licence.
    */
    juce::ToggleButton includeCabinets { "Include my cabinet responses" };
    juce::TextButton importProfile { "Import .ntone" };
    juce::TextButton exportProfile { "Export current rig" };
    juce::TextButton loadProfile { "Load profile" };
    juce::TextButton refreshProfiles { "Refresh" };
    juce::Label details;

    std::vector<nts::ecosystem::ProfileRecord> visibleProfiles;
    int lastReportedCount { -1 };
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProfileLibraryPage)
};
