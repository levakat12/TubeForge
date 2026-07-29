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
/// It owns the chrome that surrounds every page -- header, gear browser, I/O rail, signal
/// chain, diagnostics -- and it owns the pages themselves, but it knows nothing about what is
/// inside any of them. Its whole relationship with a page is: construct it, give it bounds,
/// show or hide it, and call refresh() while it is showing.
class TubeForgeAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& processor);
    ~TubeForgeAudioProcessorEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

    static constexpr int moduleCount = 8;
    /// Modules from this index on are engineering surfaces, hidden until PRO is switched on.
    static constexpr int firstProModule = 5;
    /// The page the header's preset controls talk to.
    static constexpr int libraryModule = 4;

private:
    void timerCallback() override;
    void buildPages();
    void buildGearBrowser();
    void setActiveModule(int index);
    void applyProModeVisibility();
    void chooseProjectToSave();
    void chooseProjectToOpen();

    TubeForgeAudioProcessor& processor;
    TubeForgeLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 700 };

    juce::Label title;
    juce::Label productTagline;
    juce::Label presetCaption;
    juce::Label presetName;
    juce::TextButton presetPrevious { "<" };
    juce::TextButton presetNext { ">" };
    juce::TextButton presetBrowse { "Browse" };
    juce::ComboBox engineModeSelector;
    juce::ToggleButton bypass { "BYPASS" };
    juce::ToggleButton proMode { "PRO" };
    juce::TextButton audioSettings { "Audio" };
    juce::TextButton openProject { "Open" };
    juce::TextButton saveProject { "Save" };

    juce::Component browserPanel;
    juce::Label browserTitle;
    juce::Label mode;
    juce::Label deviceStatus;
    std::array<juce::Label, 3> browserGroupLabels;
    std::vector<std::unique_ptr<tf::ui::GearBrowserItem>> gearItems;

    juce::Component pageHost;
    juce::Label moduleTitle;
    juce::Label moduleSubtitle;
    /// Owned pages, in module order. The shell only ever touches them through ModulePage.
    std::array<std::unique_ptr<ModulePage>, moduleCount> pages;
    /// Borrowed view of pages[libraryModule], for the header's preset controls.
    ProfileLibraryPage* library {};
    int activeModule {};

    /// Panels the shell paints itself rather than owning as components, cached by resized()
    /// so paint() never has to re-derive the layout and drift out of step with it.
    juce::Rectangle<int> headerBounds;
    juce::Rectangle<int> meterRailBounds;

    juce::Label inputLabel;
    juce::Label outputLabel;
    juce::Slider inputGain;
    juce::Slider outputGain;
    tf::ui::StudioLevelMeter studioMeter;
    tf::ui::StudioSignalChain signalChain;
    juce::Label diagnosticsText;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> engineModeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TubeForgeAudioProcessorEditor)
};
