#include "NeuralCapturePage.h"
#include "../PluginProcessor.h"
#include "../TubeForgeTheme.h"

namespace theme = tf::theme;

NeuralCapturePage::NeuralCapturePage(TubeForgeAudioProcessor& processorToUse)
    : ModulePage(processorToUse)
{
    tf::ui::configureFieldCaption(monitorCaption, "Monitor");
    tf::ui::populateFromParameter(monitorSelector, processor.getParameters(), "neuralMonitor");
    tf::ui::configureLabel(status, "No neural model loaded", 12.0f, false, theme::textSecondary);
    tf::ui::configureLabel(calibration, "Input calibration waiting for audio", 12.0f, false,
                           theme::textSecondary);
    tf::ui::configureLabel(captureWizardHelp,
                           "Captures are made outside the plug-in: run ml/scripts/capture_wizard.py, "
                           "then load the folder it exports with the button above.",
                           11.0f, false, theme::textTertiary);
    captureWizardHelp.setJustificationType(juce::Justification::topLeft);

    addAndMakeVisible(modelPanel);
    addAndMakeVisible(calibrationPanel);
    addAndMakeVisible(captureWizardHelp);
    for (auto* component : std::initializer_list<juce::Component*> { &loadModel, &monitorCaption,
             &monitorSelector, &compensation, &status })
        modelPanel.addAndMakeVisible(*component);
    calibrationPanel.addAndMakeVisible(calibration);

    monitorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "neuralMonitor", monitorSelector);
    compensationAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "neuralCompensation", compensation);

    loadModel.onClick = [this] { chooseModel(); };
}

void NeuralCapturePage::resized()
{
    auto area = getLocalBounds();
    modelPanel.setBounds(area.removeFromTop(150));
    area.removeFromTop(10);
    calibrationPanel.setBounds(area.removeFromTop(90));
    area.removeFromTop(12);
    captureWizardHelp.setBounds(area.removeFromTop(48).withTrimmedLeft(13));

    auto content = modelPanel.contentArea();
    auto top = content.removeFromTop(41);
    loadModel.setBounds(top.removeFromLeft(150).withTrimmedTop(13).withHeight(28));
    top.removeFromLeft(14);
    tf::ui::layOutField(top.removeFromLeft(200), monitorCaption, monitorSelector);
    content.removeFromTop(10);
    compensation.setBounds(content.removeFromTop(30).withWidth(std::min(content.getWidth(), 340)));
    content.removeFromTop(6);
    status.setBounds(content);
    calibration.setBounds(calibrationPanel.contentArea());
}

void NeuralCapturePage::refresh()
{
    status.setText(processor.neuralModelStatusText(), juce::dontSendNotification);
    const auto reading = processor.neuralCalibrationReading();
    calibration.setText(
        "Input RMS " + juce::String(reading.rmsDb, 1) + " dBFS      Peak "
        + juce::String(reading.peakDb, 1) + " dBFS      Difference "
        + juce::String(reading.mismatchDb, 1) + " dB\n"
        + (reading.warning ? "Level mismatch: the model expects a different input level."
                           : "Calibrated."), juce::dontSendNotification);
    calibration.setColour(juce::Label::textColourId, reading.warning ? theme::warn : theme::textSecondary);
}

void NeuralCapturePage::chooseModel()
{
    fileChooser = std::make_unique<juce::FileChooser>("Choose exported neural model artifact");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = juce::Component::SafePointer<NeuralCapturePage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestNeuralModelLoad(chooser.getResult());
        });
}
