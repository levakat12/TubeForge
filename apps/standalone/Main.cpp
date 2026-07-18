#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

namespace
{
constexpr auto deviceStateKey = "audioDeviceState";
constexpr auto processorStateKey = "processorState";
constexpr auto windowXKey = "windowX";
constexpr auto windowYKey = "windowY";
constexpr auto windowWidthKey = "windowWidth";
constexpr auto windowHeightKey = "windowHeight";

class StandaloneContent final : public juce::Component,
                                private juce::ChangeListener
{
public:
    explicit StandaloneContent(juce::PropertiesFile& settingsFile)
        : settings(settingsFile), tabs(juce::TabbedButtonBar::TabsAtTop)
    {
        processor = std::make_unique<TubeForgeAudioProcessor>();
        processor->setStandaloneApplicationMode(true);

        std::unique_ptr<juce::XmlElement> savedDeviceState;
        if (const auto saved = settings.getValue(deviceStateKey); saved.isNotEmpty())
            savedDeviceState = juce::parseXML(saved);

        const auto deviceError = deviceManager.initialise(2, 2, savedDeviceState.get(), true);
        if (deviceError.isNotEmpty())
            statusMessage = "Audio fallback: " + deviceError;

        if (const auto encodedState = settings.getValue(processorStateKey); encodedState.isNotEmpty())
        {
            juce::MemoryBlock state;
            if (state.fromBase64Encoding(encodedState))
                processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }

        player.setProcessor(processor.get());
        deviceManager.addAudioCallback(&player);
        deviceManager.addChangeListener(this);

        editor.reset(processor->createEditor());
        deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent>(
            deviceManager, 0, 2, 0, 2, false, false, true, false);
        deviceSelector->setItemHeight(26);

        tabs.addTab("Tone", juce::Colour::fromRGB(28, 32, 39), editor.get(), false);
        tabs.addTab("Audio settings", juce::Colour::fromRGB(28, 32, 39), deviceSelector.get(), false);
        addAndMakeVisible(tabs);

        if (statusMessage.isNotEmpty())
        {
            status.setText(statusMessage, juce::dontSendNotification);
            status.setColour(juce::Label::textColourId, juce::Colours::orange);
            status.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(status);
        }

        setSize(980, 680);
    }

    ~StandaloneContent() override
    {
        saveState();
        deviceManager.removeChangeListener(this);
        deviceManager.removeAudioCallback(&player);
        player.setProcessor(nullptr);
        tabs.clearTabs();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        if (status.isVisible())
            status.setBounds(area.removeFromBottom(28).reduced(8, 2));
        tabs.setBounds(area);
    }

    void saveState()
    {
        if (const auto deviceState = deviceManager.createStateXml())
            settings.setValue(deviceStateKey, deviceState->toString());

        juce::MemoryBlock processorState;
        processor->getStateInformation(processorState);
        settings.setValue(processorStateKey, processorState.toBase64Encoding());
        settings.saveIfNeeded();
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        saveState();
    }

    juce::PropertiesFile& settings;
    juce::AudioDeviceManager deviceManager;
    juce::AudioProcessorPlayer player;
    std::unique_ptr<TubeForgeAudioProcessor> processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    juce::TabbedComponent tabs;
    juce::Label status;
    juce::String statusMessage;
};

class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow(juce::PropertiesFile& settings)
        : DocumentWindow("TubeForge", juce::Colour::fromRGB(20, 23, 28),
                         juce::DocumentWindow::allButtons), properties(settings)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setContentOwned(new StandaloneContent(settings), true);

        const auto width = properties.getIntValue(windowWidthKey, 980);
        const auto height = properties.getIntValue(windowHeightKey, 680);
        const auto x = properties.getIntValue(windowXKey, -1);
        const auto y = properties.getIntValue(windowYKey, -1);
        setSize(width, height);
        if (x >= 0 && y >= 0)
            setTopLeftPosition(x, y);
        else
            centreWithSize(width, height);
        setVisible(true);
    }

    ~MainWindow() override
    {
        properties.setValue(windowXKey, getX());
        properties.setValue(windowYKey, getY());
        properties.setValue(windowWidthKey, getWidth());
        properties.setValue(windowHeightKey, getHeight());
        properties.saveIfNeeded();
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    juce::PropertiesFile& properties;
};

class TubeForgeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "TubeForge"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "TubeForge";
        options.filenameSuffix = "settings";
        options.folderName = "TubeForge";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters(options);
        window = std::make_unique<MainWindow>(*properties.getUserSettings());
    }

    void shutdown() override
    {
        window.reset();
        properties.closeFiles();
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}

private:
    juce::ApplicationProperties properties;
    std::unique_ptr<MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION(TubeForgeApplication)
