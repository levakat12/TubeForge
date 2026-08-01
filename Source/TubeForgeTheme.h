#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/// Shared visual language for the TubeForge editor.
///
/// The whole window is built from four flat tones -- chrome, panel, stage, and a single
/// hairline -- plus near-white text. There are no gradients, bevels or glows: depth comes
/// from one-pixel rules and from how much light a surface reflects, the way a studio
/// front panel does. Anything drawn by hand (meters, plots, schematics) pulls its colours
/// from here rather than inventing another near-identical grey.
namespace tf::theme
{
/// Warm charcoal rather than blue-black: a neutral grey turns faintly cyan next to the gold,
/// which is what makes an amber-on-grey interface look cheap.
inline const juce::Colour backdropTop { 0xff1b1917 };
inline const juce::Colour backdropBottom { 0xff100f0e };
inline const juce::Colour shell { 0xff161514 };
inline const juce::Colour panel { 0xff1b1a18 };
inline const juce::Colour panelRaised { 0xff242220 };
inline const juce::Colour sunken { 0xff0b0a09 };
inline const juce::Colour hairline { 0xff322f2b };

/// One accent, and it is the only hue in the interface: golden orange, the colour of a lit
/// valve. Everything that means "this is live, this is the value, this is where you are" uses
/// it, and nothing else does -- so it never has to compete for attention.
inline const juce::Colour accent { 0xfff0a340 };
inline const juce::Colour accentDim { 0xff8f6a35 };
inline const juce::Colour accentWash { 0xff2e2417 };

inline const juce::Colour good { 0xff5cbf92 };
/// Yellow rather than amber, so a warning cannot be mistaken for the accent.
inline const juce::Colour warn { 0xffe3c04f };
inline const juce::Colour bad { 0xffdc5348 };

inline const juce::Colour textPrimary { 0xfff3f0ea };
inline const juce::Colour textSecondary { 0xffa09a90 };
inline const juce::Colour textTertiary { 0xff67625a };

[[nodiscard]] inline juce::Font font(float height, bool bold = false)
{
    return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}

/// A flat panel that sits on the backdrop: one fill, one hairline. `raised` is the hover /
/// emphasis variant -- it lightens the fill rather than adding an edge, so a row of them
/// stays quiet until you point at one.
inline void glass(juce::Graphics& graphics, juce::Rectangle<float> area, float radius = 8.0f,
                  bool raised = false)
{
    if (area.isEmpty()) return;
    graphics.setColour(raised ? panelRaised : panel);
    graphics.fillRoundedRectangle(area, radius);
    graphics.setColour(hairline);
    graphics.drawRoundedRectangle(area.reduced(0.5f), radius, 1.0f);
}

/// A recessed well for content that lives inside a panel: meters, plots, lists, readouts.
inline void well(juce::Graphics& graphics, juce::Rectangle<float> area, float radius = 5.0f)
{
    if (area.isEmpty()) return;
    graphics.setColour(sunken);
    graphics.fillRoundedRectangle(area, radius);
    graphics.setColour(hairline.withAlpha(0.75f));
    graphics.drawRoundedRectangle(area.reduced(0.5f), radius, 1.0f);
}

/// The main content area: the darkest surface in the window, so the chrome around it reads
/// as a frame and the module you are actually working in reads as the subject.
inline void stage(juce::Graphics& graphics, juce::Rectangle<float> area, float radius = 8.0f)
{
    if (area.isEmpty()) return;
    graphics.setColour(sunken);
    graphics.fillRoundedRectangle(area, radius);
    graphics.setColour(hairline.withAlpha(0.60f));
    graphics.drawRoundedRectangle(area.reduced(0.5f), radius, 1.0f);
}

/// A one-pixel rule. Separators are how this interface groups things; it has no boxes.
inline void rule(juce::Graphics& graphics, juce::Rectangle<int> area, float alpha = 1.0f)
{
    graphics.setColour(hairline.withAlpha(alpha));
    graphics.fillRect(area);
}

/// A short vertical divider between two clusters of controls, inset from the top and bottom
/// of the strip it sits in so it separates without drawing a full-height line.
inline void divider(juce::Graphics& graphics, int x, int centreY, int height, float alpha = 0.9f)
{
    graphics.setColour(hairline.brighter(0.12f).withAlpha(alpha));
    graphics.fillRect(x, centreY - height / 2, 1, height);
}

[[nodiscard]] inline float trackedWidth(const juce::Font& usedFont, const juce::String& text,
                                        float tracking = 1.5f)
{
    if (text.isEmpty()) return 0.0f;
    auto width = -tracking;
    for (int index = 0; index < text.length(); ++index)
        width += juce::GlyphArrangement::getStringWidth(usedFont, text.substring(index, index + 1)) + tracking;
    return width;
}

/// Small-caps text with manual letter tracking. JUCE has no tracking of its own, and
/// untracked 9px capitals look cramped and cheap at the sizes the section headings use.
inline void tracked(juce::Graphics& graphics, const juce::String& text, juce::Rectangle<int> area,
                    juce::Justification justification, float tracking = 1.5f)
{
    if (text.isEmpty()) return;
    const auto currentFont = graphics.getCurrentFont();
    const auto width = trackedWidth(currentFont, text, tracking);

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
