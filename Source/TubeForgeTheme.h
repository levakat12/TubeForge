#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/// Shared visual language for the TubeForge editor. Every surface is built from the same
/// small set of colours and the same two recipes -- a raised "glass" panel and a recessed
/// "well" -- so the window reads as one instrument instead of a pile of debug widgets.
/// Anything drawn by hand (meters, plots, schematics) pulls its colours from here rather
/// than inventing another near-identical grey.
namespace tf::theme
{
inline const juce::Colour backdropTop { 0xff1a1e26 };
inline const juce::Colour backdropBottom { 0xff0a0b0e };
inline const juce::Colour shell { 0xff0d0f14 };
inline const juce::Colour panel { 0xff151920 };
inline const juce::Colour panelRaised { 0xff1c212a };
inline const juce::Colour sunken { 0xff090b0e };
inline const juce::Colour hairline { 0xff272d37 };

inline const juce::Colour accent { 0xffff9a45 };
inline const juce::Colour accentDim { 0xffb96a2e };
inline const juce::Colour accentWash { 0xff33210f };

inline const juce::Colour good { 0xff4fd08a };
inline const juce::Colour warn { 0xfff0a94b };
inline const juce::Colour bad { 0xffe8574b };

inline const juce::Colour textPrimary { 0xffe6e9ef };
inline const juce::Colour textSecondary { 0xff98a1ae };
inline const juce::Colour textTertiary { 0xff5f6875 };

[[nodiscard]] inline juce::Font font(float height, bool bold = false)
{
    return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}

/// The one glass recipe: a soft top-lit gradient, a light seam on the edge, and a dark
/// halo just outside it. Used for every panel that sits on the backdrop.
inline void glass(juce::Graphics& graphics, juce::Rectangle<float> area, float radius = 10.0f,
                  bool raised = false)
{
    if (area.isEmpty()) return;
    const auto base = raised ? panelRaised : panel;
    graphics.setGradientFill(juce::ColourGradient(base.brighter(0.10f), area.getX(), area.getY(),
                                                  base.darker(0.24f), area.getX(), area.getBottom(), false));
    graphics.fillRoundedRectangle(area, radius);
    graphics.setColour(juce::Colours::black.withAlpha(0.40f));
    graphics.drawRoundedRectangle(area.expanded(0.5f), radius + 0.5f, 1.0f);
    graphics.setColour(juce::Colours::white.withAlpha(0.06f));
    graphics.drawRoundedRectangle(area.reduced(0.5f), radius, 1.0f);
}

/// A recessed well for content that lives inside a panel: meters, plots, lists, readouts.
inline void well(juce::Graphics& graphics, juce::Rectangle<float> area, float radius = 6.0f)
{
    if (area.isEmpty()) return;
    graphics.setGradientFill(juce::ColourGradient(sunken.darker(0.30f), area.getX(), area.getY(),
                                                  sunken.brighter(0.16f), area.getX(), area.getBottom(), false));
    graphics.fillRoundedRectangle(area, radius);
    graphics.setColour(juce::Colours::black.withAlpha(0.55f));
    graphics.drawRoundedRectangle(area.reduced(0.5f), radius, 1.0f);
}

/// Small-caps text with manual letter tracking. JUCE has no tracking of its own, and
/// untracked 9px capitals look cramped and cheap at the sizes the section headings use.
inline void tracked(juce::Graphics& graphics, const juce::String& text, juce::Rectangle<int> area,
                    juce::Justification justification, float tracking = 1.5f)
{
    if (text.isEmpty()) return;
    const auto currentFont = graphics.getCurrentFont();
    auto width = -tracking;
    for (int index = 0; index < text.length(); ++index)
        width += juce::GlyphArrangement::getStringWidth(currentFont, text.substring(index, index + 1)) + tracking;

    auto x = static_cast<float>(area.getX());
    if (justification.testFlags(juce::Justification::horizontallyCentred))
        x += (static_cast<float>(area.getWidth()) - width) * 0.5f;
    else if (justification.testFlags(juce::Justification::right))
        x += static_cast<float>(area.getWidth()) - width;

    const auto baseline = static_cast<float>(area.getCentreY())
                        + (currentFont.getAscent() - currentFont.getDescent()) * 0.5f;
    for (int index = 0; index < text.length(); ++index)
    {
        const auto glyph = text.substring(index, index + 1);
        graphics.drawSingleLineText(glyph, juce::roundToInt(x), juce::roundToInt(baseline));
        x += juce::GlyphArrangement::getStringWidth(currentFont, glyph) + tracking;
    }
}

/// The standard section heading: tracked small capitals in the quietest text weight.
inline void caption(juce::Graphics& graphics, juce::Rectangle<int> area, const juce::String& text,
                    juce::Colour colour = textTertiary,
                    juce::Justification justification = juce::Justification::centredLeft)
{
    graphics.setColour(colour);
    graphics.setFont(font(9.0f, true));
    tracked(graphics, text.toUpperCase(), area, justification);
}
} // namespace tf::theme
