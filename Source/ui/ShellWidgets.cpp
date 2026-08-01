#include "ShellWidgets.h"
#include "../TubeForgeTheme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace theme = tf::theme;

namespace tf::ui
{
namespace
{
/// Every glyph is drawn in the same 24x24 box and scaled to fit wherever it is used, so a
/// 20px preset icon and a 34px navigation icon share one set of proportions and one weight.
constexpr float glyphBox = 24.0f;

void addStar(juce::Path& path, float centreX, float centreY, float radius)
{
    const auto waist = radius * 0.28f;
    path.startNewSubPath(centreX, centreY - radius);
    path.quadraticTo(centreX + waist, centreY - waist, centreX + radius, centreY);
    path.quadraticTo(centreX + waist, centreY + waist, centreX, centreY + radius);
    path.quadraticTo(centreX - waist, centreY + waist, centreX - radius, centreY);
    path.quadraticTo(centreX - waist, centreY - waist, centreX, centreY - radius);
    path.closeSubPath();
}

juce::Path buildGlyph(Glyph glyph)
{
    juce::Path path;
    switch (glyph)
    {
        case Glyph::amplifier:
            // An amp head, carrying handle and all: the handle is what stops a plain rounded
            // rectangle from reading as "some box" at 34 pixels.
            path.addRoundedRectangle(2.5f, 6.5f, 19.0f, 13.5f, 2.0f);
            path.startNewSubPath(9.5f, 6.5f);
            path.lineTo(9.5f, 4.0f);
            path.lineTo(14.5f, 4.0f);
            path.lineTo(14.5f, 6.5f);
            path.startNewSubPath(2.5f, 11.0f);
            path.lineTo(21.5f, 11.0f);
            for (const auto x : { 6.0f, 9.4f, 12.8f })
                path.addEllipse(x - 1.0f, 7.8f, 2.0f, 2.0f);
            break;

        case Glyph::toneShaping:
        {
            constexpr std::array handleY { 9.0f, 14.5f, 7.5f };
            for (std::size_t index = 0; index < handleY.size(); ++index)
            {
                const auto x = 5.0f + static_cast<float>(index) * 7.0f;
                path.startNewSubPath(x, 3.5f);
                path.lineTo(x, 20.5f);
                path.addRoundedRectangle(x - 3.0f, handleY[index] - 1.3f, 6.0f, 2.6f, 1.3f);
            }
            break;
        }

        case Glyph::cabinet:
            path.addRoundedRectangle(3.0f, 3.0f, 18.0f, 18.0f, 2.0f);
            path.startNewSubPath(3.0f, 6.5f);
            path.lineTo(21.0f, 6.5f);
            path.addEllipse(7.0f, 9.0f, 10.0f, 10.0f);
            path.addEllipse(10.6f, 12.6f, 2.8f, 2.8f);
            break;

        case Glyph::neuralCapture:
            // A chip. The node-graph version of this read as a pair of scissors once it was
            // scaled down to the size the toolbar actually uses it at.
            path.addRoundedRectangle(6.5f, 6.5f, 11.0f, 11.0f, 2.0f);
            path.addRoundedRectangle(10.0f, 10.0f, 4.0f, 4.0f, 1.0f);
            for (const auto offset : { 9.0f, 12.0f, 15.0f })
            {
                path.startNewSubPath(offset, 6.5f);
                path.lineTo(offset, 3.5f);
                path.startNewSubPath(offset, 17.5f);
                path.lineTo(offset, 20.5f);
                path.startNewSubPath(6.5f, offset);
                path.lineTo(3.5f, offset);
                path.startNewSubPath(17.5f, offset);
                path.lineTo(20.5f, offset);
            }
            break;

        case Glyph::toneAssistant:
            addStar(path, 10.0f, 10.0f, 7.0f);
            addStar(path, 18.0f, 17.8f, 3.6f);
            break;

        case Glyph::profileLibrary:
            path.addRoundedRectangle(3.5f, 3.5f, 7.4f, 7.4f, 1.6f);
            path.addRoundedRectangle(13.1f, 3.5f, 7.4f, 7.4f, 1.6f);
            path.addRoundedRectangle(3.5f, 13.1f, 7.4f, 7.4f, 1.6f);
            path.addRoundedRectangle(13.1f, 13.1f, 7.4f, 7.4f, 1.6f);
            break;

        case Glyph::circuit:
            path.addRoundedRectangle(7.0f, 2.5f, 10.0f, 14.5f, 5.0f);
            path.addRoundedRectangle(7.8f, 15.2f, 8.4f, 3.8f, 1.0f);
            for (const auto x : { 9.8f, 12.0f, 14.2f })
            {
                path.startNewSubPath(x, 19.0f);
                path.lineTo(x, 21.5f);
            }
            path.startNewSubPath(10.0f, 11.5f);
            path.lineTo(12.0f, 7.0f);
            path.lineTo(14.0f, 11.5f);
            break;

        case Glyph::toneAnalyzer:
            path.addEllipse(3.5f, 3.5f, 13.0f, 13.0f);
            path.startNewSubPath(15.6f, 15.6f);
            path.lineTo(20.5f, 20.5f);
            path.startNewSubPath(7.6f, 12.2f);
            path.lineTo(7.6f, 7.8f);
            path.startNewSubPath(10.0f, 14.0f);
            path.lineTo(10.0f, 6.0f);
            path.startNewSubPath(12.4f, 12.6f);
            path.lineTo(12.4f, 7.4f);
            break;

        case Glyph::songMatch:
            path.addEllipse(3.6f, 15.0f, 5.6f, 4.6f);
            path.addEllipse(14.4f, 12.8f, 5.6f, 4.6f);
            path.startNewSubPath(9.2f, 17.3f);
            path.lineTo(9.2f, 5.2f);
            path.startNewSubPath(20.0f, 15.1f);
            path.lineTo(20.0f, 3.0f);
            path.startNewSubPath(9.2f, 5.2f);
            path.lineTo(20.0f, 3.0f);
            path.startNewSubPath(9.2f, 8.4f);
            path.lineTo(20.0f, 6.2f);
            break;

        case Glyph::save:
            path.startNewSubPath(4.5f, 4.5f);
            path.lineTo(16.0f, 4.5f);
            path.lineTo(19.5f, 8.0f);
            path.lineTo(19.5f, 19.5f);
            path.lineTo(4.5f, 19.5f);
            path.closeSubPath();
            path.addRoundedRectangle(8.0f, 4.5f, 6.8f, 4.4f, 0.6f);
            path.addRoundedRectangle(7.6f, 12.6f, 8.8f, 6.9f, 0.6f);
            break;

        case Glyph::open:
            path.startNewSubPath(4.5f, 14.0f);
            path.lineTo(4.5f, 19.5f);
            path.lineTo(19.5f, 19.5f);
            path.lineTo(19.5f, 14.0f);
            path.startNewSubPath(12.0f, 3.5f);
            path.lineTo(12.0f, 14.6f);
            path.startNewSubPath(7.8f, 10.4f);
            path.lineTo(12.0f, 14.6f);
            path.lineTo(16.2f, 10.4f);
            break;

        case Glyph::browse:
            path.addEllipse(3.8f, 3.8f, 12.6f, 12.6f);
            path.startNewSubPath(15.4f, 15.4f);
            path.lineTo(20.4f, 20.4f);
            break;

        case Glyph::previous:
            path.startNewSubPath(14.2f, 5.5f);
            path.lineTo(8.8f, 12.0f);
            path.lineTo(14.2f, 18.5f);
            break;

        case Glyph::next:
            path.startNewSubPath(9.8f, 5.5f);
            path.lineTo(15.2f, 12.0f);
            path.lineTo(9.8f, 18.5f);
            break;

        case Glyph::settings:
            path.addEllipse(8.6f, 8.6f, 6.8f, 6.8f);
            for (int tooth = 0; tooth < 8; ++tooth)
            {
                const auto a = juce::MathConstants<float>::twoPi * static_cast<float>(tooth) / 8.0f;
                const auto sine = std::sin(a);
                const auto cosine = std::cos(a);
                path.startNewSubPath(12.0f + sine * 7.0f, 12.0f - cosine * 7.0f);
                path.lineTo(12.0f + sine * 9.4f, 12.0f - cosine * 9.4f);
            }
            break;
    }
    return path;
}
} // namespace

juce::Path glyphPath(Glyph glyph, juce::Rectangle<float> area)
{
    auto path = buildGlyph(glyph);
    const auto size = std::min(area.getWidth(), area.getHeight());
    path.applyTransform(juce::AffineTransform::scale(size / glyphBox)
                            .translated(area.getCentreX() - size * 0.5f,
                                        area.getCentreY() - size * 0.5f));
    return path;
}

IconButton::IconButton(Glyph glyphToDraw, const juce::String& name, bool marksActive)
    : juce::Button(name), glyph(glyphToDraw), underlinesActive(marksActive)
{
    setClickingTogglesState(false);
}

void IconButton::paintButton(juce::Graphics& graphics, bool isMouseOver, bool isButtonDown)
{
    const auto active = getToggleState();
    const auto area = getLocalBounds().toFloat();

    if (isMouseOver || isButtonDown)
    {
        graphics.setColour(juce::Colours::white.withAlpha(isButtonDown ? 0.08f : 0.045f));
        graphics.fillRoundedRectangle(area.reduced(1.0f), 5.0f);
    }

    auto icon = area.reduced(area.getWidth() * 0.24f, area.getHeight() * 0.24f);
    if (underlinesActive)
        icon = icon.withTrimmedBottom(3.0f);

    const auto size = std::min(icon.getWidth(), icon.getHeight());
    const auto tint = ! isEnabled()  ? theme::textTertiary.withAlpha(0.45f)
                    : active         ? theme::accent
                    : isMouseOver    ? theme::textPrimary.withAlpha(0.85f)
                                     : theme::textSecondary.withAlpha(0.80f);
    graphics.setColour(tint);
    graphics.strokePath(glyphPath(glyph, icon),
                        juce::PathStrokeType(std::max(1.0f, size / glyphBox * 1.6f),
                                             juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    if (underlinesActive && active)
    {
        graphics.setColour(theme::accent);
        graphics.fillRoundedRectangle(juce::Rectangle<float>(16.0f, 2.0f).withCentre(
            { area.getCentreX(), area.getBottom() - 3.0f }), 1.0f);
    }
}

void StudioSignalChain::setEngineMode(int mode)
{
    if (activeEngineMode == mode) return;
    activeEngineMode = mode;
    repaint();
}

void StudioSignalChain::paint(juce::Graphics& graphics)
{
    constexpr std::array stages { "INPUT", "GATE", "PREAMP", "TONE", "POWER", "CABINET", "OUTPUT" };
    // The three stages in the middle are the ones the engine mode actually swaps out.
    constexpr int firstCoreStage = 2;
    constexpr int lastCoreStage = 4;
    constexpr float tracking = 1.4f;
    constexpr float gap = 9.0f;

    const auto stageFont = theme::font(8.5f, true);
    const auto separatorWidth = juce::GlyphArrangement::getStringWidth(stageFont, ">");

    auto total = 0.0f;
    for (std::size_t index = 0; index < stages.size(); ++index)
        total += theme::trackedWidth(stageFont, stages[index], tracking)
               + (index + 1 < stages.size() ? separatorWidth + gap * 2.0f : 0.0f);

    graphics.setFont(stageFont);
    auto x = static_cast<float>(getLocalBounds().getCentreX()) - total * 0.5f;
    const auto row = getLocalBounds();

    for (std::size_t index = 0; index < stages.size(); ++index)
    {
        const auto isCore = static_cast<int>(index) >= firstCoreStage
                         && static_cast<int>(index) <= lastCoreStage;
        const auto width = theme::trackedWidth(stageFont, stages[index], tracking);
        graphics.setColour(isCore ? theme::accent : theme::textTertiary);
        theme::tracked(graphics, stages[index],
                       row.withX(juce::roundToInt(x)).withWidth(juce::roundToInt(width) + 2),
                       juce::Justification::centredLeft, tracking);
        x += width;

        if (index + 1 < stages.size())
        {
            x += gap;
            graphics.setColour(theme::hairline.brighter(0.15f));
            graphics.drawSingleLineText(">", juce::roundToInt(x),
                                        row.getCentreY() + juce::roundToInt(stageFont.getAscent() * 0.5f) - 1);
            x += separatorWidth + gap;
        }
    }
}

void LevelBar::setLevel(float newLevel)
{
    const auto clamped = std::clamp(newLevel, 0.0f, 1.0f);
    if (std::abs(clamped - level) < 0.002f) return;
    level = clamped;
    repaint();
}

void LevelBar::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat();
    const auto radius = area.getHeight() * 0.5f;
    graphics.setColour(theme::hairline);
    graphics.fillRoundedRectangle(area, radius);
    if (level <= 0.002f) return;

    // The accent all the way up and red at the top. There is no amber step in between: against
    // a golden accent it would read as a brighter signal rather than as a warning.
    graphics.setColour(level > 0.97f ? theme::bad : theme::accent);
    graphics.fillRoundedRectangle(area.withWidth(std::max(area.getHeight(), area.getWidth() * level)),
                                  radius);
}
} // namespace tf::ui
