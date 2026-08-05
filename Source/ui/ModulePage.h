#pragma once

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

    /// Drawn over the children so it dims the controls rather than sitting behind them.
    void paintOverChildren(juce::Graphics& graphics) override
    {
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
};
