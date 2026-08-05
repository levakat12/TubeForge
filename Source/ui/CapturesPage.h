#pragma once

#include "ModulePage.h"
#include "ShellWidgets.h"
#include "UiSupport.h"

#include <nts/nam/CaptureLibrary.h>

#include <memory>
#include <vector>

/// The collection of Neural Amp Modeler captures the user has imported.
///
/// Importing reads a `.zip`, a `.nam`, or a folder of either, and converts **everything** inside
/// it. That is the only arrangement that makes the real archives usable: one pedal archive holds
/// up to eighty-nine captures of the same box at different settings, so there is no answer to
/// "which one" at import time -- only at load time, which is what this page and the pickers on
/// the Pedals and Neural Capture pages are for.
class CapturesPage final : public ModulePage,
                           private juce::ListBoxModel
{
public:
    explicit CapturesPage(TubeForgeAudioProcessor& processor);
    ~CapturesPage() override;

    void resized() override;
    void refresh() override;

private:
    /// Which captures the list is showing. Amps and full rigs are one filter because they go to
    /// the same place -- a full rig is an amplifier with its cabinet already on it.
    enum class Filter { all, amplifiers, pedals };

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height,
                          bool selected) override;
    void selectedRowsChanged(int row) override;

    void rebuild();
    void chooseImportSource();
    void sendSelectedTo(int pedalSlot);
    void deleteSelected();
    [[nodiscard]] const nts::nam::LibraryEntry* selectedEntry() const;

    juce::Label help;
    tf::ui::SectionPanel importPanel { "Import" };
    tf::ui::SectionPanel listPanel { "Library" };

    juce::TextButton importButton { "Import .zip or .nam" };
    juce::TextButton cancelImport { "Cancel" };
    juce::Label tierCaption;
    juce::ComboBox tier;
    juce::Label importStatus;
    tf::ui::LevelBar importProgress;

    juce::TextEditor search;
    std::array<juce::TextButton, 3> filters { juce::TextButton { "All" },
                                              juce::TextButton { "Amps & rigs" },
                                              juce::TextButton { "Pedals" } };
    juce::ListBox list { "captures", this };

    juce::Label destinationCaption;
    juce::ComboBox destination;
    juce::TextButton send { "Load into" };
    juce::TextButton remove { "Delete" };
    juce::Label detail;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::vector<nts::nam::LibraryEntry> shown;
    Filter filter { Filter::all };
    std::size_t lastLibrarySize { static_cast<std::size_t>(-1) };
    juce::String lastSearch { "\n" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CapturesPage)
};
