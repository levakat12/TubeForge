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
                   juce::String status, float progress);
    void paint(juce::Graphics& graphics) override;
private:
    std::optional<nts::reconstruction::ReconstructionResult> result;
    juce::String statusText;
    float progressValue {};
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
