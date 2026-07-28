#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <nts/circuit/Components.h>

#include <algorithm>
#include <cmath>

namespace
{
constexpr std::array simpleIds { "gain", "bass", "mid", "treble", "presence", "resonance", "master" };
constexpr std::array simpleNames { "Gain", "Bass", "Mid", "Treble", "Presence", "Resonance", "Master" };
constexpr std::array advancedIds { "stage1", "stage2", "stage3", "stage4", "bias", "lowCut", "highCut",
                                   "sag", "feedback", "crossover", "cleanBlend", "cabinetAlignment",
                                   "tightness", "pickEmphasis" };
constexpr std::array advancedNames { "Stage 1 gain", "Stage 2 gain", "Stage 3 gain", "Stage 4 gain",
                                     "Bias", "Pre low cut", "Pre high cut", "Sag", "Feedback",
                                     "Bass crossover", "Clean blend", "Cab alignment", "Tightness",
                                     "Pick emphasis" };

juce::String statusName(nts::diagnostics::AssetLoadStatus status)
{
    switch (status)
    {
        case nts::diagnostics::AssetLoadStatus::unavailable: return "N/A";
        case nts::diagnostics::AssetLoadStatus::idle: return "Idle";
        case nts::diagnostics::AssetLoadStatus::loading: return "Loading";
        case nts::diagnostics::AssetLoadStatus::ready: return "Ready";
        case nts::diagnostics::AssetLoadStatus::failed: return "Failed";
    }
    return "Unknown";
}
} // namespace

void CircuitSchematicView::setSnapshot(std::vector<nts::circuit::NodeTelemetry> telemetry, juce::String status,
                                       nts::circuit::CircuitGraphDescription graph)
{
    stages = std::move(telemetry);
    statusText = std::move(status);
    circuit = std::move(graph);
    repaint();
}

void CircuitSchematicView::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat().reduced(14.0f);
    graphics.setColour(juce::Colour::fromRGB(15, 18, 23));
    graphics.fillRoundedRectangle(bounds, 8.0f);
    graphics.setColour(juce::Colour::fromRGB(235, 175, 83));
    graphics.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    graphics.drawText("EDITABLE PHYSICAL CIRCUIT", bounds.removeFromTop(28.0f), juce::Justification::centredLeft);

    auto schematic = bounds.removeFromTop(190.0f).reduced(4.0f);
    const auto nodeCount = std::max<std::size_t>(1, circuit.nodes.size());
    const auto cellWidth = schematic.getWidth() / static_cast<float>(nodeCount);
    for (std::size_t index = 0; index < circuit.nodes.size(); ++index)
    {
        const auto& node = circuit.nodes[index];
        const auto centreX = schematic.getX() + cellWidth * (static_cast<float>(index) + 0.5f);
        const auto box = juce::Rectangle<float>(cellWidth - 8.0f, 50.0f)
            .withCentre(juce::Point<float> { centreX, schematic.getY() + 48.0f });
        if (index + 1 < circuit.nodes.size())
        {
            graphics.setColour(juce::Colour::fromRGB(114, 92, 63));
            graphics.drawArrow({ box.getRight(), box.getCentreY(), box.getRight() + 8.0f, box.getCentreY() }, 1.2f, 5.0f, 5.0f);
        }
        graphics.setColour(juce::Colour::fromRGB(39, 44, 53)); graphics.fillRoundedRectangle(box, 5.0f);
        graphics.setColour(juce::Colour::fromRGB(207, 135, 61)); graphics.drawRoundedRectangle(box, 5.0f, 1.3f);
        graphics.setColour(juce::Colour::fromRGB(239, 222, 194)); graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        auto nodeName = juce::String(nts::circuit::toString(node.type).data()).toUpperCase();
        nodeName = nodeName.replace("STAGE", "").substring(0, 9);
        graphics.drawFittedText(nodeName, box.toNearestInt(), juce::Justification::centred, 1);
        const auto telemetry = std::find_if(stages.begin(), stages.end(), [&node](const auto& stage)
        { return stage.nodeId == node.id; });
        if (telemetry != stages.end())
        {
            const auto& meter = telemetry->values;
            const auto text = juce::String(meter.outputRms, 3) + " rms\n"
                            + (meter.valid ? "VALID" : "CHECK");
            graphics.setColour(meter.valid ? juce::Colours::lightgreen : juce::Colours::orange);
            graphics.setFont(juce::FontOptions(9.5f));
            graphics.drawFittedText(text, box.translated(0.0f, 54.0f).withHeight(34.0f).toNearestInt(),
                                    juce::Justification::centred, 2);
        }
    }
    graphics.setColour(juce::Colour::fromRGB(174, 181, 193));
    graphics.setFont(juce::FontOptions(11.5f));
    const auto triode = std::find_if(circuit.nodes.begin(), circuit.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    const auto power = std::find_if(circuit.nodes.begin(), circuit.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::powerStage; });
    const auto tone = std::find_if(circuit.nodes.begin(), circuit.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::toneStack; });
    const auto details = juce::String(triode == circuit.nodes.end() ? "No preamp" : triode->modelId)
        + "  |  " + (power == circuit.nodes.end() ? "No power stage" : juce::String(power->modelId))
        + "  |  " + (tone == circuit.nodes.end() ? "No tone stack" : juce::String(tone->modelId));
    graphics.drawFittedText(details, schematic.removeFromBottom(28.0f).toNearestInt(), juce::Justification::centred, 1);

    auto graphArea = bounds.removeFromTop(128.0f).reduced(12.0f, 4.0f);
    graphics.setColour(juce::Colour::fromRGB(58, 64, 74));
    graphics.drawRect(graphArea, 1.0f);
    juce::Path response;
    for (int pixel = 0; pixel < static_cast<int>(graphArea.getWidth()); ++pixel)
    {
        const auto normalized = static_cast<float>(pixel) / std::max(1.0f, graphArea.getWidth() - 1.0f);
        const auto x = graphArea.getX() + static_cast<float>(pixel);
        const auto frequency = 20.0 * std::pow(1000.0, static_cast<double>(normalized));
        const auto magnitude = tone == circuit.nodes.end() ? 1.0
            : nts::circuit::toneStackMagnitude(*tone, frequency, 48000.0);
        const auto decibels = std::clamp(20.0 * std::log10(std::max(1.0e-6, magnitude)), -30.0, 10.0);
        const auto y = graphArea.getBottom() - graphArea.getHeight() * static_cast<float>((decibels + 30.0) / 40.0);
        if (pixel == 0) response.startNewSubPath(x, y); else response.lineTo(x, y);
    }
    graphics.setColour(juce::Colour::fromRGB(234, 151, 65)); graphics.strokePath(response, juce::PathStrokeType(2.0f));
    graphics.setColour(juce::Colour::fromRGB(154, 162, 174)); graphics.setFont(juce::FontOptions(10.0f));
    graphics.drawText("20 Hz", graphArea.withHeight(18.0f), juce::Justification::bottomLeft);
    graphics.drawText("20 kHz", graphArea.withHeight(18.0f), juce::Justification::bottomRight);

    graphics.setColour(statusText.containsIgnoreCase("failed") || statusText.containsIgnoreCase("invalid")
                           ? juce::Colours::orange : juce::Colours::lightgreen);
    graphics.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    graphics.drawFittedText(statusText, bounds.toNearestInt(), juce::Justification::centredLeft, 2);
}

void StudioSignalChain::setEngineMode(int mode)
{
    if (activeEngineMode == mode) return;
    activeEngineMode = mode;
    repaint();
}

void StudioSignalChain::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(10.0f, 8.0f);
    graphics.setColour(juce::Colour::fromRGB(12, 14, 17));
    graphics.fillRoundedRectangle(area, 7.0f);
    graphics.setColour(juce::Colour::fromRGB(43, 48, 56));
    graphics.drawRoundedRectangle(area, 7.0f, 1.0f);

    constexpr std::array moduleNames { "INPUT", "GATE / EQ", "PREAMP", "TONE", "POWER", "CABINET", "OUTPUT" };
    constexpr std::array moduleNumbers { "01", "02", "03", "04", "05", "06", "07" };
    auto modules = area.reduced(12.0f, 7.0f);
    const auto gap = 8.0f;
    const auto width = (modules.getWidth() - gap * static_cast<float>(moduleNames.size() - 1))
                     / static_cast<float>(moduleNames.size());
    for (std::size_t index = 0; index < moduleNames.size(); ++index)
    {
        auto card = juce::Rectangle<float>(modules.getX() + static_cast<float>(index) * (width + gap),
                                            modules.getY(), width, modules.getHeight());
        const auto isCore = index >= 2 && index <= 4;
        auto colour = isCore ? juce::Colour::fromRGB(68, 45, 30) : juce::Colour::fromRGB(31, 35, 41);
        if (activeEngineMode == 1 && isCore) colour = juce::Colour::fromRGB(35, 54, 65);
        if (activeEngineMode == 2 && isCore) colour = juce::Colour::fromRGB(55, 45, 68);
        graphics.setColour(colour); graphics.fillRoundedRectangle(card, 5.0f);
        graphics.setColour(isCore ? juce::Colour::fromRGB(227, 132, 52) : juce::Colour::fromRGB(75, 82, 93));
        graphics.drawRoundedRectangle(card, 5.0f, isCore ? 1.5f : 1.0f);
        graphics.setColour(juce::Colour::fromRGB(132, 139, 150));
        graphics.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        graphics.drawText(moduleNumbers[index], card.removeFromTop(14.0f).reduced(5.0f, 0.0f), juce::Justification::topLeft);
        graphics.setColour(juce::Colour::fromRGB(231, 232, 234));
        graphics.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        graphics.drawFittedText(moduleNames[index], card.toNearestInt(), juce::Justification::centred, 1);
        graphics.setColour(isCore ? juce::Colour::fromRGB(245, 147, 61) : juce::Colour::fromRGB(78, 188, 116));
        graphics.fillEllipse(card.getRight() - 10.0f, card.getY() + 5.0f, 4.0f, 4.0f);
    }
}

void StudioLevelMeter::setLevels(float input, float output)
{
    inputLevel = std::clamp(input, 0.0f, 1.0f);
    outputLevel = std::clamp(output, 0.0f, 1.0f);
    repaint();
}

