#pragma once

#include "UiSupport.h"

#include <nts/nam/CaptureLibrary.h>

#include <functional>
#include <memory>
#include <vector>

class TubeForgeAudioProcessor;

/// Choosing a converted capture, wherever the choice is being made.
///
/// The Pedals page and the Neural Capture page both need the same thing -- pick one of the
/// captures the user has imported, or import some more -- and neither wants to send the user to
/// a different page to do it. This is that list, shown in place over the page that asked for it,
/// with an Import button running the same conversion the Captures page does.
///
/// It sorts by how well a capture fits where it is going and it says when one does not fit, but
/// it never withholds one: `gear_type` is the capture author's label, not a rule, and running a
/// full-rig capture in a pedal slot is a strange thing to want and a legitimate thing to try.
class CapturePicker final : public juce::Component,
                            private juce::Timer,
                            private juce::ListBoxModel
{
public:
    CapturePicker(TubeForgeAudioProcessor& processor, nts::nam::GearKind destination,
                  juce::String heading);
    ~CapturePicker() override;

    void resized() override;
    void paint(juce::Graphics& graphics) override;

    /// Called with the chosen capture's artifact directory, after the picker has closed itself.
    std::function<void(juce::File)> onChosen;

    /** Opens a picker filling `parent` and returns it.

        `parent` takes ownership, and the picker deletes itself when it is dismissed or when a
        capture is chosen, so a caller only ever has to supply the callback.
    */
    static void openOver(juce::Component& parent, TubeForgeAudioProcessor& processor,
                         nts::nam::GearKind destination, juce::String heading,
                         std::function<void(juce::File)> chosen);

private:
    void timerCallback() override;
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height,
                          bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

    void reload();
    void dismiss();
    void chooseSelected();
    void chooseImportSource();

    TubeForgeAudioProcessor& processor;
    nts::nam::GearKind destination;
    juce::String title;

    juce::TextEditor search;
    juce::ListBox list { "captures", this };
    juce::TextButton importButton { "Import .zip or .nam" };
    juce::TextButton loadSelected { "Load selected" };
    juce::TextButton close { "Close" };
    juce::Label status;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::vector<nts::nam::LibraryEntry> shown;
    /// Rebuilding the list on every tick would fight the user's scrolling, so it is rebuilt only
    /// when the library or the search text has actually changed.
    std::size_t lastLibrarySize { static_cast<std::size_t>(-1) };
    juce::String lastFilter { "\n" };
    /// Set once dismissal is queued, so a second click cannot queue a second delete.
    bool dismissing {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CapturePicker)
};
