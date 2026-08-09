#include "SongMatchPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>
#include <cmath>

namespace theme = tf::theme;

void ReconstructionView::setResult(std::optional<nts::reconstruction::ReconstructionResult> value,
                                   juce::String status, float progress,
                                   juce::StringArray chainWarnings)
{
    result = std::move(value); statusText = std::move(status);
    progressValue = std::clamp(progress, 0.0f, 1.0f);
    warnings = std::move(chainWarnings); repaint();
}

void ReconstructionView::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    theme::glass(graphics, area, 9.0f);
    area = area.reduced(14.0f, 12.0f);

    graphics.setColour(theme::accent);
    graphics.setFont(theme::font(13.0f, true));
    graphics.drawFittedText(statusText, area.removeFromTop(26.0f).toNearestInt(), juce::Justification::centredLeft, 1);

    auto progressBar = area.removeFromTop(6.0f);
    theme::well(graphics, progressBar, 3.0f);
    if (progressValue > 0.001f)
    {
        graphics.setColour(theme::accent);
        graphics.fillRoundedRectangle(progressBar.withWidth(progressBar.getWidth() * progressValue), 3.0f);
    }
    area.removeFromTop(10.0f);

    constexpr std::array steps { "IMPORT", "STEMS", "TARGET", "REGION", "ANALYZE",
                                 "SEARCH", "COMPARE", "LIVE DI", "EXPORT" };
    auto stepArea = area.removeFromTop(34.0f);
    const auto stepWidth = stepArea.getWidth() / static_cast<float>(steps.size());
    for (std::size_t index = 0; index < steps.size(); ++index)
    {
        auto cell = juce::Rectangle<float>(stepArea.getX() + static_cast<float>(index) * stepWidth,
                                            stepArea.getY(), stepWidth - 4.0f, stepArea.getHeight());
        const auto active = progressValue >= static_cast<float>(index) / static_cast<float>(steps.size());
        theme::well(graphics, cell, 4.0f);
        if (active)
        {
            graphics.setColour(theme::accentWash);
            graphics.fillRoundedRectangle(cell, 4.0f);
        }
        graphics.setColour(active ? theme::accent : theme::textTertiary);
        graphics.setFont(theme::font(8.0f, true));
        theme::tracked(graphics, steps[index], cell.toNearestInt(), juce::Justification::centred, 1.0f);
    }
    area.removeFromTop(10.0f);

    if (! result || ! result->success)
    {
        graphics.setColour(theme::textTertiary);
        graphics.setFont(theme::font(13.0f));
        graphics.drawFittedText("Import a song you own, then choose guitar or bass and which side of the mix to listen to.\n\n"
                                "TubeForge picks the cleanest part to work from and hands back several editable rigs.\n"
                                "The song is cached locally and is never included in anything you export.",
                                area.reduced(36.0f).toNearestInt(), juce::Justification::centred, 6);
        return;
    }

    const auto& reconstruction = *result;
    auto report = area.removeFromTop(70.0f);
    graphics.setColour(theme::textPrimary);
    graphics.setFont(theme::font(11.0f, true));
    graphics.drawText(juce::String(nts::reconstruction::toString(reconstruction.reference.target).data()).toUpperCase()
        + "   /   REGION " + juce::String(reconstruction.reference.regionStartSeconds, 1) + "-"
        + juce::String(reconstruction.reference.regionEndSeconds, 1) + " s   /   ANALYSIS "
        + juce::String(juce::roundToInt(reconstruction.reference.analysisConfidence * 100.0f)) + "%",
        report.removeFromTop(22.0f), juce::Justification::centredLeft);
    graphics.setColour(theme::accent);
    graphics.drawFittedText(juce::String(reconstruction.reference.gainCharacter).toUpperCase()
        + "   /   PITCH " + reconstruction.reference.dominantPitch + " ("
        + juce::String(juce::roundToInt(reconstruction.reference.pitchConfidence * 100.0f)) + "%)   /   "
        + reconstruction.reference.estimatedTuning + "   /   OFFSET "
        + juce::String(reconstruction.reference.tuningOffsetCents >= 0.0f ? "+" : "")
        + juce::String(juce::roundToInt(reconstruction.reference.tuningOffsetCents)) + " cents",
        report.removeFromTop(22.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    graphics.setColour(theme::textTertiary);
    graphics.setFont(theme::font(9.5f));
    graphics.drawFittedText("Tone similarity compares the amp character alone; recording similarity also counts "
                            "loudness and production differences.",
                            report.toNearestInt(), juce::Justification::centredLeft, 2);

    // Taken off the bottom before the cards are measured, so a long list shortens the
    // shortlist rather than being drawn across it.
    if (! warnings.isEmpty())
    {
        constexpr auto lineHeight = 26.0f;
        auto notice = area.removeFromBottom(std::min(area.getHeight() * 0.55f,
            24.0f + static_cast<float>(warnings.size()) * lineHeight)).reduced(0.0f, 4.0f);
        theme::well(graphics, notice, 6.0f);
        notice = notice.reduced(11.0f, 7.0f);
        graphics.setColour(theme::warn);
        graphics.setFont(theme::font(8.5f, true));
        theme::tracked(graphics, "STILL IN THE CHAIN", notice.removeFromTop(12.0f).toNearestInt(),
                       juce::Justification::centredLeft, 1.0f);
        graphics.setFont(theme::font(10.5f));
        for (const auto& warning : warnings)
        {
            if (notice.getHeight() < 11.0f) break;
            graphics.drawFittedText(warning,
                notice.removeFromTop(std::min(notice.getHeight(), lineHeight)).toNearestInt(),
                juce::Justification::topLeft, 2);
        }
    }

    const auto count = std::max<std::size_t>(1, reconstruction.candidates.size());
    const auto cardHeight = std::min(58.0f, area.getHeight() / static_cast<float>(count));
    for (std::size_t index = 0; index < reconstruction.candidates.size(); ++index)
    {
        const auto& rig = reconstruction.candidates[index];
        auto card = area.removeFromTop(cardHeight).reduced(0.0f, 3.0f);
        const auto best = index == 0;
        theme::glass(graphics, card, 6.0f, best);
        if (best)
        {
            graphics.setColour(theme::accent.withAlpha(0.55f));
            graphics.drawRoundedRectangle(card.reduced(0.5f), 6.0f, 1.0f);
        }
        auto name = card.removeFromLeft(card.getWidth() * 0.54f).reduced(12.0f, 2.0f);
        graphics.setColour(theme::textPrimary);
        graphics.setFont(theme::font(11.5f, true));
        graphics.drawFittedText(juce::String(static_cast<int>(index + 1)) + ".  " + rig.rigPreset.name,
                                name.toNearestInt(), juce::Justification::centredLeft, 2);
        graphics.setColour(theme::textSecondary);
        graphics.setFont(theme::font(11.0f, true));
        graphics.drawText("TONE " + juce::String(juce::roundToInt(rig.toneSimilarity * 100.0f)) + "%",
                          card.removeFromLeft(card.getWidth() * 0.45f), juce::Justification::centred);
        graphics.setColour(theme::accent);
        graphics.drawText("RECORDING " + juce::String(juce::roundToInt(rig.recordingSimilarity * 100.0f)) + "%",
                          card, juce::Justification::centred);
    }
}

SongMatchPage::SongMatchPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    tf::ui::populateFromParameter(target, processor.getParameters(), "instrument");
    target.setSelectedId(1, juce::dontSendNotification);
    stereoMode.addItemList({ "Full stereo", "Left", "Right", "Mid", "Side", "Panned estimate" }, 1);
    stereoMode.setSelectedId(1, juce::dontSendNotification);
    region.setTextWhenNoChoicesAvailable("Import a song first");
    region.setTextWhenNothingSelected("Import a song first");
    candidate.setTextWhenNoChoicesAvailable("No candidates yet");
    candidate.setTextWhenNothingSelected("No candidates yet");
    tf::ui::configureFieldCaption(captions[0], "Instrument");
    tf::ui::configureFieldCaption(captions[1], "Listen to");
    tf::ui::configureFieldCaption(captions[2], "Part of the song");
    tf::ui::configureFieldCaption(captions[3], "Rig candidate");
    isolateChain.setToggleState(true, juce::dontSendNotification);
    isolateChain.setTooltip("Bypasses the four pedal slots and closes the delay and reverb sends "
                            "when a candidate is applied. None of them were in the offline "
                            "render the candidate was ranked from, so leaving them in means "
                            "judging the match through a sound it was never compared against. "
                            "Pedal slots keep their settings and their loaded models; the two "
                            "sends have no bypass, so their mix is turned down instead.");
    autoMatch.setTooltip("Lets the analysis own the amplifier. Applying a candidate then sets "
                         "every control it fitted -- drive, EQ, presence, resonance and the whole "
                         "Tone Shaping page -- and keeps them. You can still change any of them; "
                         "TubeForge asks once, the first time you reach for each one, and then "
                         "leaves that control to you.");
    reclaim.setTooltip("Puts the matched values back on the controls you have taken over, and "
                       "resumes holding them.");
    tracking.setTooltip("Lets the held rig follow what you are playing: the input trim aims the "
                        "incoming peak where the preamp expects it, and the gate threshold sits "
                        "just above the measured noise floor. Both stay within 6 dB of what the "
                        "match set, and nothing else moves. Useful when you swap to a guitar with "
                        "hotter pickups; off by default, because a control that moves on its own "
                        "is alarming until you know why.");
    restoreWarnings.setTooltip("You asked not to be warned before changing a control the match "
                               "had set. Click to be asked again.");
    tf::ui::configureLabel(autoStatus, "", 10.0f, false, theme::textTertiary);
    for (auto* component : std::initializer_list<juce::Component*> { &importSong, &cancel,
             &applyCandidate, &exportCandidate, &target, &stereoMode, &region, &useRegion,
             &candidate, &captions[0], &captions[1], &captions[2], &captions[3], &isolateChain,
             &autoMatch, &tracking, &autoStatus, &view })
        addAndMakeVisible(*component);
    addChildComponent(reclaim);
    addChildComponent(restoreWarnings);

    autoMatch.onClick = [this] { processor.setAutoMatchEnabled(autoMatch.getToggleState()); };
    reclaim.onClick = [this] { processor.reclaimAllAutoMatchParameters(); };
    tracking.onClick = [this] { processor.setAutoMatchTracking(tracking.getToggleState()); };
    restoreWarnings.onClick = [this] { processor.setAutoMatchWarningsSuppressed(false); };
    // Pushed rather than read back, so an automatic apply does exactly what pressing Apply with
    // the same switch set would have done. Sent once now for the default and on every change.
    processor.setAutoMatchIsolatesChain(isolateChain.getToggleState());
    isolateChain.onClick = [this]
    { processor.setAutoMatchIsolatesChain(isolateChain.getToggleState()); };

    importSong.onClick = [this] { chooseSong(); };
    cancel.onClick = [this] { processor.cancelSongReconstruction(); };
    useRegion.onClick = [this]
    {
        const auto index = std::max(0, region.getSelectedId() - 1);
        (void) processor.requestReconstructionRegion(static_cast<std::size_t>(index));
    };
    applyCandidate.onClick = [this]
    {
        const auto index = std::max(0, candidate.getSelectedId() - 1);
        (void) processor.applyReconstructionCandidate(static_cast<std::size_t>(index),
                                                      isolateChain.getToggleState());
    };
    exportCandidate.onClick = [this] { chooseExportDestination(); };
}

