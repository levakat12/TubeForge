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

protected:
    TubeForgeAudioProcessor& processor;
};
