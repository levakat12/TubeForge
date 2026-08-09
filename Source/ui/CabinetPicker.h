#pragma once

#include "UiSupport.h"

#include <nts/ir/CabinetLibrary.h>

#include <functional>
#include <memory>
#include <vector>

class TubeForgeAudioProcessor;

/** Choosing an impulse response from the user's own folder of them.

    Modelled on `CapturePicker`: it fills its parent, owns itself, and deletes itself when it is
    dismissed. The deferred delete is not a style choice — this dismisses from inside its own
    button handler, and JUCE keeps touching a component after the handler returns.

    What it is *not* is an import. A `.nam` capture has to be converted before the audio thread can
    read it, so the capture library owns copies; an impulse response is a WAV file, and the folder
    the user already keeps their packs in is the library. Copying them would produce a second copy
    of every pack and a list that disagrees with the disk the first time a file is renamed.

    Choosing a response **auditions** it: the picker stays open and loads each selection into the
    slot that opened it, because a shelf of four hundred responses is a thing you listen through
    rather than read. Close is what commits — there is nothing to commit, the slot already has it.
*/
class CabinetPicker final : public juce::Component,
                            private juce::ListBoxModel
{
public:
    CabinetPicker(TubeForgeAudioProcessor& processor, int slot);
    ~CabinetPicker() override;

    void resized() override;
    void paint(juce::Graphics& graphics) override;

    /** Opens a picker filling `parent`. `parent` takes ownership and the picker deletes itself. */
    static void openOver(juce::Component& parent, TubeForgeAudioProcessor& processor, int slot);

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height,
                          bool selected) override;
    void selectedRowsChanged(int row) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& event) override;

    void rebuild();
    void dismiss();
    void chooseRoot();
    /// Loads the response on `row` into the slot this picker was opened for.
    void audition(int row);

    TubeForgeAudioProcessor& processor;
    int targetSlot {};
    /// The rows currently shown: the library filtered by the search box and the folder rail.
    std::vector<nts::ir::CabinetLibraryEntry> visible;
    /// Empty means every folder. Matches `CabinetLibraryEntry::folder`.
    juce::String activeFolder;
    /// -1 for the "All" tab; otherwise an index into the library's folder list.
    int activeFolderIndex { -1 };
    /// True while the Recents shelf is showing instead of the scan.
    bool showingRecents {};

    juce::TextEditor search;
    juce::ListBox list { "cabinets", this };
    juce::ComboBox folderRail;
    juce::TextButton recentsButton { "Recents" };
    juce::TextButton favouriteButton { "Favourite" };
    juce::TextButton rootButton { "Choose folder" };
    juce::TextButton close { "Close" };
    juce::Label status;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CabinetPicker)
};
