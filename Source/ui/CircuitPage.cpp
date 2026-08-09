#include "CircuitPage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

#include <nts/circuit/Components.h>

#include <algorithm>
#include <cmath>

namespace theme = tf::theme;

namespace
{
/* `circuitCabinetStyle` is deliberately absent.

   The circuit engine used to synthesise a cabinet node of its own, while the impulse-response
   cabinet lived inside the traditional amplifier -- so the two engines had different speakers and
   a loaded response did nothing on this one. The cabinet is a stage after all three engines now,
   and this page offering a second one would be offering two cabinets in series.

   The *parameter* still exists and is still saved: removing it would shift every id after it in
   `ampControlIds` and silently corrupt every saved project (see the note at that list). What is
   removed is the control, because a control that does nothing is worse than no control. */
constexpr std::array selectorIds { "circuitPreampTube", "circuitPowerTube", "circuitPowerTopology",
                                   "circuitToneStack", "circuitBackend" };
constexpr std::array selectorCaptions { "Preamp tube", "Power tube", "Power topology",
                                        "Tone stack", "Solver" };
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
    auto bounds = getLocalBounds().toFloat();
    theme::glass(graphics, bounds.reduced(0.5f), 9.0f);
    bounds = bounds.reduced(14.0f, 12.0f);
    theme::caption(graphics, bounds.removeFromTop(22.0f).toNearestInt(), "Signal path", theme::textSecondary);

    auto schematic = bounds.removeFromTop(180.0f);
    const auto nodeCount = std::max<std::size_t>(1, circuit.nodes.size());
    const auto cellWidth = schematic.getWidth() / static_cast<float>(nodeCount);
    for (std::size_t index = 0; index < circuit.nodes.size(); ++index)
    {
        const auto& node = circuit.nodes[index];
        const auto centreX = schematic.getX() + cellWidth * (static_cast<float>(index) + 0.5f);
        const auto box = juce::Rectangle<float>(cellWidth - 10.0f, 48.0f)
            .withCentre(juce::Point<float> { centreX, schematic.getY() + 46.0f });
        if (index + 1 < circuit.nodes.size())
        {
            graphics.setColour(theme::accentDim.withAlpha(0.7f));
            graphics.drawArrow({ box.getRight() + 1.0f, box.getCentreY(), box.getRight() + 9.0f, box.getCentreY() },
                               1.2f, 5.0f, 5.0f);
        }
        theme::glass(graphics, box, 6.0f, true);
        graphics.setColour(theme::textPrimary);
        graphics.setFont(theme::font(10.0f, true));
        // drawFittedText, not tracked(): node names vary in length and a fixed truncation
        // left "TONE-STAC" and "TRANSFORM" spilling out of their boxes.
        auto nodeName = juce::String(nts::circuit::toString(node.type).data()).toUpperCase();
        nodeName = nodeName.replace("STAGE", "").replace("-", " ").trim();
        graphics.drawFittedText(nodeName, box.toNearestInt().reduced(4, 0),
                                juce::Justification::centred, 2, 0.7f);
        const auto telemetry = std::find_if(stages.begin(), stages.end(), [&node](const auto& stage)
        { return stage.nodeId == node.id; });
        if (telemetry != stages.end())
        {
            const auto& meter = telemetry->values;
            // A healthy stage is quiet. Eleven green "OK"s in a row say nothing and drown out
            // the one node that is actually in trouble.
            graphics.setColour(meter.valid ? theme::textTertiary : theme::warn);
            graphics.setFont(theme::font(9.5f));
            graphics.drawFittedText(juce::String(meter.outputRms, 3) + " rms\n" + (meter.valid ? "OK" : "CHECK"),
                                    box.translated(0.0f, 52.0f).withHeight(32.0f).toNearestInt(),
                                    juce::Justification::centred, 2);
        }
    }
    graphics.setColour(theme::textSecondary);
    graphics.setFont(theme::font(11.0f));
    const auto triode = std::find_if(circuit.nodes.begin(), circuit.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::triodeStage; });
    const auto power = std::find_if(circuit.nodes.begin(), circuit.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::powerStage; });
    const auto tone = std::find_if(circuit.nodes.begin(), circuit.nodes.end(), [](const auto& node)
    { return node.type == nts::circuit::NodeType::toneStack; });
    const auto details = juce::String(triode == circuit.nodes.end() ? "No preamp" : triode->modelId)
        + "   /   " + (power == circuit.nodes.end() ? "No power stage" : juce::String(power->modelId))
        + "   /   " + (tone == circuit.nodes.end() ? "No tone stack" : juce::String(tone->modelId));
    graphics.drawFittedText(details, schematic.removeFromBottom(26.0f).toNearestInt(), juce::Justification::centred, 1);

    auto graphArea = bounds.removeFromTop(std::min(140.0f, bounds.getHeight() - 26.0f)).reduced(2.0f, 6.0f);
    theme::well(graphics, graphArea, 6.0f);
    auto plot = graphArea.reduced(10.0f, 8.0f);
    juce::Path response;
    for (int pixel = 0; pixel < static_cast<int>(plot.getWidth()); ++pixel)
    {
        const auto normalized = static_cast<float>(pixel) / std::max(1.0f, plot.getWidth() - 1.0f);
        const auto x = plot.getX() + static_cast<float>(pixel);
        const auto frequency = 20.0 * std::pow(1000.0, static_cast<double>(normalized));
        const auto magnitude = tone == circuit.nodes.end() ? 1.0
            : nts::circuit::toneStackMagnitude(*tone, frequency, 48000.0);
        const auto decibels = std::clamp(20.0 * std::log10(std::max(1.0e-6, magnitude)), -30.0, 10.0);
        const auto y = plot.getBottom() - plot.getHeight() * static_cast<float>((decibels + 30.0) / 40.0);
        if (pixel == 0) response.startNewSubPath(x, y); else response.lineTo(x, y);
    }
    graphics.setColour(theme::accent.withAlpha(0.22f));
    graphics.strokePath(response, juce::PathStrokeType(5.0f));
    graphics.setColour(theme::accent);
    graphics.strokePath(response, juce::PathStrokeType(1.8f));
    graphics.setColour(theme::textTertiary);
    graphics.setFont(theme::font(9.5f));
    graphics.drawText("20 Hz", graphArea.reduced(8.0f, 4.0f), juce::Justification::bottomLeft);
    graphics.drawText("20 kHz", graphArea.reduced(8.0f, 4.0f), juce::Justification::bottomRight);

    const auto trouble = statusText.containsIgnoreCase("failed") || statusText.containsIgnoreCase("invalid");
    graphics.setColour(trouble ? theme::warn : theme::textSecondary);
    graphics.setFont(theme::font(11.5f, true));
    graphics.drawFittedText(statusText, bounds.toNearestInt(), juce::Justification::centredLeft, 2);
}

