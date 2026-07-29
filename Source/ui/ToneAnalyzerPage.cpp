#include "ToneAnalyzerPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <algorithm>

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
    graphics.setColour(theme::good);
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
        graphics.setColour(confidenceValues[index] > 0.65f ? theme::good : theme::warn);
        graphics.fillRoundedRectangle(cell.reduced(1.5f).withTop(
            cell.getBottom() - 1.5f - (cell.getHeight() - 3.0f) * confidenceValues[index]), 2.0f);
        graphics.setColour(theme::textTertiary);
        graphics.setFont(theme::font(8.0f, true));
        graphics.drawText(confidenceNames[index], label, juce::Justification::centred);
    }
    featurePanel.removeFromTop(8.0f);
    juce::StringArray warningValues;
    for (const auto& warning : analysis.report.warnings) warningValues.add(warning);
    graphics.setColour(warningValues.isEmpty() ? theme::good : theme::warn);
    graphics.setFont(theme::font(10.0f));
    graphics.drawFittedText(warningValues.isEmpty() ? "No quality warnings"
                                                    : warningValues.joinIntoString("   /   "),
                            featurePanel.toNearestInt(), juce::Justification::topLeft, 4);
}

ToneAnalyzerPage::ToneAnalyzerPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    addAndMakeVisible(analyzeClip);
    addAndMakeVisible(view);
    analyzeClip.onClick = [this] { chooseClip(); };
}

void ToneAnalyzerPage::resized()
{
    auto area = getLocalBounds();
    analyzeClip.setBounds(area.removeFromTop(34).removeFromLeft(160));
    area.removeFromTop(10);
    view.setBounds(area);
}

void ToneAnalyzerPage::refresh()
{
    view.setResult(processor.toneAnalysisSnapshot(), processor.toneAnalysisStatusText(),
                   processor.toneNearestProfileText(), processor.toneProfileCount());
}

void ToneAnalyzerPage::chooseClip()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose an isolated guitar or bass clip", juce::File {}, "*.wav;*.aif;*.aiff;*.flac;*.ogg");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<ToneAnalyzerPage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestToneAnalysis(chooser.getResult());
        });
}
