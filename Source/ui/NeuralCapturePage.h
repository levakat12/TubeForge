#pragma once

#include "ModulePage.h"
#include "UiSupport.h"

#include <memory>

/// Loading and monitoring a captured amp model.
class NeuralCapturePage final : public ModulePage
{
public:
    explicit NeuralCapturePage(TubeForgeAudioProcessor& processor);

    void resized() override;
    void refresh() override;

private:
    void chooseModel();

    tf::ui::SectionPanel modelPanel { "Model" };
    tf::ui::SectionPanel calibrationPanel { "Input calibration" };
    juce::TextButton loadModel { "Load a model" };
    juce::Label monitorCaption;
    juce::ComboBox monitorSelector;
    juce::ToggleButton compensation { "Match the model's expected input level" };
    juce::Label status;
    juce::Label calibration;
    juce::Label captureWizardHelp;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> monitorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> compensationAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NeuralCapturePage)
};