void StudioLevelMeter::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(10.0f);
    graphics.setColour(juce::Colour::fromRGB(12, 14, 17));
    graphics.fillRoundedRectangle(area, 7.0f);
    graphics.setColour(juce::Colour::fromRGB(43, 48, 56));
    graphics.drawRoundedRectangle(area, 7.0f, 1.0f);
    auto heading = area.removeFromTop(24.0f);
    graphics.setColour(juce::Colour::fromRGB(168, 174, 184));
    graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    graphics.drawText("LEVELS", heading, juce::Justification::centred);

    const auto drawMeter = [&graphics](juce::Rectangle<float> meter, float value, const juce::String& name)
    {
        auto label = meter.removeFromBottom(20.0f);
        graphics.setColour(juce::Colour::fromRGB(28, 31, 36)); graphics.fillRoundedRectangle(meter, 3.0f);
        const auto fill = meter.withTop(meter.getBottom() - meter.getHeight() * value);
        juce::ColourGradient gradient(juce::Colour::fromRGB(54, 205, 112), fill.getBottomLeft(),
                                      juce::Colour::fromRGB(245, 173, 61), fill.getTopLeft(), false);
        gradient.addColour(0.86, juce::Colour::fromRGB(235, 210, 67));
        gradient.addColour(1.0, juce::Colour::fromRGB(232, 73, 60));
        graphics.setGradientFill(gradient); graphics.fillRoundedRectangle(fill, 3.0f);
        graphics.setColour(juce::Colour::fromRGB(74, 80, 90)); graphics.drawRoundedRectangle(meter, 3.0f, 1.0f);
        graphics.setColour(juce::Colour::fromRGB(184, 190, 199)); graphics.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        graphics.drawText(name, label, juce::Justification::centred);
    };
    auto meters = area.reduced(10.0f, 6.0f);
    const auto meterWidth = 18.0f;
    drawMeter(juce::Rectangle<float>(meterWidth, meters.getHeight()).withCentre(
                  { meters.getCentreX() - 16.0f, meters.getCentreY() }), inputLevel, "IN");
    drawMeter(juce::Rectangle<float>(meterWidth, meters.getHeight()).withCentre(
                  { meters.getCentreX() + 16.0f, meters.getCentreY() }), outputLevel, "OUT");
}

void ToneAnalysisView::setResult(std::optional<nts::tone::ToneAnalysisResult> analysis,
                                 juce::String status, juce::String nearest, std::size_t profileCount)
{
    result = std::move(analysis); statusText = std::move(status); nearestText = std::move(nearest);
    profiles = profileCount; repaint();
}

void ToneAnalysisView::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(12.0f);
    graphics.setColour(juce::Colour::fromRGB(13, 16, 19)); graphics.fillRoundedRectangle(area, 7.0f);
    graphics.setColour(juce::Colour::fromRGB(48, 54, 62)); graphics.drawRoundedRectangle(area, 7.0f, 1.0f);
    auto header = area.removeFromTop(48.0f).reduced(12.0f, 4.0f);
    graphics.setColour(juce::Colour::fromRGB(240, 144, 58)); graphics.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    graphics.drawFittedText(statusText, header.toNearestInt(), juce::Justification::centredLeft, 1);
    graphics.setColour(juce::Colour::fromRGB(126, 134, 146)); graphics.setFont(juce::FontOptions(10.0f));
    graphics.drawText("Session profiles: " + juce::String(profiles), header.removeFromRight(120.0f), juce::Justification::centredRight);
    if (!result || !result->success)
    {
        graphics.setColour(juce::Colour::fromRGB(109, 117, 130));
        graphics.setFont(juce::FontOptions(14.0f));
        graphics.drawFittedText("Load an isolated guitar/bass clip, plugin render, paired capture, or amp recording.\nAnalysis is limited to 60 seconds and never runs in the playback callback.",
                                area.reduced(30.0f).toNearestInt(), juce::Justification::centred, 3);
        return;
    }

    const auto& analysis = *result;
    auto summary = area.removeFromTop(42.0f).reduced(12.0f, 2.0f);
    graphics.setColour(juce::Colour::fromRGB(231, 233, 236)); graphics.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    const auto context = juce::String(nts::tone::toString(analysis.report.context.instrument).data()).toUpperCase()
        + "  /  " + juce::String(nts::tone::toString(analysis.report.context.technique).data()).toUpperCase()
        + "  /  " + juce::String(nts::tone::toString(analysis.report.context.gainCategory).data()).toUpperCase();
    graphics.drawText(context, summary.removeFromLeft(summary.getWidth() * 0.62f), juce::Justification::centredLeft);
    graphics.setColour(juce::Colour::fromRGB(87, 205, 127));
    graphics.drawText("CONFIDENCE " + juce::String(analysis.report.confidence.aggregate * 100.0f, 0) + "%",
                      summary, juce::Justification::centredRight);

    auto lower = area.reduced(12.0f, 8.0f);
    auto descriptors = lower.removeFromLeft(lower.getWidth() * 0.54f);
    lower.removeFromLeft(18.0f);
    const std::array descriptorValues {
        std::pair { "GAIN", analysis.report.gain }, std::pair { "BRIGHTNESS", analysis.report.brightness },
        std::pair { "TIGHTNESS", analysis.report.tightness }, std::pair { "COMPRESSION", analysis.report.compression },
        std::pair { "CLEAN LOW BLEND", analysis.report.cleanLowBlendEstimate },
        std::pair { "CABINET DARKNESS", analysis.report.cabinetDarkness },
        std::pair { "ROOM AMOUNT", analysis.report.roomAmount }
    };
    const auto rowHeight = std::max(29.0f, descriptors.getHeight() / static_cast<float>(descriptorValues.size() + 1));
    for (const auto& [name, descriptor] : descriptorValues)
    {
        auto row = descriptors.removeFromTop(rowHeight);
        auto label = row.removeFromLeft(116.0f);
        graphics.setColour(juce::Colour::fromRGB(161, 168, 178)); graphics.setFont(juce::FontOptions(9.5f, juce::Font::bold));
        graphics.drawText(name, label, juce::Justification::centredLeft);
        auto bar = row.reduced(4.0f, rowHeight * 0.31f);
        graphics.setColour(juce::Colour::fromRGB(31, 35, 41)); graphics.fillRoundedRectangle(bar, 3.0f);
        graphics.setColour(juce::Colour::fromRGB(226, 127, 47));
        graphics.fillRoundedRectangle(bar.withWidth(bar.getWidth() * descriptor.value), 3.0f);
        const auto uncertaintyWidth = bar.getWidth() * descriptor.uncertainty * 0.5f;
        const auto centre = bar.getX() + bar.getWidth() * descriptor.value;
        graphics.setColour(juce::Colour::fromRGB(245, 185, 91).withAlpha(0.35f));
        graphics.fillRect(juce::Rectangle<float>(centre - uncertaintyWidth, bar.getY(), uncertaintyWidth * 2.0f, bar.getHeight())
                              .getIntersection(bar));
        graphics.setColour(juce::Colour::fromRGB(225, 229, 234)); graphics.setFont(juce::FontOptions(9.0f));
        graphics.drawText(juce::String(descriptor.value * 100.0f, 0), bar.toNearestInt(), juce::Justification::centredRight);
    }
    graphics.setColour(juce::Colour::fromRGB(124, 132, 143)); graphics.setFont(juce::FontOptions(10.0f));
    graphics.drawFittedText(nearestText, descriptors.toNearestInt(), juce::Justification::bottomLeft, 2);

    auto featurePanel = lower;
    auto spectralGraph = featurePanel.removeFromTop(featurePanel.getHeight() * 0.46f);
    graphics.setColour(juce::Colour::fromRGB(29, 33, 39)); graphics.fillRoundedRectangle(spectralGraph, 5.0f);
    graphics.setColour(juce::Colour::fromRGB(63, 70, 81)); graphics.drawRoundedRectangle(spectralGraph, 5.0f, 1.0f);
    const std::array spectralValues { analysis.features.spectral.lowFrequencyExtension,
        analysis.features.spectral.lowMidBuildup, analysis.features.spectral.midEmphasis,
        analysis.features.spectral.upperMidAttack, 1.0f - analysis.features.spectral.highFrequencyRolloff,
        analysis.features.spectral.resonantPeakStrength, analysis.features.spectral.spectralFlatness };
    juce::Path curve;
    auto plot = spectralGraph.reduced(12.0f, 20.0f);
    for (std::size_t index = 0; index < spectralValues.size(); ++index)
    {
        const auto x = plot.getX() + plot.getWidth() * static_cast<float>(index)
            / static_cast<float>(spectralValues.size() - 1);
        const auto y = plot.getBottom() - plot.getHeight() * std::clamp(spectralValues[index], 0.0f, 1.0f);
        if (index == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
    }
    graphics.setColour(juce::Colour::fromRGB(236, 139, 52)); graphics.strokePath(curve, juce::PathStrokeType(2.0f));
    graphics.setColour(juce::Colour::fromRGB(147, 154, 165)); graphics.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    graphics.drawText("MULTI-RESOLUTION TONE PROFILE", spectralGraph.reduced(8.0f), juce::Justification::topLeft);

    featurePanel.removeFromTop(10.0f);
    auto confidence = featurePanel.removeFromTop(72.0f);
    constexpr std::array confidenceNames { "S/L", "CLASS", "TIME", "BANDS", "POLY", "SEP", "DOMAIN" };
    const std::array confidenceValues { analysis.report.confidence.signalToLeakage,
        analysis.report.confidence.classifierCertainty, analysis.report.confidence.duration,
        analysis.report.confidence.spectralCoverage, analysis.report.confidence.polyphony,
        analysis.report.confidence.separationQuality, analysis.report.confidence.modelDomainProximity };
    const auto confidenceWidth = confidence.getWidth() / static_cast<float>(confidenceNames.size());
    for (std::size_t index = 0; index < confidenceNames.size(); ++index)
    {
        auto cell = juce::Rectangle<float>(confidence.getX() + confidenceWidth * static_cast<float>(index),
                                            confidence.getY(), confidenceWidth - 3.0f, confidence.getHeight());
        auto label = cell.removeFromBottom(16.0f);
        graphics.setColour(juce::Colour::fromRGB(34, 39, 45)); graphics.fillRoundedRectangle(cell, 3.0f);
        graphics.setColour(confidenceValues[index] > 0.65f ? juce::Colour::fromRGB(77, 194, 120)
                                                            : juce::Colour::fromRGB(231, 151, 60));
        graphics.fillRoundedRectangle(cell.withTop(cell.getBottom() - cell.getHeight() * confidenceValues[index]), 3.0f);
        graphics.setColour(juce::Colour::fromRGB(137, 145, 156)); graphics.setFont(juce::FontOptions(8.0f, juce::Font::bold));
        graphics.drawText(confidenceNames[index], label, juce::Justification::centred);
    }
    featurePanel.removeFromTop(8.0f);
    graphics.setColour(analysis.report.warnings.empty() ? juce::Colour::fromRGB(103, 178, 126)
                                                        : juce::Colour::fromRGB(232, 157, 68));
    graphics.setFont(juce::FontOptions(10.0f));
    juce::StringArray warningValues;
    for (const auto& warning : analysis.report.warnings) warningValues.add(warning);
    const juce::String warnings = warningValues.isEmpty() ? "No quality warnings"
                                                           : warningValues.joinIntoString("  |  ");
    graphics.drawFittedText(warnings, featurePanel.toNearestInt(), juce::Justification::topLeft, 4);
}

