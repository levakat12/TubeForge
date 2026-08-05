#include "NeuralCapturePage.h"
#include "CapturePicker.h"
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
                           "Load a capture picks from the Neural Amp Modeler captures you have "
                           "imported, and can import more -- a .zip straight from a capture pack "
                           "is read here, in the plug-in. Load a trained model takes an artifact "
                           "folder exported by ml/scripts/capture_wizard.py instead.",
                           11.0f, false, theme::textTertiary);
    captureWizardHelp.setJustificationType(juce::Justification::topLeft);
    loadModel.setTooltip("Choose from your imported .nam captures, or import an archive.");
    loadArtifact.setTooltip("Load a model folder this plug-in's own capture wizard exported.");

    addAndMakeVisible(modelPanel);
    addAndMakeVisible(calibrationPanel);
    addAndMakeVisible(captureWizardHelp);
    for (auto* component : std::initializer_list<juce::Component*> { &loadModel, &loadArtifact,
             &monitorCaption, &monitorSelector, &compensation, &status })
        modelPanel.addAndMakeVisible(*component);
    calibrationPanel.addAndMakeVisible(calibration);

    monitorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.getParameters(), "neuralMonitor", monitorSelector);
    compensationAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "neuralCompensation", compensation);

    loadModel.onClick = [this] { chooseModel(); };
    loadArtifact.onClick = [this] { chooseArtifactDirectory(); };
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
    top.removeFromLeft(10);
    loadArtifact.setBounds(top.removeFromLeft(170).withTrimmedTop(13).withHeight(28));
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
    CapturePicker::openOver(*this, processor, nts::nam::GearKind::amp,
                            "AMP ENGINE   /   CHOOSE A CAPTURE",
                            [safeThis = juce::Component::SafePointer<NeuralCapturePage>(this)]
                            (juce::File artifact)
                            {
                                if (safeThis == nullptr) return;
                                safeThis->processor.requestNeuralModelLoad(artifact);
                            });
}

void NeuralCapturePage::chooseArtifactDirectory()
{
    // The path for a model this plug-in trained itself, which is a directory rather than a
    // capture and never appears in the capture library.
    fileChooser = std::make_unique<juce::FileChooser>("Choose exported neural model artifact");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = juce::Component::SafePointer<NeuralCapturePage>(this)](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || chooser.getResult() == juce::File {}) return;
            safeThis->processor.requestNeuralModelLoad(chooser.getResult());
        });
}
