#pragma once

#include "../TubeForgeTheme.h"

#include <juce_gui_basics/juce_gui_basics.h>

class TubeForgeAudioProcessor;

/// Base class for the editor's module pages.
///
/// A page owns everything inside it: its controls, its parameter attachments, its file
/// choosers and its own layout. The shell only ever gives a page bounds and ticks it, and
/// pages never reach into one another -- anything that has to cross a page boundary leaves
/// through a std::function on the page that owns the data.
class ModulePage : public juce::Component
{
public:
    explicit ModulePage(TubeForgeAudioProcessor& processorToUse) : processor(processorToUse) {}

    /// Pulls fresh engine state into this page's views. The shell calls it from its timer,
    /// but only while the page is showing, and once immediately when the page is opened.
    ///
    /// This is for *display* only. Engine work that has to happen whether or not anyone is
    /// looking -- recompiling the physical circuit, draining the assistant's audio-thread
    /// queue -- stays in the shell's tick, because gating it on visibility would silently
    /// stop it when the user switched pages.
    virtual void refresh() {}

    /** Tells a page which of its controls the performance tier has taken over.

        Pushed from the shell rather than pulled, because the tier is a rig-wide setting and a
        page has no business polling it. Default is to do nothing: most pages own no control the
        tier touches, and they should not have to say so.

        These are limits, not values. The tier caps what the engine derives from a control and
        never rewrites what is stored, so a page must grey a control out rather than move it --
        raising the tier has to bring it back exactly where the user left it.
    */
    virtual void setPerformanceLimits(int /*maximumOversamplingFactor*/, bool /*singleCabinet*/) {}

    /** Veils the page while a song match is running, with the reason written across it.

        A warning rather than a lock. The controls underneath stay live, deliberately: a match can
        take minutes, the reconstruction renders its candidates through its own amplifier instances
        so nothing the user touches here can corrupt the result, and disabling a page someone is
        already working in is a worse surprise than letting them work in it. What they actually need
        to know is that applying a candidate afterwards will overwrite these settings -- which is a
        sentence, not a padlock.

        Implemented once here rather than three times, because the pages it applies to have nothing
        else in common and each would have grown its own slightly different scrim.
    */
    void setSongMatchVeil(bool veiled, juce::String reason)
    {
        if (veiled == veilShown && reason == veilReason) return;
        veilShown = veiled;
        veilReason = std::move(reason);
        repaint();
    }

    [[nodiscard]] bool songMatchVeilShown() const noexcept { return veilShown; }

    /** A standing note that Auto Match is holding the controls on this page.

        Unlike the veil this dims nothing and covers nothing: it is a state the user chose and
        may sit in for a whole session, so it has to be readable at a glance and invisible the
        rest of the time. Empty text removes it.

        Held controls are deliberately *not* recoloured -- in Auto Match almost every control on
        a page is held, so tinting them all would repaint the page in one colour and say nothing.
        What is worth marking is the exception, which is why `markAutoMatch` below colours the
        controls the user has taken back rather than the ones the analyzer still owns.
    */
    void setAutoMatchBadge(juce::String text)
    {
        if (text == autoMatchBadge) return;
        autoMatchBadge = std::move(text);
        repaint();
    }

    /** Colours one control by whether Auto Match still holds it.

        `released` is the only state that gets a colour of its own: it means the analyzer set
        this control and the user then took it back, so it is no longer part of the matched rig
        and will not be rewritten. Everything else -- held, or Auto Match off entirely -- keeps
        the normal accent, because that is the ordinary appearance of a control.
    */
    static void markAutoMatch(juce::Slider& slider, bool released)
    {
        const auto colour = released ? tf::theme::good : tf::theme::accent;
        slider.setColour(juce::Slider::rotarySliderFillColourId, colour);
        slider.setColour(juce::Slider::trackColourId, colour);
    }

    /// Drawn over the children so it dims the controls rather than sitting behind them.
    void paintOverChildren(juce::Graphics& graphics) override
    {
        /* The Auto Match note, drawn first so the song-match veil covers it if both are up --
           which they can be, because re-running a match while one is already held is normal.
           A pill in the top-right corner: it is the one band of every page that carries no
           control, so this never has to move anything to make room for itself. */
        if (autoMatchBadge.isNotEmpty())
        {
            const auto font = tf::theme::font(9.5f, true);
            const auto width = std::min(getWidth() - 24,
                                        juce::GlyphArrangement::getStringWidthInt(font, autoMatchBadge) + 26);
            const auto pill = juce::Rectangle<int>(getWidth() - width - 8, 2, width, 18);
            graphics.setColour(tf::theme::accentWash);
            graphics.fillRoundedRectangle(pill.toFloat(), 9.0f);
            graphics.setColour(tf::theme::accent.withAlpha(0.45f));
            graphics.drawRoundedRectangle(pill.toFloat().reduced(0.5f), 9.0f, 1.0f);
            graphics.setColour(tf::theme::accent);
            graphics.setFont(font);
            graphics.drawText(autoMatchBadge, pill, juce::Justification::centred, false);
        }

        if (! veilShown) return;
        const auto bounds = getLocalBounds();
        // Dimmed rather than opaque: the point is that the page is discouraged, not gone, and the
        // user should still be able to see where they were.
        graphics.setColour(juce::Colours::black.withAlpha(0.55f));
        graphics.fillRect(bounds);

        auto banner = bounds.withSizeKeepingCentre(std::min(bounds.getWidth() - 40, 520), 96);
        graphics.setColour(juce::Colours::black.withAlpha(0.72f));
        graphics.fillRoundedRectangle(banner.toFloat(), 8.0f);
        graphics.setColour(juce::Colours::white.withAlpha(0.28f));
        graphics.drawRoundedRectangle(banner.toFloat().reduced(0.5f), 8.0f, 1.0f);

        graphics.setColour(juce::Colours::white.withAlpha(0.92f));
        graphics.setFont(juce::Font { juce::FontOptions { 15.0f }.withStyle("Bold") });
        graphics.drawText("Song match in progress", banner.removeFromTop(40).reduced(16, 8),
                          juce::Justification::centredBottom, false);
        graphics.setColour(juce::Colours::white.withAlpha(0.72f));
        graphics.setFont(juce::Font { juce::FontOptions { 12.0f } });
        graphics.drawFittedText(veilReason, banner.reduced(16, 4), juce::Justification::centredTop, 3);
    }

protected:
    TubeForgeAudioProcessor& processor;

private:
    bool veilShown {};
    juce::String veilReason;
    juce::String autoMatchBadge;
};
