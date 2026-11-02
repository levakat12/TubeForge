#include "ToneMatchPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>
#include <cmath>

namespace theme = tf::theme;

void ToneAnalysisView::setResult(std::optional<nts::tone::ToneAnalysisResult> analysis,
                                 juce::String status, juce::String nearest, std::size_t profileCount)
{
    result = std::move(analysis); statusText = std::move(status); nearestText = std::move(nearest);
    profiles = profileCount; repaint();
}

void ToneAnalysisView::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    theme::glass(graphics, area, 9.0f);
    area = area.reduced(14.0f, 12.0f);

    auto header = area.removeFromTop(30.0f);
    graphics.setColour(theme::textTertiary);
    graphics.setFont(theme::font(10.0f));
    graphics.drawText(juce::String(profiles) + " profiles this session",
                      header.removeFromRight(180.0f), juce::Justification::centredRight);
    graphics.setColour(theme::accent);
    graphics.setFont(theme::font(14.0f, true));
    graphics.drawFittedText(statusText, header.toNearestInt(), juce::Justification::centredLeft, 1);

    if (!result || !result->success)
    {
        graphics.setColour(theme::textTertiary);
        graphics.setFont(theme::font(13.0f));
        graphics.drawFittedText("Load an isolated guitar or bass clip to see its tonal fingerprint.\n\n"
                                "Plugin renders, paired captures and amp recordings all work. Analysis runs "
                                "offline on up to 60 seconds and never touches the playback thread.",
                                area.reduced(40.0f).toNearestInt(), juce::Justification::centred, 5);
        return;
    }

    const auto& analysis = *result;
    auto summary = area.removeFromTop(34.0f);
    graphics.setColour(theme::textPrimary);
    graphics.setFont(theme::font(12.5f, true));
    const auto context = juce::String(nts::tone::toString(analysis.report.context.instrument).data()).toUpperCase()
        + "   /   " + juce::String(nts::tone::toString(analysis.report.context.technique).data()).toUpperCase()
        + "   /   " + juce::String(nts::tone::toString(analysis.report.context.gainCategory).data()).toUpperCase();
    theme::tracked(graphics, context, summary.removeFromLeft(summary.getWidth() * 0.62f).toNearestInt(),
                   juce::Justification::centredLeft);
    graphics.setColour(theme::textSecondary);
    graphics.drawText("CONFIDENCE " + juce::String(juce::roundToInt(analysis.report.confidence.aggregate * 100.0f)) + "%",
                      summary, juce::Justification::centredRight);

    auto lower = area.reduced(0.0f, 6.0f);
    auto descriptors = lower.removeFromLeft(lower.getWidth() * 0.54f);
    lower.removeFromLeft(20.0f);
    const std::array descriptorValues {
        std::pair { "GAIN", analysis.report.gain }, std::pair { "BRIGHTNESS", analysis.report.brightness },
        std::pair { "TIGHTNESS", analysis.report.tightness }, std::pair { "COMPRESSION", analysis.report.compression },
        std::pair { "CLEAN LOW BLEND", analysis.report.cleanLowBlendEstimate },
        std::pair { "CABINET DARKNESS", analysis.report.cabinetDarkness },
        std::pair { "ROOM AMOUNT", analysis.report.roomAmount }
    };
    const auto rowHeight = std::max(28.0f, descriptors.getHeight() / static_cast<float>(descriptorValues.size() + 1));
    for (const auto& [name, descriptor] : descriptorValues)
    {
        auto row = descriptors.removeFromTop(rowHeight);
        auto label = row.removeFromLeft(122.0f);
        graphics.setColour(theme::textSecondary);
        graphics.setFont(theme::font(9.0f, true));
        theme::tracked(graphics, name, label.toNearestInt(), juce::Justification::centredLeft);
        auto bar = juce::Rectangle<float>(row.getX(), row.getCentreY() - 4.0f, row.getWidth() - 34.0f, 8.0f);
        theme::well(graphics, bar, 4.0f);
        graphics.setColour(theme::accent);
        graphics.fillRoundedRectangle(bar.reduced(1.0f).withWidth(std::max(2.0f, (bar.getWidth() - 2.0f) * descriptor.value)), 3.0f);
        const auto uncertaintyWidth = bar.getWidth() * descriptor.uncertainty * 0.5f;
        const auto centre = bar.getX() + bar.getWidth() * descriptor.value;
        graphics.setColour(theme::accent.brighter(0.4f).withAlpha(0.30f));
        graphics.fillRect(juce::Rectangle<float>(centre - uncertaintyWidth, bar.getY(),
                                                 uncertaintyWidth * 2.0f, bar.getHeight()).getIntersection(bar));
        graphics.setColour(theme::textSecondary);
        graphics.setFont(theme::font(9.5f));
        graphics.drawText(juce::String(juce::roundToInt(descriptor.value * 100.0f)), row, juce::Justification::centredRight);
    }
    graphics.setColour(theme::textTertiary);
    graphics.setFont(theme::font(10.0f));
    graphics.drawFittedText(nearestText, descriptors.toNearestInt(), juce::Justification::bottomLeft, 2);

    auto featurePanel = lower;
    auto spectralGraph = featurePanel.removeFromTop(featurePanel.getHeight() * 0.44f);
    theme::well(graphics, spectralGraph, 6.0f);
    const std::array spectralValues { analysis.features.spectral.lowFrequencyExtension,
        analysis.features.spectral.lowMidBuildup, analysis.features.spectral.midEmphasis,
        analysis.features.spectral.upperMidAttack, 1.0f - analysis.features.spectral.highFrequencyRolloff,
        analysis.features.spectral.resonantPeakStrength, analysis.features.spectral.spectralFlatness };
    juce::Path curve;
    auto plot = spectralGraph.reduced(14.0f, 22.0f);
    for (std::size_t index = 0; index < spectralValues.size(); ++index)
    {
        const auto x = plot.getX() + plot.getWidth() * static_cast<float>(index)
            / static_cast<float>(spectralValues.size() - 1);
        const auto y = plot.getBottom() - plot.getHeight() * std::clamp(spectralValues[index], 0.0f, 1.0f);
        if (index == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
    }
    graphics.setColour(theme::accent.withAlpha(0.25f));
    graphics.strokePath(curve, juce::PathStrokeType(5.0f));
    graphics.setColour(theme::accent);
    graphics.strokePath(curve, juce::PathStrokeType(1.8f));
    theme::caption(graphics, spectralGraph.reduced(10.0f, 6.0f).removeFromTop(14.0f).toNearestInt(),
                   "Tone profile");

    featurePanel.removeFromTop(10.0f);
    auto confidence = featurePanel.removeFromTop(70.0f);
    constexpr std::array confidenceNames { "S/L", "CLASS", "TIME", "BANDS", "POLY", "SEP", "DOMAIN" };
    const std::array confidenceValues { analysis.report.confidence.signalToLeakage,
        analysis.report.confidence.classifierCertainty, analysis.report.confidence.duration,
        analysis.report.confidence.spectralCoverage, analysis.report.confidence.polyphony,
        analysis.report.confidence.separationQuality, analysis.report.confidence.modelDomainProximity };
    const auto confidenceWidth = confidence.getWidth() / static_cast<float>(confidenceNames.size());
    for (std::size_t index = 0; index < confidenceNames.size(); ++index)
    {
        auto cell = juce::Rectangle<float>(confidence.getX() + confidenceWidth * static_cast<float>(index),
                                            confidence.getY(), confidenceWidth - 4.0f, confidence.getHeight());
        auto label = cell.removeFromBottom(15.0f);
        theme::well(graphics, cell, 3.0f);
        graphics.setColour(confidenceValues[index] > 0.65f ? theme::accent : theme::warn);
        graphics.fillRoundedRectangle(cell.reduced(1.5f).withTop(
            cell.getBottom() - 1.5f - (cell.getHeight() - 3.0f) * confidenceValues[index]), 2.0f);
        graphics.setColour(theme::textTertiary);
        graphics.setFont(theme::font(8.0f, true));
        graphics.drawText(confidenceNames[index], label, juce::Justification::centred);
    }
    featurePanel.removeFromTop(8.0f);
    juce::StringArray warningValues;
    for (const auto& warning : analysis.report.warnings) warningValues.add(warning);
    graphics.setColour(warningValues.isEmpty() ? theme::textTertiary : theme::warn);
    graphics.setFont(theme::font(10.0f));
    graphics.drawFittedText(warningValues.isEmpty() ? "No quality warnings"
                                                    : warningValues.joinIntoString("   /   "),
                            featurePanel.toNearestInt(), juce::Justification::topLeft, 4);
}

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
    graphics.drawFittedText(juce::String(std::string(nts::reconstruction::toString(reconstruction.reference.gainCharacter))).toUpperCase()
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

ToneMatchPage::ToneMatchPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    source.addItemList({ "A song to rebuild", "A clip to measure" }, 1);
    source.setSelectedId(1, juce::dontSendNotification);
    source.setTooltip("A song is separated into stems, cut into playable parts and searched for "
                      "rigs you can apply. A clip is measured and described, and nothing is "
                      "applied. Both run offline and never touch the playback thread.");

    tf::ui::populateFromParameter(target, processor.getParameters(), "instrument");
    target.setSelectedId(1, juce::dontSendNotification);
    stereoMode.addItemList({ "Full stereo", "Left", "Right", "Mid", "Side", "Panned estimate" }, 1);
    stereoMode.setSelectedId(1, juce::dontSendNotification);
    region.setTextWhenNoChoicesAvailable("Import a song first");
    region.setTextWhenNothingSelected("Import a song first");
    candidate.setTextWhenNoChoicesAvailable("No candidates yet");
    candidate.setTextWhenNothingSelected("No candidates yet");
    tf::ui::configureFieldCaption(captions[0], "Start from");
    tf::ui::configureFieldCaption(captions[1], "Instrument");
    tf::ui::configureFieldCaption(captions[2], "Listen to");
    tf::ui::configureFieldCaption(captions[3], "Part of the song");
    tf::ui::configureFieldCaption(captions[4], "Rig candidate");
    tf::ui::configureFieldCaption(chainCaption, "Effects on apply");
    chainMode.addItemList({ "Bypass them", "Leave mine alone", "Match the record" }, 1);
    chainMode.setSelectedId(1, juce::dontSendNotification);
    chainMode.setTooltip("Bypass them: the four pedal slots are bypassed and the delay and reverb "
                         "sends are closed, so you hear the candidate that was actually ranked -- "
                         "none of those were in the offline render it was compared against. Slots "
                         "keep their settings and their loaded models; the sends have no bypass, "
                         "so their mix is turned down instead.\n\n"
                         "Leave mine alone: your board and sends are untouched.\n\n"
                         "Match the record: the level, room, delay, width and compression measured "
                         "from the song are written onto those same controls, so you can hear and "
                         "undo every one of them. This is an estimate, not a recovery -- and if "
                         "the part you matched already carries its own room, you will be applying "
                         "a second one on top of it.");
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

    addAndMakeVisible(source);
    addAndMakeVisible(captions[0]);
    // Everything else is added hidden and shown by applySource: which half of the page is
    // live is one decision, made in one place, rather than a default that has to agree with it.
    for (auto* component : std::initializer_list<juce::Component*> { &importSong, &cancel,
             &applyCandidate, &exportCandidate, &target, &stereoMode, &region, &useRegion,
             &candidate, &captions[1], &captions[2], &captions[3], &captions[4], &chainMode,
             &chainCaption, &autoMatch, &tracking, &autoStatus, &matchView, &analyzeClip,
             &analysisView, &reclaim, &restoreWarnings })
        addChildComponent(*component);

    source.onChange = [this] { applySource(); };
    analyzeClip.onClick = [this] { chooseClip(); };
    autoMatch.onClick = [this] { processor.setAutoMatchEnabled(autoMatch.getToggleState()); };
    reclaim.onClick = [this] { processor.reclaimAllAutoMatchParameters(); };
    tracking.onClick = [this] { processor.setAutoMatchTracking(tracking.getToggleState()); };
    restoreWarnings.onClick = [this] { processor.setAutoMatchWarningsSuppressed(false); };
    // Pushed rather than read back, so an automatic apply does exactly what pressing Apply with
    // the same choice set would have done. Sent once now for the default and on every change.
    processor.setAutoMatchChainMode(selectedChainMode());
    chainMode.onChange = [this] { processor.setAutoMatchChainMode(selectedChainMode()); };

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
                                                      selectedChainMode());
    };
    exportCandidate.onClick = [this] { chooseExportDestination(); };

    applySource();
}

