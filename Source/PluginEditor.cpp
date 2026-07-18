#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <algorithm>

namespace
{
juce::String statusName(nts::diagnostics::AssetLoadStatus status)
{
    switch (status)
    {
        case nts::diagnostics::AssetLoadStatus::unavailable: return "N/A";
        case nts::diagnostics::AssetLoadStatus::idle: return "Idle";
        case nts::diagnostics::AssetLoadStatus::loading: return "Loading";
        case nts::diagnostics::AssetLoadStatus::ready: return "Ready";
        case nts::diagnostics::AssetLoadStatus::failed: return "Failed";
    }
    return "Unknown";
}
} // namespace

TubeForgeAudioProcessorEditor::TubeForgeAudioProcessorEditor(TubeForgeAudioProcessor& owner)
    : juce::AudioProcessorEditor(owner), processor(owner)
{
    configureHeading(title, "TubeForge - Core Infrastructure");
    title.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    configureHeading(mode, "Mode: " + processor.modeName());
    configureHeading(deviceStatus, processor.deviceStatusText());
    configureHeading(diagnosticsText, "Diagnostics waiting for audio...");
    configureHeading(inputLabel, "Input gain");
    configureHeading(outputLabel, "Output gain");

    configureSlider(inputGain);
    configureSlider(outputGain);
    inputGain.setTextValueSuffix(" dB");
    outputGain.setTextValueSuffix(" dB");

    for (auto* component : std::initializer_list<juce::Component*> {
             &title, &mode, &deviceStatus, &diagnosticsText, &inputLabel, &outputLabel,
             &inputGain, &outputGain, &bypass, &audioSettings, &openProject, &saveProject,
             &inputMeter, &outputMeter })
        addAndMakeVisible(*component);

    inputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "input", inputGain);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getParameters(), "output", outputGain);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getParameters(), "bypass", bypass);

    audioSettings.onClick = [this]
    {
        const auto message = processor.modeName() == "Standalone"
            ? "Open the Audio settings tab to select ASIO/WASAPI devices, channels, sample rate, and buffer size."
            : "Audio devices, sample rate, channels, and buffer size are controlled by the plugin host.";
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Audio settings", message);
    };
    saveProject.onClick = [this] { chooseProjectToSave(); };
    openProject.onClick = [this] { chooseProjectToOpen(); };

    setResizable(true, true);
    setResizeLimits(620, 440, 1100, 800);
    setSize(760, 520);
    startTimerHz(20);
}

void TubeForgeAudioProcessorEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour::fromRGB(20, 23, 28));
    graphics.setColour(juce::Colour::fromRGB(47, 53, 64));
    graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(12.0f), 8.0f, 1.0f);
}

void TubeForgeAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(24);
    title.setBounds(area.removeFromTop(38));
    mode.setBounds(area.removeFromTop(26));
    deviceStatus.setBounds(area.removeFromTop(26));
    area.removeFromTop(14);

    auto controls = area.removeFromTop(150);
    auto inputArea = controls.removeFromLeft(controls.getWidth() / 2).reduced(8);
    auto outputArea = controls.reduced(8);
    inputLabel.setBounds(inputArea.removeFromTop(24));
    inputGain.setBounds(inputArea.removeFromTop(80));
    inputMeter.setBounds(inputArea.removeFromTop(22));
    outputLabel.setBounds(outputArea.removeFromTop(24));
    outputGain.setBounds(outputArea.removeFromTop(80));
    outputMeter.setBounds(outputArea.removeFromTop(22));

    area.removeFromTop(10);
    auto buttons = area.removeFromTop(34);
    bypass.setBounds(buttons.removeFromLeft(100));
    audioSettings.setBounds(buttons.removeFromLeft(140).reduced(4, 0));
    openProject.setBounds(buttons.removeFromLeft(130).reduced(4, 0));
    saveProject.setBounds(buttons.removeFromLeft(130).reduced(4, 0));
    area.removeFromTop(16);
    diagnosticsText.setBounds(area.removeFromTop(82));
}

void TubeForgeAudioProcessorEditor::timerCallback()
{
    processor.refreshNonRealtimeDiagnostics();
    const auto& meters = processor.meterState();
    inputMeterValue = std::clamp(static_cast<double>(std::max(meters.inputPeak(0), meters.inputPeak(1))), 0.0, 1.0);
    outputMeterValue = std::clamp(static_cast<double>(std::max(meters.outputPeak(0), meters.outputPeak(1))), 0.0, 1.0);

    const auto diagnostics = processor.diagnosticsSnapshot();
    diagnosticsText.setText(
        "CPU: " + juce::String(diagnostics.cpuLoadPercent, 2) + "%    "
        "Callback: " + juce::String(diagnostics.callbackMilliseconds, 3) + " ms    "
        "Max: " + juce::String(diagnostics.maximumCallbackMilliseconds, 3) + " ms\n"
        "Deadline: " + juce::String(diagnostics.deadlineMilliseconds, 3) + " ms    "
        "Dropouts: " + juce::String(diagnostics.dropoutCount) + "    "
        "Latency: " + juce::String(diagnostics.currentGraphLatencySamples) + " samples    "
        "Memory: " + juce::String(static_cast<double>(diagnostics.workingSetBytes) / (1024.0 * 1024.0), 1) + " MiB\n"
        "Model: " + statusName(diagnostics.modelLoadStatus) + "    "
        "IR: " + statusName(diagnostics.irLoadStatus) + "    "
        "Background failures: " + juce::String(diagnostics.backgroundJobFailureCount),
        juce::dontSendNotification);
}

void TubeForgeAudioProcessorEditor::chooseProjectToSave()
{
    fileChooser = std::make_unique<juce::FileChooser>("Save TubeForge project", juce::File {}, "*.tforge");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
                             {
                                 if (safeThis == nullptr || chooser.getResult() == juce::File {})
                                     return;
                                 const auto result = safeThis->processor.saveProject(chooser.getResult().withFileExtension("tforge"));
                                 if (result.failed())
                                     juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                            "Save failed", result.getErrorMessage());
                             });
}

void TubeForgeAudioProcessorEditor::chooseProjectToOpen()
{
    fileChooser = std::make_unique<juce::FileChooser>("Open TubeForge project", juce::File {}, "*.tforge");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safeThis = juce::Component::SafePointer<TubeForgeAudioProcessorEditor>(this)](const juce::FileChooser& chooser)
                             {
                                 if (safeThis == nullptr || chooser.getResult() == juce::File {})
                                     return;
                                 const auto result = safeThis->processor.loadProject(chooser.getResult());
                                 if (result.failed())
                                     juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                            "Open failed", result.getErrorMessage());
                             });
}

void TubeForgeAudioProcessorEditor::configureSlider(juce::Slider& slider)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 24);
}

void TubeForgeAudioProcessorEditor::configureHeading(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
    label.setJustificationType(juce::Justification::centredLeft);
}
