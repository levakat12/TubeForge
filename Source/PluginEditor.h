#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

class TubeForgeAudioProcessor;

class TubeForgeAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& processor);
    ~TubeForgeAudioProcessorEditor() override = default;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void timerCallback() override;
    void chooseProjectToSave();
    void chooseProjectToOpen();
    static void configureSlider(juce::Slider& slider);
    static void configureHeading(juce::Label& label, const juce::String& text);

    TubeForgeAudioProcessor& processor;
    juce::Label title;
    juce::Label mode;
    juce::Label deviceStatus;
    juce::Label diagnosticsText;
    juce::Label inputLabel;
    juce::Label outputLabel;
    juce::Slider inputGain;
    juce::Slider outputGain;
    juce::ToggleButton bypass { "Bypass" };
    juce::TextButton audioSettings { "Audio settings" };
    juce::TextButton openProject { "Open project" };
    juce::TextButton saveProject { "Save project" };
    double inputMeterValue {};
    double outputMeterValue {};
    juce::ProgressBar inputMeter { inputMeterValue };
    juce::ProgressBar outputMeter { outputMeterValue };
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TubeForgeAudioProcessorEditor)
};
