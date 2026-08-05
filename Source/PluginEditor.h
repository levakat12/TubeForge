#pragma once

#include "TubeForgeLookAndFeel.h"
#include "ui/ModulePage.h"
#include "ui/ShellWidgets.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <memory>
#include <vector>

class TubeForgeAudioProcessor;
class ProfileLibraryPage;

/// The editor shell.
///
/// It owns the chrome that surrounds every page -- the icon navigation across the top, the
/// global control rail beneath it, and the status bar -- and it owns the pages themselves,
/// but it knows nothing about what is inside any of them. Its whole relationship with a page
/// is: construct it, give it bounds, show or hide it, and call refresh() while it is showing.
///
/// The layout is three fixed-height horizontal bands with the page stage filling whatever is
/// left. Nothing floats and nothing overlaps, which is what keeps a window with nine modules
/// in it from reading as a pile of panels.
class TubeForgeAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& processor);
    ~TubeForgeAudioProcessorEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

    static constexpr int moduleCount = 12;
    /// Modules from this index on are engineering surfaces, hidden until PRO is switched on.
    static constexpr int firstProModule = 9;
    /// The page the header's preset controls talk to.
    static constexpr int libraryModule = 8;

private:
    void timerCallback() override;
    void buildPages();
    void buildNavigation();
    void layOutNavigation();
    void layOutRail();
    void setActiveModule(int index);
    void applyProModeVisibility();
    /** Greys the controls the performance tier has taken over.

        A capped control that still looks live is worse than one that is visibly unavailable: the
        user turns it, nothing happens, and there is nothing on screen saying why. This does not
        change any value -- the tier caps what the engine derives, never what is stored -- so
        raising the tier brings the control back exactly where it was left.
    */
    void applyPerformanceTierVisibility();
    void chooseProjectToSave();
    void chooseProjectToOpen();

    TubeForgeAudioProcessor& processor;
    TubeForgeLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 700 };

    juce::Label title;
    juce::Label productTagline;

    /// One icon per module, in module order. This is the only navigation in the window.
    std::vector<std::unique_ptr<tf::ui::IconButton>> navIcons;

    juce::Label presetCaption;
    juce::Label presetName;
    tf::ui::IconButton presetPrevious { tf::ui::Glyph::previous, "Previous rig" };
    tf::ui::IconButton presetNext { tf::ui::Glyph::next, "Next rig" };
    tf::ui::IconButton presetBrowse { tf::ui::Glyph::browse, "Browse rigs" };
    tf::ui::IconButton openProject { tf::ui::Glyph::open, "Open project" };
    tf::ui::IconButton saveProject { tf::ui::Glyph::save, "Save project" };
    tf::ui::IconButton audioSettings { tf::ui::Glyph::settings, "Audio settings" };

    juce::Label engineCaption;
    juce::ComboBox engineModeSelector;
    juce::Label performanceCaption;
    juce::ComboBox performanceSelector;
    juce::ToggleButton bypass { "BYPASS" };
    juce::ToggleButton proMode { "PRO" };

    juce::Label inputLabel;
    juce::Label outputLabel;
    juce::Slider inputGain;
    juce::Slider outputGain;
    tf::ui::LevelBar inputMeter;
    tf::ui::LevelBar outputMeter;

    juce::Component pageHost;
    /// Owned pages, in module order. The shell only ever touches them through ModulePage.
    std::array<std::unique_ptr<ModulePage>, moduleCount> pages;
    /// Borrowed view of pages[libraryModule], for the header's preset controls.
    ProfileLibraryPage* library {};
    int activeModule {};
    /// The active module's name and tag, drawn above the page.
    juce::String moduleCaption;

    juce::Label mode;
    juce::Label deviceStatus;
    tf::ui::StudioSignalChain signalChain;
    juce::Label diagnosticsText;

    /// Bands the shell paints itself rather than owning as components, cached by resized() so
    /// paint() never has to re-derive the layout and drift out of step with it.
    juce::Rectangle<int> navBounds;
    juce::Rectangle<int> railBounds;
    juce::Rectangle<int> stageBounds;
    juce::Rectangle<int> captionBounds;
    juce::Rectangle<int> footerBounds;
    /// Hairlines between the navigation groups and between the rail's control clusters.
    std::array<int, 2> navDividerX {};
    std::array<int, 2> railDividerX {};

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> engineModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> performanceAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TubeForgeAudioProcessorEditor)
};