void ReconstructionView::setResult(std::optional<nts::reconstruction::ReconstructionResult> value,
                                   juce::String status, float progress)
{
    result = std::move(value); statusText = std::move(status);
    progressValue = std::clamp(progress, 0.0f, 1.0f); repaint();
}

void ReconstructionView::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(10.0f);
    graphics.setColour(juce::Colour::fromRGB(13, 16, 19)); graphics.fillRoundedRectangle(area, 7.0f);
    graphics.setColour(juce::Colour::fromRGB(48, 54, 62)); graphics.drawRoundedRectangle(area, 7.0f, 1.0f);
    auto status = area.removeFromTop(38.0f).reduced(12.0f, 2.0f);
    graphics.setColour(juce::Colour::fromRGB(240, 144, 58)); graphics.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    graphics.drawFittedText(statusText, status.toNearestInt(), juce::Justification::centredLeft, 1);
    auto progressBar = area.removeFromTop(8.0f).reduced(12.0f, 1.0f);
    graphics.setColour(juce::Colour::fromRGB(31, 35, 41)); graphics.fillRoundedRectangle(progressBar, 3.0f);
    graphics.setColour(juce::Colour::fromRGB(226, 127, 47));
    graphics.fillRoundedRectangle(progressBar.withWidth(progressBar.getWidth() * progressValue), 3.0f);
    area.removeFromTop(8.0f);
    constexpr std::array steps { "1 IMPORT", "2 STEMS", "3 TARGET", "4 REGION", "5 ANALYZE",
                                 "6 SEARCH", "7 COMPARE", "8 LIVE DI", "9 EXPORT" };
    auto stepArea = area.removeFromTop(42.0f);
    const auto stepWidth = stepArea.getWidth() / static_cast<float>(steps.size());
    for (std::size_t index = 0; index < steps.size(); ++index)
    {
        auto cell = juce::Rectangle<float>(stepArea.getX() + static_cast<float>(index) * stepWidth,
                                            stepArea.getY(), stepWidth - 3.0f, stepArea.getHeight());
        const auto active = progressValue >= static_cast<float>(index) / static_cast<float>(steps.size());
        graphics.setColour(active ? juce::Colour::fromRGB(66, 48, 34) : juce::Colour::fromRGB(29, 33, 39));
        graphics.fillRoundedRectangle(cell, 3.0f);
        graphics.setColour(active ? juce::Colour::fromRGB(235, 145, 65) : juce::Colour::fromRGB(108, 115, 125));
        graphics.setFont(juce::FontOptions(8.0f, juce::Font::bold));
        graphics.drawFittedText(steps[index], cell.toNearestInt().reduced(2), juce::Justification::centred, 1);
    }
    area.removeFromTop(8.0f);
    if (! result || ! result->success)
    {
        graphics.setColour(juce::Colour::fromRGB(112, 120, 132)); graphics.setFont(juce::FontOptions(13.0f));
        graphics.drawFittedText("Import a user-provided song, choose guitar or bass and a stereo view.\n"
                                "TubeForge will recommend the cleanest region and return several plausible editable rigs.\n"
                                "The source is cached locally and is never included in exported profiles.",
                                area.reduced(28.0f).toNearestInt(), juce::Justification::centred, 5);
        return;
    }
    const auto& reconstruction = *result;
    auto report = area.removeFromTop(76.0f).reduced(10.0f, 2.0f);
    graphics.setColour(juce::Colour::fromRGB(225, 229, 234)); graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    graphics.drawText(juce::String(nts::reconstruction::toString(reconstruction.reference.target).data()).toUpperCase()
        + "  |  REGION " + juce::String(reconstruction.reference.regionStartSeconds, 1) + "–"
        + juce::String(reconstruction.reference.regionEndSeconds, 1) + " s  |  ANALYSIS "
        + juce::String(reconstruction.reference.analysisConfidence * 100.0f, 0) + "%",
        report.removeFromTop(24.0f), juce::Justification::centredLeft);
    graphics.setColour(juce::Colour::fromRGB(236, 160, 78));
    graphics.drawFittedText(juce::String(reconstruction.reference.gainCharacter).toUpperCase()
        + "  |  PITCH " + reconstruction.reference.dominantPitch + " ("
        + juce::String(reconstruction.reference.pitchConfidence * 100.0f, 0) + "%)  |  "
        + reconstruction.reference.estimatedTuning + "  |  OFFSET "
        + juce::String(reconstruction.reference.tuningOffsetCents >= 0.0f ? "+" : "")
        + juce::String(reconstruction.reference.tuningOffsetCents, 0) + " cents",
        report.removeFromTop(22.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    graphics.setColour(juce::Colour::fromRGB(135, 143, 154)); graphics.setFont(juce::FontOptions(9.0f));
    graphics.drawFittedText("Tone similarity measures the amp-like character; recording similarity also includes loudness and production differences.",
                            report.toNearestInt(), juce::Justification::centredLeft, 2);
    const auto count = std::max<std::size_t>(1, reconstruction.candidates.size());
    const auto cardHeight = std::min(62.0f, area.getHeight() / static_cast<float>(count));
    for (std::size_t index = 0; index < reconstruction.candidates.size(); ++index)
    {
        const auto& candidate = reconstruction.candidates[index];
        auto card = area.removeFromTop(cardHeight).reduced(8.0f, 4.0f);
        graphics.setColour(index == 0 ? juce::Colour::fromRGB(53, 42, 32) : juce::Colour::fromRGB(28, 32, 38));
        graphics.fillRoundedRectangle(card, 5.0f);
        graphics.setColour(index == 0 ? juce::Colour::fromRGB(226, 127, 47) : juce::Colour::fromRGB(63, 70, 80));
        graphics.drawRoundedRectangle(card, 5.0f, 1.0f);
        auto name = card.removeFromLeft(card.getWidth() * 0.54f).reduced(10.0f, 2.0f);
        graphics.setColour(juce::Colour::fromRGB(230, 232, 235)); graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        graphics.drawFittedText(juce::String(static_cast<int>(index + 1)) + ". " + candidate.rigPreset.name,
                                name.toNearestInt(), juce::Justification::centredLeft, 2);
        graphics.setColour(juce::Colour::fromRGB(84, 201, 126));
        graphics.drawText("TONE " + juce::String(candidate.toneSimilarity * 100.0f, 0) + "%",
                          card.removeFromLeft(card.getWidth() * 0.45f), juce::Justification::centred);
        graphics.setColour(juce::Colour::fromRGB(227, 158, 76));
        graphics.drawText("RECORDING " + juce::String(candidate.recordingSimilarity * 100.0f, 0) + "%",
                          card, juce::Justification::centred);
    }
}

void AssistantView::setRecommendations(std::vector<nts::assistant::Recommendation> values,
                                       int selectedIndex, juce::String status, bool previewActive)
{
    recommendations = std::move(values); selected = std::max(0, selectedIndex);
    statusText = std::move(status); preview = previewActive; repaint();
}

void AssistantView::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(10.0f);
    graphics.setColour(juce::Colour::fromRGB(13, 16, 19)); graphics.fillRoundedRectangle(area, 7.0f);
    graphics.setColour(preview ? juce::Colour::fromRGB(232, 148, 62) : juce::Colour::fromRGB(48, 54, 62));
    graphics.drawRoundedRectangle(area, 7.0f, preview ? 2.0f : 1.0f);
    auto header = area.removeFromTop(42.0f).reduced(12.0f, 2.0f);
    graphics.setColour(preview ? juce::Colour::fromRGB(245, 176, 78) : juce::Colour::fromRGB(240, 144, 58));
    graphics.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    graphics.drawFittedText(statusText, header.toNearestInt(), juce::Justification::centredLeft, 1);
    area.removeFromTop(5.0f);
    if (recommendations.empty())
    {
        graphics.setColour(juce::Colour::fromRGB(112, 120, 132)); graphics.setFont(juce::FontOptions(13.0f));
        graphics.drawFittedText("Play through TubeForge for technical diagnostics, analyze a clip for spectral evidence,\n"
                                "or select a musical goal. Suggestions never apply without a preview.",
                                area.reduced(30.0f).toNearestInt(), juce::Justification::centred, 4);
        return;
    }
    selected = std::clamp(selected, 0, static_cast<int>(recommendations.size()) - 1);
    auto list = area.removeFromLeft(area.getWidth() * 0.40f);
    area.removeFromLeft(12.0f);
    const auto rowHeight = std::min(66.0f, list.getHeight() / static_cast<float>(recommendations.size()));
    for (std::size_t index = 0; index < recommendations.size(); ++index)
    {
        const auto& recommendation = recommendations[index];
        auto row = list.removeFromTop(rowHeight).reduced(4.0f, 3.0f);
        const auto active = static_cast<int>(index) == selected;
        graphics.setColour(active ? juce::Colour::fromRGB(61, 44, 31) : juce::Colour::fromRGB(28, 32, 38));
        graphics.fillRoundedRectangle(row, 4.0f);
        graphics.setColour(active ? juce::Colour::fromRGB(226, 127, 47) : juce::Colour::fromRGB(61, 68, 78));
        graphics.drawRoundedRectangle(row, 4.0f, 1.0f);
        auto confidence = row.removeFromRight(55.0f);
        graphics.setColour(juce::Colour::fromRGB(83, 201, 125)); graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        graphics.drawText(juce::String(recommendation.confidence * 100.0f, 0) + "%", confidence,
                          juce::Justification::centred);
        graphics.setColour(juce::Colour::fromRGB(225, 228, 233)); graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        graphics.drawFittedText(recommendation.diagnosis, row.reduced(8.0f, 2.0f).toNearestInt(),
                                juce::Justification::centredLeft, 2);
    }
    const auto& recommendation = recommendations[static_cast<std::size_t>(selected)];
    auto detail = area.reduced(8.0f, 4.0f);
    graphics.setColour(juce::Colour::fromRGB(234, 235, 238)); graphics.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    graphics.drawFittedText(recommendation.beginnerExplanation,
                            detail.removeFromTop(52.0f).toNearestInt(), juce::Justification::topLeft, 3);
    graphics.setColour(juce::Colour::fromRGB(147, 155, 166)); graphics.setFont(juce::FontOptions(10.0f));
    graphics.drawFittedText(recommendation.advancedExplanation,
                            detail.removeFromTop(66.0f).toNearestInt(), juce::Justification::topLeft, 5);
    detail.removeFromTop(5.0f);
    graphics.setColour(juce::Colour::fromRGB(232, 152, 66)); graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    graphics.drawText("EVIDENCE", detail.removeFromTop(18.0f), juce::Justification::centredLeft);
    juce::StringArray evidence; for (const auto& item : recommendation.evidence) evidence.add(item);
    graphics.setColour(juce::Colour::fromRGB(185, 191, 200)); graphics.setFont(juce::FontOptions(9.5f));
    graphics.drawFittedText(evidence.isEmpty() ? "Style preference (not a measured fault)" : evidence.joinIntoString("  |  "),
                            detail.removeFromTop(46.0f).toNearestInt(), juce::Justification::topLeft, 4);
    graphics.setColour(juce::Colour::fromRGB(232, 152, 66)); graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    graphics.drawText("VALIDATED CHANGES", detail.removeFromTop(18.0f), juce::Justification::centredLeft);
    juce::StringArray changes;
    for (const auto& item : recommendation.changes)
        changes.add(juce::String(item.parameter) + "  " + juce::String(item.from, 2) + " → " + juce::String(item.to, 2));
    graphics.setColour(juce::Colour::fromRGB(214, 219, 226)); graphics.setFont(juce::FontOptions(9.5f));
    graphics.drawFittedText(changes.isEmpty() ? "No automatic parameter change; inspect the physical signal path."
                                               : changes.joinIntoString("\n"),
                            detail.removeFromTop(std::min(100.0f, detail.getHeight() * 0.50f)).toNearestInt(),
                            juce::Justification::topLeft, 7);
    graphics.setColour(juce::Colour::fromRGB(92, 203, 132)); graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    graphics.drawFittedText("EXPECTED: " + recommendation.expectedEffect,
                            detail.toNearestInt(), juce::Justification::bottomLeft, 3);
}

