#include "ShellWidgets.h"
#include "../TubeForgeTheme.h"

#include <algorithm>

namespace theme = tf::theme;

namespace tf::ui
{
GearBrowserItem::GearBrowserItem(const juce::String& name, juce::String description)
    : juce::Button(name), tag(std::move(description))
{
    setClickingTogglesState(false);
}

void GearBrowserItem::paintButton(juce::Graphics& graphics, bool isMouseOver, bool isButtonDown)
{
    const auto active = getToggleState();
    auto area = getLocalBounds().toFloat().reduced(0.5f);

    if (active || isMouseOver)
    {
        graphics.setColour(active ? theme::accentWash : juce::Colours::white.withAlpha(0.04f));
        graphics.fillRoundedRectangle(area, 7.0f);
    }
    if (active)
    {
        graphics.setColour(theme::accent.withAlpha(isButtonDown ? 0.75f : 1.0f));
        graphics.fillRoundedRectangle(area.withWidth(3.0f).reduced(0.0f, 6.0f), 1.5f);
    }

    auto labels = getLocalBounds().reduced(12, 5);
    graphics.setColour(active ? theme::textPrimary : theme::textSecondary);
    graphics.setFont(theme::font(12.5f, active));
    graphics.drawFittedText(getButtonText(), labels.removeFromTop(16), juce::Justification::centredLeft, 1);
    graphics.setColour(active ? theme::accent.withAlpha(0.85f) : theme::textTertiary);
    graphics.setFont(theme::font(9.5f));
    graphics.drawFittedText(tag, labels, juce::Justification::topLeft, 1);
}

void StudioSignalChain::setEngineMode(int mode)
{
    if (activeEngineMode == mode) return;
    activeEngineMode = mode;
    repaint();
}

void StudioSignalChain::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    theme::glass(graphics, area, 9.0f);
    theme::caption(graphics, area.reduced(14.0f, 0.0f).removeFromTop(22.0f).toNearestInt(), "Signal chain");

    constexpr std::array moduleNames { "INPUT", "GATE / EQ", "PREAMP", "TONE", "POWER", "CABINET", "OUTPUT" };
    auto modules = area.reduced(14.0f, 0.0f).withTrimmedTop(22.0f).withTrimmedBottom(10.0f);
    constexpr auto gap = 7.0f;
    const auto width = (modules.getWidth() - gap * static_cast<float>(moduleNames.size() - 1))
                     / static_cast<float>(moduleNames.size());
    for (std::size_t index = 0; index < moduleNames.size(); ++index)
    {
        auto card = juce::Rectangle<float>(modules.getX() + static_cast<float>(index) * (width + gap),
                                            modules.getY(), width, modules.getHeight());
        // The three stages in the middle are the ones the engine mode actually swaps out.
        const auto isCore = index >= 2 && index <= 4;
        auto tint = theme::accent;
        if (activeEngineMode == 1) tint = juce::Colour(0xff56b8d8);
        if (activeEngineMode == 2) tint = juce::Colour(0xffa98ae0);
        theme::glass(graphics, card, 6.0f, isCore);
        if (isCore)
        {
            graphics.setColour(tint.withAlpha(0.10f));
            graphics.fillRoundedRectangle(card, 6.0f);
            graphics.setColour(tint.withAlpha(0.55f));
            graphics.drawRoundedRectangle(card.reduced(0.5f), 6.0f, 1.0f);
        }
        graphics.setColour(isCore ? theme::textPrimary : theme::textSecondary);
        graphics.setFont(theme::font(9.5f, true));
        theme::tracked(graphics, moduleNames[index], card.toNearestInt(), juce::Justification::centred);
        graphics.setColour(isCore ? tint : theme::good.withAlpha(0.7f));
        graphics.fillEllipse(card.getRight() - 11.0f, card.getY() + 6.0f, 4.0f, 4.0f);
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
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    theme::glass(graphics, area, 9.0f);
    theme::caption(graphics, area.reduced(0.0f, 0.0f).removeFromTop(24.0f).toNearestInt(), "Levels",
                   theme::textTertiary, juce::Justification::centred);

    const auto drawMeter = [&graphics](juce::Rectangle<float> meter, float value, const juce::String& name)
    {
        auto label = meter.removeFromBottom(18.0f);
        theme::well(graphics, meter, 3.0f);
        const auto fill = meter.reduced(2.0f).withTop(meter.getBottom() - 2.0f
                                                      - (meter.getHeight() - 4.0f) * value);
        if (value > 0.001f)
        {
            juce::ColourGradient gradient(theme::good, fill.getBottomLeft(), theme::bad, meter.getTopLeft(), false);
            gradient.addColour(0.72, theme::accent);
            graphics.setGradientFill(gradient);
            graphics.fillRoundedRectangle(fill, 2.0f);
        }
        graphics.setColour(theme::textTertiary);
        graphics.setFont(theme::font(8.5f, true));
        theme::tracked(graphics, name, label.toNearestInt(), juce::Justification::centred);
    };
    auto meters = area.withTrimmedTop(24.0f).reduced(10.0f, 6.0f);
    constexpr auto meterWidth = 16.0f;
    drawMeter(juce::Rectangle<float>(meterWidth, meters.getHeight()).withCentre(
                  { meters.getCentreX() - 15.0f, meters.getCentreY() }), inputLevel, "IN");
    drawMeter(juce::Rectangle<float>(meterWidth, meters.getHeight()).withCentre(
                  { meters.getCentreX() + 15.0f, meters.getCentreY() }), outputLevel, "OUT");
}
} // namespace tf::ui