CircuitPage::CircuitPage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    const std::array selectors { &preampTube, &powerTube, &powerTopology,
                                 &toneStack, &backend };
    for (std::size_t index = 0; index < captions.size(); ++index)
    {
        tf::ui::configureFieldCaption(captions[index], selectorCaptions[index]);
        tf::ui::populateFromParameter(*selectors[index], processor.getParameters(), selectorIds[index]);
        addAndMakeVisible(captions[index]);
        addAndMakeVisible(*selectors[index]);
    }
    addAndMakeVisible(schematic);

    for (std::size_t index = 0; index < attachments.size(); ++index)
        attachments[index] = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processor.getParameters(), selectorIds[index], *selectors[index]);
}

void CircuitPage::resized()
{
    auto area = getLocalBounds();
    auto selectorArea = area.removeFromTop(94);
    const std::array selectors { &preampTube, &powerTube, &powerTopology,
                                 &toneStack, &backend };
    for (std::size_t index = 0; index < selectors.size(); ++index)
    {
        const auto row = static_cast<int>(index / 3);
        const auto column = static_cast<int>(index % 3);
        auto cell = juce::Rectangle<int>(selectorArea.getX() + column * selectorArea.getWidth() / 3,
                                          selectorArea.getY() + row * selectorArea.getHeight() / 2,
                                          selectorArea.getWidth() / 3,
                                          selectorArea.getHeight() / 2).reduced(6, 3);
        tf::ui::layOutField(cell, captions[index], *selectors[index]);
    }
    area.removeFromTop(4);
    schematic.setBounds(area);
}

void CircuitPage::refresh()
{
    schematic.setSnapshot(processor.circuitTelemetrySnapshot(), processor.circuitStatusText(),
                          processor.circuitGraphSnapshot());
}