ChainMode ToneMatchPage::selectedChainMode() const noexcept
{
    switch (chainMode.getSelectedId())
    {
        case 2: return ChainMode::leaveAlone;
        case 3: return ChainMode::matchRecord;
        // Including nothing selected, which is the state before the page has been shown: the
        // default is the setting that plays back the candidate that was ranked.
        default: return ChainMode::bypass;
    }
}

void ToneMatchPage::applySource()
{
    const auto song = currentSource() == Source::song;
    for (auto* component : std::initializer_list<juce::Component*> { &importSong, &cancel,
             &applyCandidate, &exportCandidate, &target, &stereoMode, &region, &useRegion,
             &candidate, &captions[1], &captions[2], &captions[3], &captions[4], &chainMode,
             &chainCaption, &autoMatch, &tracking, &autoStatus, &matchView })
        component->setVisible(song);
    analyzeClip.setVisible(! song);
    analysisView.setVisible(! song);
    // The two conditional buttons stay conditional: refresh owns them, and it only runs while
    // the song half is showing, so they are cleared here rather than left in whatever state the
    // last tick put them in.
    if (! song) { reclaim.setVisible(false); restoreWarnings.setVisible(false); }
    resized();
    // The view for the half just switched to has not been filled since it was last visible.
    refresh();
}