void AmpLookAndFeel::drawRotarySlider(juce::Graphics& graphics, int x, int y, int width, int height,
                                      float sliderPosition, float rotaryStartAngle,
                                      float rotaryEndAngle, juce::Slider& slider)
{
    juce::ignoreUnused(slider);
    const auto size = static_cast<float>(std::min(width, height));
    const auto radius = size * 0.42f;
    const auto centre = juce::Point<float>(static_cast<float>(x) + static_cast<float>(width) * 0.5f,
                                           static_cast<float>(y) + static_cast<float>(height) * 0.5f);
    const auto knob = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);
    const auto angle = rotaryStartAngle + sliderPosition * (rotaryEndAngle - rotaryStartAngle);

    graphics.setColour(juce::Colours::black.withAlpha(0.55f));
    graphics.fillEllipse(knob.translated(0.0f, 3.0f));

    juce::ColourGradient face(juce::Colour::fromRGB(82, 78, 72), centre.x - radius * 0.45f,
                              centre.y - radius * 0.55f, juce::Colour::fromRGB(18, 19, 20),
                              centre.x + radius, centre.y + radius, true);
    face.addColour(0.52, juce::Colour::fromRGB(45, 44, 42));
    graphics.setGradientFill(face);
    graphics.fillEllipse(knob);

    graphics.setColour(juce::Colour::fromRGB(177, 103, 49));
    graphics.drawEllipse(knob.reduced(1.0f), 2.0f);
    graphics.setColour(juce::Colour::fromRGB(224, 154, 72).withAlpha(0.55f));
    graphics.drawEllipse(knob.reduced(4.0f), 1.0f);

    for (int tick = 0; tick <= 10; ++tick)
    {
        const auto tickAngle = rotaryStartAngle
            + static_cast<float>(tick) * (rotaryEndAngle - rotaryStartAngle) / 10.0f;
        const auto direction = juce::Point<float>(std::sin(tickAngle), -std::cos(tickAngle));
        graphics.setColour(tick <= static_cast<int>(std::round(sliderPosition * 10.0f))
                               ? juce::Colour::fromRGB(239, 166, 77)
                               : juce::Colour::fromRGB(91, 75, 63));
        graphics.drawLine({ centre + direction * (radius + 4.0f),
                            centre + direction * (radius + 8.0f) }, 1.4f);
    }

    const auto direction = juce::Point<float>(std::sin(angle), -std::cos(angle));
    graphics.setColour(juce::Colour::fromRGB(255, 184, 82));
    graphics.drawLine({ centre + direction * (radius * 0.22f),
                        centre + direction * (radius * 0.72f) }, 3.0f);
    graphics.setColour(juce::Colour::fromRGB(12, 12, 13));
    graphics.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre(centre));
}

void AmpLookAndFeel::drawButtonBackground(juce::Graphics& graphics, juce::Button& button,
                                          const juce::Colour&, bool isMouseOver, bool isButtonDown)
{
    auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    auto base = button.getToggleState() ? juce::Colour::fromRGB(90, 51, 28)
                                        : juce::Colour::fromRGB(38, 42, 49);
    if (isMouseOver) base = base.brighter(0.10f);
    if (isButtonDown) base = juce::Colour::fromRGB(80, 51, 31);
    graphics.setColour(base); graphics.fillRoundedRectangle(area, 4.0f);
    graphics.setColour(isMouseOver || button.getToggleState() ? juce::Colour::fromRGB(218, 126, 51)
                                                               : juce::Colour::fromRGB(67, 73, 83));
    graphics.drawRoundedRectangle(area, 4.0f, 1.0f);
}

void AmpLookAndFeel::drawToggleButton(juce::Graphics& graphics, juce::ToggleButton& button,
                                      bool isMouseOver, bool)
{
    auto area = button.getLocalBounds().toFloat().reduced(1.0f);
    const auto active = button.getToggleState();
    graphics.setColour(active ? juce::Colour::fromRGB(112, 61, 29) : juce::Colour::fromRGB(34, 38, 44));
    graphics.fillRoundedRectangle(area, 4.0f);
    graphics.setColour(active || isMouseOver ? juce::Colour::fromRGB(238, 139, 56) : juce::Colour::fromRGB(70, 76, 86));
    graphics.drawRoundedRectangle(area, 4.0f, 1.0f);
    graphics.setColour(active ? juce::Colours::white : juce::Colour::fromRGB(190, 195, 204));
    graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    graphics.drawFittedText(button.getButtonText(), area.toNearestInt().reduced(7, 0), juce::Justification::centred, 1);
}

void AmpLookAndFeel::drawComboBox(juce::Graphics& graphics, int width, int height, bool isButtonDown,
                                  int buttonX, int buttonY, int buttonWidth, int buttonHeight,
                                  juce::ComboBox& box)
{
    juce::ignoreUnused(buttonX, buttonY, buttonWidth, buttonHeight, box);
    auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    graphics.setColour(isButtonDown ? juce::Colour::fromRGB(55, 48, 43) : juce::Colour::fromRGB(27, 30, 35));
    graphics.fillRoundedRectangle(area, 4.0f);
    graphics.setColour(juce::Colour::fromRGB(74, 80, 91)); graphics.drawRoundedRectangle(area, 4.0f, 1.0f);
    juce::Path arrow;
    const auto x = area.getRight() - 14.0f; const auto y = area.getCentreY();
    arrow.startNewSubPath(x - 4.0f, y - 2.0f); arrow.lineTo(x, y + 2.0f); arrow.lineTo(x + 4.0f, y - 2.0f);
    graphics.setColour(juce::Colour::fromRGB(231, 137, 57)); graphics.strokePath(arrow, juce::PathStrokeType(1.5f));
}