void SongMatchPage::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(44);
    importSong.setBounds(top.removeFromLeft(126).withTrimmedTop(13).withHeight(28));
    top.removeFromLeft(12);
    tf::ui::layOutField(top.removeFromLeft(120), captions[0], target);
    top.removeFromLeft(12);
    tf::ui::layOutField(top.removeFromLeft(160), captions[1], stereoMode);
    top.removeFromLeft(12);
    cancel.setBounds(top.removeFromLeft(84).withTrimmedTop(13).withHeight(28));
    top.removeFromLeft(16);
    isolateChain.setBounds(top.withTrimmedTop(13).withHeight(28));
    area.removeFromTop(8);

    auto regionRow = area.removeFromTop(44);
    const auto rowWidth = regionRow.getWidth();
    tf::ui::layOutField(regionRow.removeFromLeft(rowWidth * 40 / 100), captions[2], region);
    regionRow.removeFromLeft(8);
    useRegion.setBounds(regionRow.removeFromLeft(112).withTrimmedTop(13).withHeight(28));
    regionRow.removeFromLeft(12);
    tf::ui::layOutField(regionRow.removeFromLeft(150), captions[3], candidate);
    regionRow.removeFromLeft(8);
    applyCandidate.setBounds(regionRow.removeFromLeft(84).withTrimmedTop(13).withHeight(28));
    regionRow.removeFromLeft(8);
    exportCandidate.setBounds(regionRow.removeFromLeft(130).withTrimmedTop(13).withHeight(28));
    area.removeFromTop(8);

    // Auto Match gets a row of its own rather than a corner of one of the others: it is the only
    // control on this page that changes what every *other* page does.
    auto autoRow = area.removeFromTop(26);
    autoMatch.setBounds(autoRow.removeFromLeft(214));
    autoRow.removeFromLeft(6);
    tracking.setBounds(autoRow.removeFromLeft(168));
    autoRow.removeFromLeft(8);
    reclaim.setBounds(autoRow.removeFromLeft(116).reduced(0, 1));
    autoRow.removeFromLeft(6);
    restoreWarnings.setBounds(autoRow.removeFromLeft(104).reduced(0, 1));
    autoRow.removeFromLeft(10);
    autoStatus.setBounds(autoRow);
    area.removeFromTop(6);
    view.setBounds(area);
}

