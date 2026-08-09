#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <nts/reconstruction/SourceReconstruction.h>

#include <array>
#include <memory>
#include <optional>

/// Draws the reconstruction run: its progress through the nine stages, what it found in the
/// chosen region, and the candidate rigs it produced.
class ReconstructionView final : public juce::Component
{
public:
    void setResult(std::optional<nts::reconstruction::ReconstructionResult> value,
                   juce::String status, float progress, juce::StringArray chainWarnings);
    void paint(juce::Graphics& graphics) override;
private:
    std::optional<nts::reconstruction::ReconstructionResult> result;
    juce::String statusText;
    float progressValue {};
    /// What the live chain still does to the rig that was applied, drawn under the candidates.
    juce::StringArray warnings;
};

/// Rebuilding a tone from a song the user owns.
class SongMatchPage final : public ModulePage
{
public:
    explicit SongMatchPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;

private:
    void chooseSong();
    void chooseExportDestination();

    juce::TextButton importSong { "Import song" };
    juce::TextButton cancel { "Cancel" };
    juce::TextButton useRegion { "Use this part" };
    juce::TextButton applyCandidate { "Apply" };
    juce::TextButton exportCandidate { "Export profile" };
    /// On by default: a candidate is chosen by how it sounds on its own, so hearing it
    /// through a board and a reverb it was never ranked with is the surprising outcome.
    juce::ToggleButton isolateChain { "Bypass pedals and effects on apply" };
    /** Hands the whole rig to the analyzer.

        Off by default, and that is the deliberate answer: applying a candidate has always been a
        one-shot write the user is then free to edit, and a plug-in that silently starts guarding
        forty-seven controls because somebody imported a song would be answering a question
        nobody asked.
    */
    juce::ToggleButton autoMatch { "Auto Match: hold every setting" };
    /** Follows the live signal within 6 dB of what the match set, and only the input trim and
        the gate threshold. Off by default -- see the parameter's own note.
    */
    juce::ToggleButton tracking { "Follow the live signal" };
    /// Only useful in the overridden state, so it is hidden the rest of the time.
    juce::TextButton reclaim { "Give them back" };
    /// Appears only once the user has switched the warnings off, because a preference with no
    /// way back is a trap. Hidden the rest of the time, where it would only be noise.
    juce::TextButton restoreWarnings { "Warnings: off" };
    juce::Label autoStatus;
    juce::ComboBox target;
    juce::ComboBox stereoMode;
    juce::ComboBox region;
    juce::ComboBox candidate;
    std::array<juce::Label, 4> captions;
    ReconstructionView view;

    /// The joined region labels last pushed into the box. Rebuilding the list every tick would
    /// fight the user's selection, so it only happens when the labels actually change.
    juce::String regionSignature;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SongMatchPage)
};