TubeForgeAudioProcessorEditor::TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& owner)
    : juce::AudioProcessorEditor(owner), processor(owner)
{
    setLookAndFeel(&ampLookAndFeel);
    configureHeading(title, "TUBEFORGE");
    title.setFont(juce::FontOptions(25.0f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, juce::Colour::fromRGB(247, 151, 63));
    configureHeading(productTagline, "NEURAL AMPLIFIER STUDIO  /  0.6");
    productTagline.setFont(juce::FontOptions(9.5f, juce::Font::bold));
    productTagline.setColour(juce::Label::textColourId, juce::Colour::fromRGB(127, 134, 145));
    configureHeading(presetCaption, "CURRENT RIG");
    presetCaption.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    presetCaption.setColour(juce::Label::textColourId, juce::Colour::fromRGB(126, 132, 142));
    configureHeading(presetName, "TF Modern Studio 01");
    presetName.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    configureHeading(mode, "Mode: " + processor.modeName());
    mode.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    mode.setColour(juce::Label::textColourId, juce::Colour::fromRGB(144, 151, 161));
    configureHeading(deviceStatus, processor.deviceStatusText());
    deviceStatus.setFont(juce::FontOptions(9.0f));
    deviceStatus.setColour(juce::Label::textColourId, juce::Colour::fromRGB(112, 120, 132));
    configureHeading(diagnosticsText, "Diagnostics waiting for audio...");
    diagnosticsText.setFont(juce::FontOptions(10.5f));
    diagnosticsText.setColour(juce::Label::textColourId, juce::Colour::fromRGB(137, 145, 156));
    configureHeading(inputLabel, "INPUT");
    configureHeading(outputLabel, "OUTPUT");
    inputLabel.setJustificationType(juce::Justification::centred);
    outputLabel.setJustificationType(juce::Justification::centred);
    inputLabel.setFont(juce::FontOptions(9.5f, juce::Font::bold));
    outputLabel.setFont(juce::FontOptions(9.5f, juce::Font::bold));

    configureKnob(inputGain);
    configureKnob(outputGain);
    inputGain.setTextValueSuffix(" dB");
    outputGain.setTextValueSuffix(" dB");

    ampArtwork.setImage(processor.ampFaceplateArtwork());
    ampArtwork.setImagePlacement(juce::RectanglePlacement::stretchToFit);
    ampArtwork.setInterceptsMouseClicks(false, false);
    simplePage.addAndMakeVisible(ampArtwork);

    for (std::size_t index = 0; index < simpleSliders.size(); ++index)
    {
        configureHeading(simpleLabels[index], simpleNames[index]);
        simpleLabels[index].setJustificationType(juce::Justification::centred);
        simpleLabels[index].setFont(juce::FontOptions(13.5f, juce::Font::bold));
        simpleLabels[index].setColour(juce::Label::textColourId, juce::Colour::fromRGB(241, 210, 167));
        configureKnob(simpleSliders[index]);
        simpleSliders[index].setLookAndFeel(&ampLookAndFeel);
        simplePage.addAndMakeVisible(simpleLabels[index]);
        simplePage.addAndMakeVisible(simpleSliders[index]);
    }
    for (std::size_t index = 0; index < advancedSliders.size(); ++index)
    {
        configureHeading(advancedLabels[index], advancedNames[index]);
        configureSlider(advancedSliders[index]);
        advancedPage.addAndMakeVisible(advancedLabels[index]);
        advancedPage.addAndMakeVisible(advancedSliders[index]);
    }
    instrumentSelector.addItemList({ "Guitar", "Bass" }, 1);
    topologySelector.addItemList({ "Tight Modern", "Vintage Bloom" }, 1);
    oversamplingSelector.addItemList({ "1x (min latency)", "2x", "4x", "8x" }, 1);
    engineModeSelector.addItemList({ "Traditional", "Neural capture", "Physical circuit" }, 1);
    neuralMonitorSelector.addItemList({ "Model", "Bypass DI", "Loudness matched" }, 1);
    configureHeading(neuralStatus, "No neural model loaded");
    configureHeading(neuralCalibration, "Input calibration waiting for audio");
    configureHeading(captureWizardHelp,
        "Capture wizard: run ml/scripts/capture_wizard.py outside the plug-in, then load its artifact here.");
    simplePage.addAndMakeVisible(instrumentSelector);
    simplePage.addAndMakeVisible(cabinetEnabled);
    ampArtwork.toBack();
    advancedPage.addAndMakeVisible(topologySelector);
    advancedPage.addAndMakeVisible(oversamplingSelector);
    for (auto* component : std::initializer_list<juce::Component*> {
             &neuralMonitorSelector, &neuralCompensation,
             &loadNeuralModel, &neuralStatus, &neuralCalibration, &captureWizardHelp })
        neuralPage.addAndMakeVisible(*component);
    configureHeading(circuitHelp,
        "Choose real circuit parts below. Amp and Advanced knobs set the component values; changes compile off-thread and crossfade safely.");
    circuitPreampTube.addItemList({ "Preamp: 12AU7", "Preamp: 12AT7", "Preamp: 12AX7" }, 1);
    circuitPowerTube.addItemList({ "Power: 6V6", "Power: EL34" }, 1);
    circuitPowerTopology.addItemList({ "Single-ended", "Push-pull Class A", "Push-pull Class AB" }, 1);
    circuitToneStack.addItemList({ "Tone: Vintage FMV", "Tone: Modern", "Tone: Bass" }, 1);
    circuitBackend.addItemList({ "Solver: Gray-box", "Solver: Numerical" }, 1);
    circuitCabinetStyle.addItemList({ "Cab: Reactive", "Cab: Open back", "Cab: Bass sealed" }, 1);
    circuitPage.addAndMakeVisible(circuitHelp);
    for (auto* selector : std::initializer_list<juce::ComboBox*> { &circuitPreampTube, &circuitPowerTube,
             &circuitPowerTopology, &circuitToneStack, &circuitBackend, &circuitCabinetStyle })
        circuitPage.addAndMakeVisible(*selector);
    circuitPage.addAndMakeVisible(circuitSchematic);
    configureHeading(analyzerHelp,
        "Offline tone intelligence: multiresolution spectrum, dynamics, nonlinear and spatial analysis, 128-D embedding, confidence, comparison and search.");
    analyzerHelp.setColour(juce::Label::textColourId, juce::Colour::fromRGB(151, 158, 169));
    analyzerPage.addAndMakeVisible(analyzeToneClip);
    analyzerPage.addAndMakeVisible(analyzerHelp);
    analyzerPage.addAndMakeVisible(toneAnalysisView);
    reconstructionTarget.addItemList({ "Guitar", "Bass" }, 1);
    reconstructionTarget.setSelectedId(1);
    reconstructionStereoMode.addItemList({ "Full stereo", "Left", "Right", "Mid", "Side", "Panned estimate" }, 1);
    reconstructionStereoMode.setSelectedId(1);
    configureHeading(reconstructionHelp,
        "Demucs detects direct guitar/bass stems, playable clean or distorted parts, dominant pitch, and likely tuning. Choose a timestamped part below and rebuild its tone.");
    for (auto* component : std::initializer_list<juce::Component*> { &importSong, &cancelReconstruction,
             &applyCandidate, &exportCandidate, &reconstructionTarget, &reconstructionStereoMode,
             &reconstructionRegion, &useReconstructionRegion, &reconstructionCandidate,
             &reconstructionHelp, &reconstructionView })
        reconstructionPage.addAndMakeVisible(*component);
    assistantGoal.addItemList({ "Diagnose", "Tight rhythm", "Clean bass support", "Aggressive picked bass",
                                "Smooth lead", "Warm clean", "Less harsh", "Preserve low end", "Match reference" }, 1);
    assistantGoal.setSelectedId(1);
    assistantPersonalization.setToggleState(processor.assistantPersonalizationEnabled(), juce::dontSendNotification);
    configureHeading(assistantHelp,
        "Evidence-based local assistant. Preview first; accept, reject, or undo exact bounded parameter changes.");
    for (auto* component : std::initializer_list<juce::Component*> { &assistantGoal, &assistantSuggestion,
             &assistantPreview, &assistantAccept, &assistantReject, &assistantUndo,
             &assistantPersonalization, &clearAssistantPreferences, &assistantHelp, &assistantView })
        assistantPage.addAndMakeVisible(*component);
    profileSearch.setTextToShowWhenEmpty("Search name, author, or tag", juce::Colour::fromRGB(112, 118, 128));
    profileName.setTextToShowWhenEmpty("Profile name", juce::Colour::fromRGB(112, 118, 128));
    profileAuthor.setTextToShowWhenEmpty("Author", juce::Colour::fromRGB(112, 118, 128));
    profileName.setText("My TubeForge Rig", false); profileAuthor.setText("Local User", false);
    profileInstrument.addItemList({ "All instruments", "Guitar", "Bass" }, 1);
    profileInstrument.setSelectedId(1);
    configureHeading(profileDetails,
        "Local file-based profile sharing. Imports are validated before assets can reach the audio engine.");
    profileDetails.setColour(juce::Label::textColourId, juce::Colour::fromRGB(151, 158, 169));
    profileDetails.setJustificationType(juce::Justification::topLeft);
    for (auto* component : std::initializer_list<juce::Component*> { &profileSearch, &profileName, &profileAuthor,
             &profileInstrument, &profileList, &profileFavoritesOnly, &profileFavorite, &importProfile,
             &exportProfile, &applyProfile, &refreshProfiles, &profileDetails })
        profilesPage.addAndMakeVisible(*component);
    ampTabs.addTab("Amp", juce::Colour::fromRGB(29, 34, 42), &simplePage, false);
    ampTabs.addTab("Advanced", juce::Colour::fromRGB(29, 34, 42), &advancedPage, false);
    ampTabs.addTab("Neural Capture", juce::Colour::fromRGB(29, 34, 42), &neuralPage, false);
    ampTabs.addTab("Circuit Engineering", juce::Colour::fromRGB(29, 34, 42), &circuitPage, false);
    ampTabs.addTab("Tone Analyzer", juce::Colour::fromRGB(29, 34, 42), &analyzerPage, false);
    ampTabs.addTab("Song Reconstruction", juce::Colour::fromRGB(29, 34, 42), &reconstructionPage, false);
    ampTabs.addTab("Tone Assistant", juce::Colour::fromRGB(29, 34, 42), &assistantPage, false);
    ampTabs.addTab("Profile Library", juce::Colour::fromRGB(29, 34, 42), &profilesPage, false);
    ampTabs.setColour(juce::TabbedComponent::backgroundColourId, juce::Colour::fromRGB(18, 20, 24));
    ampTabs.setColour(juce::TabbedComponent::outlineColourId, juce::Colour::fromRGB(54, 59, 68));

    configureHeading(browserTitle, "GEAR BROWSER");
    browserTitle.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    browserTitle.setColour(juce::Label::textColourId, juce::Colour::fromRGB(238, 142, 58));
    configureHeading(browserSubtitle, "RIG COMPONENTS");
    browserSubtitle.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    browserSubtitle.setColour(juce::Label::textColourId, juce::Colour::fromRGB(105, 112, 123));
    browserPanel.addAndMakeVisible(browserTitle);
    browserPanel.addAndMakeVisible(browserSubtitle);
    browserPanel.addAndMakeVisible(mode);
    browserPanel.addAndMakeVisible(deviceStatus);
    for (std::size_t index = 0; index < browserButtons.size(); ++index)
    {
        browserButtons[index].setClickingTogglesState(false);
        browserButtons[index].setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(199, 203, 210));
        browserButtons[index].onClick = [this, index]
        {
            ampTabs.setCurrentTabIndex(static_cast<int>(index));
        };
        browserPanel.addAndMakeVisible(browserButtons[index]);
    }

    for (auto* component : std::initializer_list<juce::Component*> {
             &title, &productTagline, &presetCaption, &presetName,
             &diagnosticsText, &inputLabel, &outputLabel, &browserPanel, &signalChain, &studioMeter,
             &inputGain, &outputGain, &bypass, &engineModeSelector, &audioSettings, &openProject, &saveProject,
             &ampTabs })
        addAndMakeVisible(*component);

    inputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "input", inputGain);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "output", outputGain);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "bypass", bypass);
    for (std::size_t index = 0; index < simpleSliders.size(); ++index)
        ampSliderAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getParameters(), simpleIds[index], simpleSliders[index]));
    for (std::size_t index = 0; index < advancedSliders.size(); ++index)
        ampSliderAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getParameters(), advancedIds[index], advancedSliders[index]));
    instrumentAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "instrument", instrumentSelector);
    topologyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "topology", topologySelector);
    oversamplingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "oversampling", oversamplingSelector);
    cabinetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "cabinet", cabinetEnabled);
    engineModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "engineMode", engineModeSelector);
    neuralMonitorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "neuralMonitor", neuralMonitorSelector);
    neuralCompensationAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "neuralCompensation", neuralCompensation);
    constexpr std::array circuitIds { "circuitPreampTube", "circuitPowerTube", "circuitPowerTopology",
                                      "circuitToneStack", "circuitBackend", "circuitCabinetStyle" };
    const std::array circuitSelectors { &circuitPreampTube, &circuitPowerTube, &circuitPowerTopology,
                                        &circuitToneStack, &circuitBackend, &circuitCabinetStyle };
    for (std::size_t index = 0; index < circuitAttachments.size(); ++index)
        circuitAttachments[index] = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processor.getParameters(), circuitIds[index], *circuitSelectors[index]);

    audioSettings.onClick = [this]
    {
        const auto message = processor.modeName() == "Standalone"
            ? "Open the Audio settings tab to select ASIO/WASAPI devices, channels, sample rate, and buffer size."
            : "Audio devices, sample rate, channels, and buffer size are controlled by the plugin host.";
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Audio settings", message);
    };
    saveProject.onClick = [this] { chooseProjectToSave(); };
    openProject.onClick = [this] { chooseProjectToOpen(); };
    loadNeuralModel.onClick = [this] { chooseNeuralModel(); };
    analyzeToneClip.onClick = [this] { chooseToneClip(); };
    importSong.onClick = [this] { chooseSongForReconstruction(); };
    cancelReconstruction.onClick = [this] { processor.cancelSongReconstruction(); };
    useReconstructionRegion.onClick = [this]
    {
        const auto index = std::max(0, reconstructionRegion.getSelectedId() - 1);
        (void) processor.requestReconstructionRegion(static_cast<std::size_t>(index));
    };
    applyCandidate.onClick = [this]
    {
        const auto index = std::max(0, reconstructionCandidate.getSelectedId() - 1);
        (void) processor.applyReconstructionCandidate(static_cast<std::size_t>(index));
    };
    exportCandidate.onClick = [this] { chooseReconstructionExport(); };
    assistantGoal.onChange = [this]
    {
        constexpr std::array goals { nts::assistant::Goal::diagnose, nts::assistant::Goal::tightRhythm,
            nts::assistant::Goal::cleanBassSupport, nts::assistant::Goal::aggressivePickedBass,
            nts::assistant::Goal::smoothLead, nts::assistant::Goal::warmClean,
            nts::assistant::Goal::lessHarsh, nts::assistant::Goal::preserveLowEnd,
            nts::assistant::Goal::matchReference };
        const auto index = std::clamp(assistantGoal.getSelectedId() - 1, 0, static_cast<int>(goals.size()) - 1);
        processor.setAssistantGoal(goals[static_cast<std::size_t>(index)]);
    };
    assistantPreview.onClick = [this]
    { (void) processor.previewAssistantRecommendation(static_cast<std::size_t>(std::max(0, assistantSuggestion.getSelectedId() - 1))); };
    assistantAccept.onClick = [this] { (void) processor.acceptAssistantPreview(); };
    assistantReject.onClick = [this] { (void) processor.rejectAssistantPreview(); };
    assistantUndo.onClick = [this] { (void) processor.undoAssistantChange(); };
    assistantPersonalization.onClick = [this]
    { processor.setAssistantPersonalizationEnabled(assistantPersonalization.getToggleState()); };
    clearAssistantPreferences.onClick = [this] { processor.clearAssistantPreferences(); };
    profileSearch.onTextChange = [this] { refreshPackageBrowser(); };
    profileInstrument.onChange = [this] { refreshPackageBrowser(); };
    profileFavoritesOnly.onClick = [this] { refreshPackageBrowser(); };
    refreshProfiles.onClick = [this]
    {
        const auto result = processor.refreshTonePackages();
        if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Profile refresh failed", result.getErrorMessage());
        refreshPackageBrowser();
    };
    profileList.onChange = [this]
    {
        const auto index = profileList.getSelectedId() - 1;
        if (index < 0 || index >= static_cast<int>(visibleProfiles.size())) return;
        const auto& profile = visibleProfiles[static_cast<std::size_t>(index)];
        profileFavorite.setToggleState(profile.favorite, juce::dontSendNotification);
        auto warning = profile.warnings.empty() ? juce::String {} : "\nWarning: " + juce::String(profile.warnings.front());
        profileDetails.setText(juce::String(profile.manifest.name) + " by " + juce::String(profile.manifest.author)
            + "\n" + juce::String(profile.manifest.instrument) + " | " + juce::String(profile.manifest.qualityTier)
            + " | runtime >= " + juce::String(profile.manifest.minimumRuntime)
            + (profile.signatureVerified ? " | trusted signature" : " | local/unsigned") + warning,
            juce::dontSendNotification);
    };
    profileFavorite.onClick = [this]
    {
        const auto index = profileList.getSelectedId() - 1;
        if (index < 0 || index >= static_cast<int>(visibleProfiles.size())) return;
        const auto result = processor.setTonePackageFavorite(
            visibleProfiles[static_cast<std::size_t>(index)].manifest.packageId,
            profileFavorite.getToggleState());
        if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Favorite failed", result.getErrorMessage());
        refreshPackageBrowser();
    };
    applyProfile.onClick = [this]
    {
        const auto index = profileList.getSelectedId() - 1;
        if (index < 0 || index >= static_cast<int>(visibleProfiles.size())) return;
        const auto result = processor.applyTonePackage(
            visibleProfiles[static_cast<std::size_t>(index)].manifest.packageId);
        if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Apply failed", result.getErrorMessage());
        else presetName.setText(visibleProfiles[static_cast<std::size_t>(index)].manifest.name,
                                juce::dontSendNotification);
        refreshPackageBrowser();
    };
    importProfile.onClick = [this] { chooseTonePackageImport(); };
    exportProfile.onClick = [this] { chooseTonePackageExport(); };
    refreshPackageBrowser();

    setResizable(true, true);
    setResizeLimits(980, 680, 1600, 1080);
    setSize(1240, 820);
    startTimerHz(20);
}