void SongMatchPage::refresh()
{
    // The switch is a parameter, so a project recall or a second editor can move it. Learned by
    // looking rather than assumed, the same way the voicing boxes on the other pages are.
    const auto autoEnabled = processor.autoMatchEnabled();
    if (autoMatch.getToggleState() != autoEnabled)
        autoMatch.setToggleState(autoEnabled, juce::dontSendNotification);
    const auto trackingEnabled = processor.autoMatchTracking();
    if (tracking.getToggleState() != trackingEnabled)
        tracking.setToggleState(trackingEnabled, juce::dontSendNotification);
    // Tracking without a rig to bound it has nothing to follow, so the control is only offered
    // once Auto Match itself is on.
    tracking.setEnabled(autoEnabled);
    const auto state = processor.autoMatchState();
    reclaim.setVisible(state == tf::automatch::State::overridden);
    restoreWarnings.setVisible(processor.autoMatchWarningsSuppressed());
    autoStatus.setText(processor.autoMatchStatusText(), juce::dontSendNotification);

    const auto reconstruction = processor.reconstructionSnapshot();
    const auto playableRegions = processor.reconstructionRegionsSnapshot();
    juce::StringArray regionLabels;
    for (const auto& playable : playableRegions)
    {
        const auto start = static_cast<int>(std::lround(playable.quality.startSeconds));
        const auto end = static_cast<int>(std::lround(playable.quality.endSeconds));
        const auto time = juce::String::formatted("%02d:%02d-%02d:%02d",
            start / 60, start % 60, end / 60, end % 60);
        const auto character = juce::String(
            nts::reconstruction::toString(playable.gainCharacter).data()).toUpperCase();
        const auto cents = juce::String(playable.tuningOffsetCents >= 0.0f ? "+" : "")
            + juce::String(juce::roundToInt(playable.tuningOffsetCents)) + "c";
        regionLabels.add(time + " | " + character + " | " + playable.dominantPitch
            + " | " + playable.estimatedTuning + " " + cents + " | "
            + juce::String(juce::roundToInt(playable.quality.confidence * 100.0f)) + "%");
    }
    // Rebuilding the list every tick would fight the user's own selection, so it only happens
    // when the labels actually change.
    const auto signature = regionLabels.joinIntoString("\n");
    if (signature != regionSignature)
    {
        regionSignature = signature;
        region.clear(juce::dontSendNotification);
        region.addItemList(regionLabels, 1);
        auto selectedId = playableRegions.empty() ? 0 : 1;
        if (reconstruction)
            for (std::size_t index = 0; index < playableRegions.size(); ++index)
                if (std::abs(playableRegions[index].quality.startSeconds
                             - reconstruction->reference.regionStartSeconds) < 0.1)
                    selectedId = static_cast<int>(index + 1);
        region.setSelectedId(selectedId, juce::dontSendNotification);
    }

    view.setResult(reconstruction, processor.reconstructionStatusText(),
                   processor.reconstructionProgress(), processor.reconstructionApplyWarnings());
    const auto candidateCount = reconstruction ? static_cast<int>(reconstruction->candidates.size()) : 0;
    if (candidate.getNumItems() != candidateCount)
    {
        candidate.clear(juce::dontSendNotification);
        for (int index = 0; index < candidateCount; ++index)
            candidate.addItem("Candidate " + juce::String(index + 1), index + 1);
        if (candidateCount > 0) candidate.setSelectedId(1, juce::dontSendNotification);
    }
}

void SongMatchPage::chooseSong()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose a user-provided song", juce::File {}, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<SongMatchPage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto instrument = safeThis->target.getSelectedId() == 2
                ? nts::reconstruction::TargetInstrument::bass
                : nts::reconstruction::TargetInstrument::guitar;
            const auto modeIndex = std::clamp(safeThis->stereoMode.getSelectedId() - 1, 0, 5);
            constexpr std::array modes { nts::reconstruction::StereoMode::fullStereo,
                nts::reconstruction::StereoMode::left, nts::reconstruction::StereoMode::right,
                nts::reconstruction::StereoMode::mid, nts::reconstruction::StereoMode::side,
                nts::reconstruction::StereoMode::pannedEstimate };
            safeThis->processor.requestSongReconstruction(chooser.getResult(), instrument,
                modes[static_cast<std::size_t>(modeIndex)]);
        });
}

void SongMatchPage::chooseExportDestination()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Export reconstruction profile without source audio", juce::File {}, "*.tfreconstruct");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis = juce::Component::SafePointer<SongMatchPage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.exportReconstruction(
                chooser.getResult().withFileExtension("tfreconstruct"));
            if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Export failed", result.getErrorMessage());
        });
}
