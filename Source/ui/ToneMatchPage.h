#pragma once

#include "ModulePage.h"
#include "UiSupport.h"
// For ChainMode, which this page's one three-way control produces and the processor consumes.
// The page holds the enumeration rather than an index of its own, so the combo box positions
// cannot drift away from the meanings they stand for.
#include "../StudioServices.h"

#include <nts/reconstruction/SourceReconstruction.h>
#include <nts/tone/ToneAnalysis.h>

#include <array>
#include <memory>
#include <optional>

/// Draws a finished tone analysis: descriptors with their uncertainty, the multi-resolution
/// tone profile, and the per-axis confidence the result carries.
class ToneAnalysisView final : public juce::Component
{
public:
    void setResult(std::optional<nts::tone::ToneAnalysisResult> analysis,
                   juce::String status, juce::String nearest, std::size_t profileCount);
    void paint(juce::Graphics& graphics) override;
private:
    std::optional<nts::tone::ToneAnalysisResult> result;
    juce::String statusText;
    juce::String nearestText;
    std::size_t profiles {};
};

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

/** Offline analysis of audio the user supplies, and the rigs rebuilt from it.

    **One page because it was always one job.** This was two -- Tone Analyzer and Song Match --
    and the split was never about what the user was doing. Both take a local file, run the same
    offline analysis off the playback thread, and report what the tone is; the only real
    difference is that a song is separated into stems and cut into regions first, and that the
    match goes on to search for rigs. Analysing a clip is the shorter half of the same pipeline,
    not a different feature, and having it on its own page meant somebody who wanted to know what
    a tone *was* had to guess which of two similarly-named entries to open.

    The `source` box is what carries the difference. It swaps the control row and the view rather
    than the page, so the shared parts -- the file chooser, the offline-work convention, the
    heading -- exist once. Both halves keep every control they had: nothing was dropped to make
    them fit, because a merge that quietly removes function is a removal wearing a merge's name.
*/
class ToneMatchPage final : public ModulePage
{
public:
    explicit ToneMatchPage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;

private:
    /// Which half of the page is showing. Ordered so the song match is the default: it is the
    /// one that produces something playable, and the clip analysis is the diagnostic beside it.
    enum class Source { song, clip };

    void applySource();
    void chooseSong();
    void chooseClip();
    void chooseExportDestination();
    /// The chain choice as the processor's own type. One place, so the combo's positions and the
    /// enumeration cannot drift apart across the three call sites that need the mapping.
    [[nodiscard]] ChainMode selectedChainMode() const noexcept;
    [[nodiscard]] Source currentSource() const noexcept
    { return source.getSelectedId() == 2 ? Source::clip : Source::song; }

    juce::ComboBox source;

    juce::TextButton importSong { "Import song" };
    juce::TextButton cancel { "Cancel" };
    juce::TextButton useRegion { "Use this part" };
    juce::TextButton applyCandidate { "Apply" };
    juce::TextButton exportCandidate { "Export profile" };
    /** What applying a candidate does to the rest of the chain.

        A three-way choice where a switch used to be. "Bypass them" stays the default and stays
        first, for the reason it was the default: a candidate is chosen by how it sounds on its
        own, so hearing it through a board and a reverb it was never ranked with is the surprising
        outcome. "Match the record" is the deliberate opposite and has to be picked -- it writes
        the sends, the cabinet placement and possibly a compressor from what the match measured
        about the mix, and on a part that already carries its own room that means applying a room
        twice.
    */
    juce::ComboBox chainMode;
    juce::Label chainCaption;
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
    std::array<juce::Label, 5> captions;
    ReconstructionView matchView;

    juce::TextButton analyzeClip { "Analyze a clip" };
    ToneAnalysisView analysisView;

    /// The joined region labels last pushed into the box. Rebuilding the list every tick would
    /// fight the user's selection, so it only happens when the labels actually change.
    juce::String regionSignature;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneMatchPage)
};