TubeForgeAudioProcessorEditor::~TubeForgeAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
    for (auto& slider : simpleSliders)
        slider.setLookAndFeel(nullptr);
}

void TubeForgeAudioProcessorEditor::paint(juce::Graphics& graphics)
{
    juce::ColourGradient background(juce::Colour::fromRGB(24, 27, 32), 0.0f, 0.0f,
                                    juce::Colour::fromRGB(10, 12, 15), 0.0f,
                                    static_cast<float>(getHeight()), false);
    graphics.setGradientFill(background); graphics.fillAll();

    auto shell = getLocalBounds().toFloat().reduced(8.0f);
    graphics.setColour(juce::Colour::fromRGB(7, 9, 11)); graphics.fillRoundedRectangle(shell, 9.0f);
    graphics.setColour(juce::Colour::fromRGB(57, 62, 70)); graphics.drawRoundedRectangle(shell, 9.0f, 1.0f);

    auto header = shell.withHeight(70.0f);
    juce::ColourGradient headerGradient(juce::Colour::fromRGB(38, 42, 48), header.getTopLeft(),
                                        juce::Colour::fromRGB(18, 21, 25), header.getBottomLeft(), false);
    graphics.setGradientFill(headerGradient); graphics.fillRoundedRectangle(header, 8.0f);
    graphics.setColour(juce::Colour::fromRGB(222, 126, 46));
    graphics.fillRect(header.withY(header.getBottom() - 2.0f).withHeight(2.0f));

    const auto drawPanel = [&graphics](juce::Rectangle<int> bounds, float radius = 6.0f)
    {
        if (bounds.isEmpty()) return;
        auto panel = bounds.toFloat();
        graphics.setColour(juce::Colour::fromRGB(18, 21, 25)); graphics.fillRoundedRectangle(panel, radius);
        graphics.setColour(juce::Colour::fromRGB(47, 52, 60)); graphics.drawRoundedRectangle(panel, radius, 1.0f);
    };
    drawPanel(browserPanel.getBounds());
    drawPanel(ampTabs.getBounds());

    auto footer = diagnosticsText.getBounds().toFloat().expanded(6.0f, 2.0f);
    graphics.setColour(juce::Colour::fromRGB(15, 18, 21)); graphics.fillRoundedRectangle(footer, 4.0f);
    graphics.setColour(juce::Colour::fromRGB(42, 47, 55)); graphics.drawRoundedRectangle(footer, 4.0f, 1.0f);
}

void TubeForgeAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(14);
    auto header = area.removeFromTop(58);
    auto brand = header.removeFromLeft(190);
    title.setBounds(brand.removeFromTop(34));
    productTagline.setBounds(brand);
    auto preset = header.removeFromLeft(220).reduced(8, 2);
    presetCaption.setBounds(preset.removeFromTop(17));
    presetName.setBounds(preset);
    engineModeSelector.setBounds(header.removeFromLeft(168).reduced(5, 11));
    bypass.setBounds(header.removeFromLeft(74).reduced(5, 11));
    openProject.setBounds(header.removeFromLeft(80).reduced(4, 11));
    saveProject.setBounds(header.removeFromLeft(80).reduced(4, 11));
    audioSettings.setBounds(header.removeFromLeft(std::min(116, header.getWidth())).reduced(4, 11));

    area.removeFromTop(10);
    diagnosticsText.setBounds(area.removeFromBottom(30).reduced(7, 2));
    area.removeFromBottom(7);
    signalChain.setBounds(area.removeFromBottom(96));
    area.removeFromBottom(8);

    auto browser = area.removeFromLeft(180);
    browserPanel.setBounds(browser);
    area.removeFromLeft(8);
    auto meterRail = area.removeFromRight(112);
    area.removeFromRight(8);
    ampTabs.setBounds(area);

    auto browserInside = browserPanel.getLocalBounds().reduced(12);
    browserTitle.setBounds(browserInside.removeFromTop(24));
    browserSubtitle.setBounds(browserInside.removeFromTop(20));
    browserInside.removeFromTop(8);
    for (auto& button : browserButtons)
    {
        button.setBounds(browserInside.removeFromTop(38).reduced(0, 3));
        browserInside.removeFromTop(2);
    }
    auto browserStatus = browserInside.removeFromBottom(66);
    mode.setBounds(browserStatus.removeFromTop(24));
    deviceStatus.setBounds(browserStatus);

    auto meter = meterRail;
    inputLabel.setBounds(meter.removeFromTop(20));
    inputGain.setBounds(meter.removeFromTop(94).reduced(4, 0));
    meter.removeFromTop(6);
    outputLabel.setBounds(meter.removeFromTop(20));
    outputGain.setBounds(meter.removeFromTop(94).reduced(4, 0));
    meter.removeFromTop(8);
    studioMeter.setBounds(meter);

    ampArtwork.setBounds(simplePage.getLocalBounds());
    auto simple = simplePage.getLocalBounds().reduced(18, 10);
    auto controlDeck = simple.removeFromBottom(std::max(154, simple.getHeight() * 40 / 100));
    auto simpleTop = controlDeck.removeFromTop(34);
    instrumentSelector.setBounds(simpleTop.removeFromLeft(160).reduced(4, 2));
    cabinetEnabled.setBounds(simpleTop.removeFromLeft(150).reduced(8, 2));
    controlDeck.removeFromTop(2);
    constexpr int simpleColumns = 7;
    for (std::size_t index = 0; index < simpleSliders.size(); ++index)
    {
        const auto column = static_cast<int>(index) % simpleColumns;
        auto cell = juce::Rectangle<int>(column * controlDeck.getWidth() / simpleColumns, 0,
            controlDeck.getWidth() / simpleColumns, controlDeck.getHeight())
            .translated(controlDeck.getX(), controlDeck.getY()).reduced(3, 0);
        simpleLabels[index].setBounds(cell.removeFromTop(20));
        simpleSliders[index].setBounds(cell);
    }

    auto advanced = advancedPage.getLocalBounds().reduced(12);
    auto advancedTop = advanced.removeFromTop(34);
    topologySelector.setBounds(advancedTop.removeFromLeft(220).reduced(4));
    oversamplingSelector.setBounds(advancedTop.removeFromLeft(150).reduced(4));
    constexpr int advancedColumns = 3;
    const auto advancedRows = static_cast<int>((advancedSliders.size() + advancedColumns - 1) / advancedColumns);
    const auto advancedCellHeight = std::max(48, advanced.getHeight() / advancedRows);
    for (std::size_t index = 0; index < advancedSliders.size(); ++index)
    {
        const auto row = static_cast<int>(index) / advancedColumns;
        const auto column = static_cast<int>(index) % advancedColumns;
        auto cell = juce::Rectangle<int>(column * advanced.getWidth() / advancedColumns,
            row * advancedCellHeight, advanced.getWidth() / advancedColumns, advancedCellHeight).translated(advanced.getX(), advanced.getY()).reduced(5);
        advancedLabels[index].setBounds(cell.removeFromTop(18)); advancedSliders[index].setBounds(cell);
    }

    auto neural = neuralPage.getLocalBounds().reduced(24);
    auto neuralTop = neural.removeFromTop(42);
    neuralMonitorSelector.setBounds(neuralTop.removeFromLeft(210).reduced(4));
    loadNeuralModel.setBounds(neuralTop.removeFromLeft(180).reduced(4));
    neural.removeFromTop(12);
    neuralCompensation.setBounds(neural.removeFromTop(34));
    neuralStatus.setBounds(neural.removeFromTop(38));
    neuralCalibration.setBounds(neural.removeFromTop(52));
    neural.removeFromTop(12);
    captureWizardHelp.setBounds(neural.removeFromTop(64));

    auto circuit = circuitPage.getLocalBounds().reduced(16);
    circuitHelp.setBounds(circuit.removeFromTop(38));
    auto selectors = circuit.removeFromTop(76);
    const std::array circuitControls { &circuitPreampTube, &circuitPowerTube, &circuitPowerTopology,
                                       &circuitToneStack, &circuitBackend, &circuitCabinetStyle };
    for (std::size_t index = 0; index < circuitControls.size(); ++index)
    {
        const auto row = static_cast<int>(index / 3);
        const auto column = static_cast<int>(index % 3);
        auto cell = juce::Rectangle<int>(column * selectors.getWidth() / 3, row * selectors.getHeight() / 2,
            selectors.getWidth() / 3, selectors.getHeight() / 2).translated(selectors.getX(), selectors.getY()).reduced(4, 3);
        circuitControls[index]->setBounds(cell);
    }
    circuitSchematic.setBounds(circuit);

    auto analyzer = analyzerPage.getLocalBounds().reduced(14);
    auto analyzerTop = analyzer.removeFromTop(42);
    analyzeToneClip.setBounds(analyzerTop.removeFromLeft(170).reduced(3));
    analyzerHelp.setBounds(analyzerTop.reduced(8, 0));
    analyzer.removeFromTop(6);
    toneAnalysisView.setBounds(analyzer);

    auto reconstruction = reconstructionPage.getLocalBounds().reduced(14);
    auto reconstructionTop = reconstruction.removeFromTop(44);
    importSong.setBounds(reconstructionTop.removeFromLeft(118).reduced(3));
    reconstructionTarget.setBounds(reconstructionTop.removeFromLeft(105).reduced(3));
    reconstructionStereoMode.setBounds(reconstructionTop.removeFromLeft(145).reduced(3));
    cancelReconstruction.setBounds(reconstructionTop.removeFromLeft(80).reduced(3));
    reconstructionHelp.setBounds(reconstruction.removeFromTop(34));
    auto reconstructionRegionRow = reconstruction.removeFromTop(44);
    const auto regionRowWidth = reconstructionRegionRow.getWidth();
    reconstructionRegion.setBounds(reconstructionRegionRow.removeFromLeft(regionRowWidth * 42 / 100).reduced(3));
    useReconstructionRegion.setBounds(reconstructionRegionRow.removeFromLeft(regionRowWidth * 18 / 100).reduced(3));
    reconstructionCandidate.setBounds(reconstructionRegionRow.removeFromLeft(regionRowWidth * 15 / 100).reduced(3));
    applyCandidate.setBounds(reconstructionRegionRow.removeFromLeft(regionRowWidth * 12 / 100).reduced(3));
    exportCandidate.setBounds(reconstructionRegionRow.reduced(3));
    reconstruction.removeFromTop(4);
    reconstructionView.setBounds(reconstruction);

    auto assistant = assistantPage.getLocalBounds().reduced(14);
    auto assistantTop = assistant.removeFromTop(44);
    assistantGoal.setBounds(assistantTop.removeFromLeft(170).reduced(3));
    assistantSuggestion.setBounds(assistantTop.removeFromLeft(150).reduced(3));
    assistantPreview.setBounds(assistantTop.removeFromLeft(84).reduced(3));
    assistantAccept.setBounds(assistantTop.removeFromLeft(78).reduced(3));
    assistantReject.setBounds(assistantTop.removeFromLeft(78).reduced(3));
    assistantUndo.setBounds(assistantTop.removeFromLeft(72).reduced(3));
    assistantPersonalization.setBounds(assistantTop.removeFromLeft(160).reduced(3));
    clearAssistantPreferences.setBounds(assistantTop.removeFromLeft(132).reduced(3));
    assistantHelp.setBounds(assistant.removeFromTop(34));
    assistant.removeFromTop(4);
    assistantView.setBounds(assistant);

    auto profiles = profilesPage.getLocalBounds().reduced(14);
    auto profileFilters = profiles.removeFromTop(42);
    profileSearch.setBounds(profileFilters.removeFromLeft(250).reduced(3));
    profileInstrument.setBounds(profileFilters.removeFromLeft(145).reduced(3));
    profileFavoritesOnly.setBounds(profileFilters.removeFromLeft(125).reduced(3));
    refreshProfiles.setBounds(profileFilters.removeFromLeft(84).reduced(3));
    auto profileSelection = profiles.removeFromTop(46);
    profileList.setBounds(profileSelection.removeFromLeft(330).reduced(3));
    applyProfile.setBounds(profileSelection.removeFromLeft(112).reduced(3));
    profileFavorite.setBounds(profileSelection.removeFromLeft(100).reduced(3));
    importProfile.setBounds(profileSelection.removeFromLeft(122).reduced(3));
    auto profileExport = profiles.removeFromTop(46);
    profileName.setBounds(profileExport.removeFromLeft(220).reduced(3));
    profileAuthor.setBounds(profileExport.removeFromLeft(170).reduced(3));
    exportProfile.setBounds(profileExport.removeFromLeft(152).reduced(3));
    profiles.removeFromTop(6);
    profileDetails.setBounds(profiles);
}