void ToneMatchPage::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(44);
    tf::ui::layOutField(top.removeFromLeft(160), captions[0], source);
    top.removeFromLeft(12);

    if (currentSource() == Source::clip)
    {
        analyzeClip.setBounds(top.removeFromLeft(160).withTrimmedTop(13).withHeight(28));
        area.removeFromTop(10);
        analysisView.setBounds(area);
        return;
    }

    importSong.setBounds(top.removeFromLeft(126).withTrimmedTop(13).withHeight(28));
    top.removeFromLeft(8);
    cancel.setBounds(top.removeFromLeft(84).withTrimmedTop(13).withHeight(28));
    top.removeFromLeft(12);
    tf::ui::layOutField(top.removeFromLeft(120), captions[1], target);
    top.removeFromLeft(12);
    tf::ui::layOutField(top.removeFromLeft(160), captions[2], stereoMode);
    top.removeFromLeft(16);
    tf::ui::layOutField(top.removeFromLeft(180), chainCaption, chainMode);
    area.removeFromTop(8);

    auto regionRow = area.removeFromTop(44);
    const auto rowWidth = regionRow.getWidth();
    tf::ui::layOutField(regionRow.removeFromLeft(rowWidth * 40 / 100), captions[3], region);
    regionRow.removeFromLeft(8);
    useRegion.setBounds(regionRow.removeFromLeft(112).withTrimmedTop(13).withHeight(28));
    regionRow.removeFromLeft(12);
    tf::ui::layOutField(regionRow.removeFromLeft(150), captions[4], candidate);
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
    matchView.setBounds(area);
}

void ToneMatchPage::refresh()
{
    if (currentSource() == Source::clip)
    {
        analysisView.setResult(processor.toneAnalysisSnapshot(), processor.toneAnalysisStatusText(),
                               processor.toneNearestProfileText(), processor.toneProfileCount());
        return;
    }

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

    matchView.setResult(reconstruction, processor.reconstructionStatusText(),
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

void ToneMatchPage::chooseSong()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose a user-provided song", juce::File {}, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<ToneMatchPage>(this)](const juce::FileChooser& chooser)
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

void ToneMatchPage::chooseClip()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose an isolated guitar or bass clip", juce::File {}, "*.wav;*.aif;*.aiff;*.flac;*.ogg");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<ToneMatchPage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestToneAnalysis(chooser.getResult());
        });
}

void ToneMatchPage::chooseExportDestination()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Export reconstruction profile without source audio", juce::File {}, "*.tfreconstruct");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis = juce::Component::SafePointer<ToneMatchPage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.exportReconstruction(
                chooser.getResult().withFileExtension("tfreconstruct"));
            if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Export failed", result.getErrorMessage());
        });
}