void TubeForgeAudioProcessorEditor::timerCallback()
{
    processor.refreshPhysicalCircuit();
    circuitSchematic.setSnapshot(processor.circuitTelemetrySnapshot(), processor.circuitStatusText(),
                                 processor.circuitGraphSnapshot());
    processor.refreshNonRealtimeDiagnostics();
    const auto& meters = processor.meterState();
    inputMeterValue = std::clamp(static_cast<double>(std::max(meters.inputPeak(0), meters.inputPeak(1))), 0.0, 1.0);
    outputMeterValue = std::clamp(static_cast<double>(std::max(meters.outputPeak(0), meters.outputPeak(1))), 0.0, 1.0);
    studioMeter.setLevels(static_cast<float>(inputMeterValue), static_cast<float>(outputMeterValue));
    const auto engineMode = static_cast<int>(std::lround(
        processor.getParameters().getRawParameterValue("engineMode")->load(std::memory_order_relaxed)));
    signalChain.setEngineMode(engineMode);
    toneAnalysisView.setResult(processor.toneAnalysisSnapshot(), processor.toneAnalysisStatusText(),
                               processor.toneNearestProfileText(), processor.toneProfileCount());
    const auto reconstruction = processor.reconstructionSnapshot();
    const auto playableRegions = processor.reconstructionRegionsSnapshot();
    juce::StringArray regionLabels;
    for (const auto& region : playableRegions)
    {
        const auto start = static_cast<int>(std::lround(region.quality.startSeconds));
        const auto end = static_cast<int>(std::lround(region.quality.endSeconds));
        const auto time = juce::String::formatted("%02d:%02d-%02d:%02d",
            start / 60, start % 60, end / 60, end % 60);
        const auto character = juce::String(nts::reconstruction::toString(region.gainCharacter).data()).toUpperCase();
        const auto cents = juce::String(region.tuningOffsetCents >= 0.0f ? "+" : "")
            + juce::String(region.tuningOffsetCents, 0) + "c";
        regionLabels.add(time + " | " + character + " | " + region.dominantPitch
            + " | " + region.estimatedTuning + " " + cents + " | "
            + juce::String(region.quality.confidence * 100.0f, 0) + "%");
    }
    const auto regionSignature = regionLabels.joinIntoString("\n");
    if (regionSignature != reconstructionRegionSignature)
    {
        reconstructionRegionSignature = regionSignature;
        reconstructionRegion.clear(juce::dontSendNotification);
        reconstructionRegion.addItemList(regionLabels, 1);
        auto selectedId = playableRegions.empty() ? 0 : 1;
        if (reconstruction)
            for (std::size_t index = 0; index < playableRegions.size(); ++index)
                if (std::abs(playableRegions[index].quality.startSeconds
                             - reconstruction->reference.regionStartSeconds) < 0.1)
                    selectedId = static_cast<int>(index + 1);
        reconstructionRegion.setSelectedId(selectedId, juce::dontSendNotification);
    }
    reconstructionView.setResult(reconstruction, processor.reconstructionStatusText(),
                                 processor.reconstructionProgress());
    const auto candidateCount = reconstruction ? static_cast<int>(reconstruction->candidates.size()) : 0;
    if (reconstructionCandidate.getNumItems() != candidateCount)
    {
        reconstructionCandidate.clear(juce::dontSendNotification);
        for (int index = 0; index < candidateCount; ++index)
            reconstructionCandidate.addItem("Candidate " + juce::String(index + 1), index + 1);
        if (candidateCount > 0) reconstructionCandidate.setSelectedId(1, juce::dontSendNotification);
    }
    processor.refreshAssistant();
    const auto recommendations = processor.assistantRecommendations();
    if (assistantSuggestion.getNumItems() != static_cast<int>(recommendations.size()))
    {
        assistantSuggestion.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < recommendations.size(); ++index)
            assistantSuggestion.addItem(juce::String(index + 1) + ". " + recommendations[index].diagnosis,
                                        static_cast<int>(index + 1));
        if (! recommendations.empty()) assistantSuggestion.setSelectedId(1, juce::dontSendNotification);
    }
    const auto selectedAssistant = std::max(0, assistantSuggestion.getSelectedId() - 1);
    assistantView.setRecommendations(recommendations, selectedAssistant, processor.assistantStatusText(),
                                     processor.assistantPreviewActive());
    assistantPersonalization.setToggleState(processor.assistantPersonalizationEnabled(), juce::dontSendNotification);
    const auto currentTab = ampTabs.getCurrentTabIndex();
    for (std::size_t index = 0; index < browserButtons.size(); ++index)
        browserButtons[index].setToggleState(static_cast<int>(index) == currentTab, juce::dontSendNotification);
    neuralStatus.setText(processor.neuralModelStatusText(), juce::dontSendNotification);
    const auto calibration = processor.neuralCalibrationReading();
    neuralCalibration.setText(
        "Input RMS: " + juce::String(calibration.rmsDb, 1) + " dBFS    Peak: "
        + juce::String(calibration.peakDb, 1) + " dBFS    Difference: "
        + juce::String(calibration.mismatchDb, 1) + " dB    "
        + (calibration.warning ? "LEVEL MISMATCH" : "Calibrated"), juce::dontSendNotification);
    neuralCalibration.setColour(juce::Label::textColourId,
        calibration.warning ? juce::Colours::orange : juce::Colours::lightgreen);

    const auto diagnostics = processor.diagnosticsSnapshot();
    const auto diagnosticSummary =
        "DSP " + juce::String(diagnostics.cpuLoadPercent, 1) + "%   |   callback "
        + juce::String(diagnostics.callbackMilliseconds, 3) + " ms   |   peak "
        + juce::String(diagnostics.maximumCallbackMilliseconds, 3) + " ms   |   latency "
        + juce::String(diagnostics.currentGraphLatencySamples) + " smp   |   dropouts "
        + juce::String(diagnostics.dropoutCount) + "   |   model " + statusName(diagnostics.modelLoadStatus)
        + "   |   IR " + statusName(diagnostics.irLoadStatus);
    diagnosticsText.setText(diagnosticSummary, juce::dontSendNotification);
    diagnosticsText.setTooltip(
        diagnosticSummary + "\nDeadline " + juce::String(diagnostics.deadlineMilliseconds, 3)
        + " ms; memory " + juce::String(static_cast<double>(diagnostics.workingSetBytes) / (1024.0 * 1024.0), 1)
        + " MiB; background failures " + juce::String(diagnostics.backgroundJobFailureCount));
}

void TubeForgeAudioProcessorEditor::chooseProjectToSave()
{
    fileChooser = std::make_unique<juce::FileChooser>("Save TubeForge project", juce::File {}, "*.tforge");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
                             {
                                 if (safeThis == nullptr || chooser.getResult() == juce::File {})
                                     return;
                                 const auto result = safeThis->processor.saveProject(chooser.getResult().withFileExtension("tforge"));
                                 if (result.failed())
                                     juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                            "Save failed", result.getErrorMessage());
                             });
}

void TubeForgeAudioProcessorEditor::chooseProjectToOpen()
{
    fileChooser = std::make_unique<juce::FileChooser>("Open TubeForge project", juce::File {}, "*.tforge");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
                             {
                                 if (safeThis == nullptr || chooser.getResult() == juce::File {})
                                     return;
                                 const auto result = safeThis->processor.loadProject(chooser.getResult());
                                 if (result.failed())
                                     juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                            "Open failed", result.getErrorMessage());
                             });
}

void TubeForgeAudioProcessorEditor::configureSlider(juce::Slider& slider)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 24);
}

void TubeForgeAudioProcessorEditor::chooseNeuralModel()
{
    fileChooser = std::make_unique<juce::FileChooser>("Choose exported neural model artifact");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestNeuralModelLoad(chooser.getResult());
        });
}

void TubeForgeAudioProcessorEditor::chooseToneClip()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose an isolated guitar or bass clip", juce::File {}, "*.wav;*.aif;*.aiff;*.flac;*.ogg");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestToneAnalysis(chooser.getResult());
        });
}

void TubeForgeAudioProcessorEditor::chooseSongForReconstruction()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choose a user-provided song", juce::File {}, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto target = safeThis->reconstructionTarget.getSelectedId() == 2
                ? nts::reconstruction::TargetInstrument::bass : nts::reconstruction::TargetInstrument::guitar;
            const auto modeIndex = std::clamp(safeThis->reconstructionStereoMode.getSelectedId() - 1, 0, 5);
            constexpr std::array modes { nts::reconstruction::StereoMode::fullStereo,
                nts::reconstruction::StereoMode::left, nts::reconstruction::StereoMode::right,
                nts::reconstruction::StereoMode::mid, nts::reconstruction::StereoMode::side,
                nts::reconstruction::StereoMode::pannedEstimate };
            safeThis->processor.requestSongReconstruction(chooser.getResult(), target,
                modes[static_cast<std::size_t>(modeIndex)]);
        });
}

void TubeForgeAudioProcessorEditor::chooseReconstructionExport()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Export reconstruction profile without source audio", juce::File {}, "*.tfreconstruct");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.exportReconstruction(
                chooser.getResult().withFileExtension("tfreconstruct"));
            if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Export failed", result.getErrorMessage());
        });
}

void TubeForgeAudioProcessorEditor::refreshPackageBrowser()
{
    nts::ecosystem::ProfileQuery query;
    query.searchText = profileSearch.getText().trim().toStdString();
    query.instrument = profileInstrument.getSelectedId() == 2 ? "guitar"
                     : profileInstrument.getSelectedId() == 3 ? "bass" : "";
    query.favoritesOnly = profileFavoritesOnly.getToggleState(); query.compatibleOnly = false;
    const auto previousId = profileList.getSelectedId() > 0
                         && profileList.getSelectedId() <= static_cast<int>(visibleProfiles.size())
        ? visibleProfiles[static_cast<std::size_t>(profileList.getSelectedId() - 1)].manifest.packageId
        : std::string {};
    visibleProfiles = processor.searchTonePackages(query);
    profileList.clear(juce::dontSendNotification);
    int selected {};
    for (std::size_t index = 0; index < visibleProfiles.size(); ++index)
    {
        const auto& value = visibleProfiles[index];
        profileList.addItem((value.favorite ? juce::String::fromUTF8("\xe2\x98\x85 ") : juce::String {})
                            + juce::String(value.manifest.name) + " — " + juce::String(value.manifest.author),
                            static_cast<int>(index + 1));
        if (value.manifest.packageId == previousId) selected = static_cast<int>(index + 1);
    }
    if (selected == 0 && ! visibleProfiles.empty()) selected = 1;
    profileList.setSelectedId(selected, juce::sendNotificationSync);
    if (visibleProfiles.empty())
    {
        profileFavorite.setToggleState(false, juce::dontSendNotification);
        profileDetails.setText("No matching profiles. Import a .ntone directory or export the current rig.",
                               juce::dontSendNotification);
    }
}

void TubeForgeAudioProcessorEditor::chooseTonePackageImport()
{
    fileChooser = std::make_unique<juce::FileChooser>("Choose a .ntone profile directory");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.importTonePackage(chooser.getResult());
            if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Import rejected", result.getErrorMessage());
            else safeThis->refreshPackageBrowser();
        });
}

void TubeForgeAudioProcessorEditor::chooseTonePackageExport()
{
    fileChooser = std::make_unique<juce::FileChooser>("Export current rig as a .ntone profile",
                                                       juce::File {}, "*.ntone");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            const auto result = safeThis->processor.exportCurrentTonePackage(
                chooser.getResult(), safeThis->profileName.getText(), safeThis->profileAuthor.getText());
            if (result.failed()) juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Export failed", result.getErrorMessage());
        });
}

void TubeForgeAudioProcessorEditor::configureKnob(juce::Slider& slider)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                               juce::MathConstants<float>::pi * 2.75f, true);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 20);
    slider.setPopupDisplayEnabled(true, false, nullptr);
    slider.setTooltip("Drag up/down or left/right; double-click the value to type an exact setting");
    slider.setColour(juce::Slider::textBoxTextColourId, juce::Colour::fromRGB(246, 220, 181));
    slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::black.withAlpha(0.58f));
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour::fromRGB(121, 77, 43));
}

void TubeForgeAudioProcessorEditor::configureHeading(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
    label.setJustificationType(juce::Justification::centredLeft);
}
