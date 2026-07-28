#include "PluginProcessor.h"
#include "AudioInputRouting.h"

#include <nts/ecosystem/ReleaseServices.h>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace
{
constexpr auto deviceStateKey = "audioDeviceState";
constexpr auto processorStateKey = "processorState";
constexpr auto windowXKey = "windowX";
constexpr auto windowYKey = "windowY";
constexpr auto windowWidthKey = "windowWidth";
constexpr auto windowHeightKey = "windowHeight";
constexpr auto minimumLatencyDspConfiguredKey = "minimumLatencyDspConfigured";
constexpr auto minimumLatencyBackendConfiguredKey = "minimumLatencyBackendConfigured";
constexpr auto audioBackendVersionKey = "audioBackendVersion";
constexpr auto inputRoutingKey = "inputRouting";
constexpr int currentAudioBackendVersion = 1;

juce::String requestAsioBackend(juce::AudioDeviceManager& manager)
{
#if JUCE_ASIO
    for (auto* type : manager.getAvailableDeviceTypes())
    {
        if (type == nullptr || ! type->getTypeName().containsIgnoreCase("ASIO"))
            continue;

        type->scanForDevices();
        auto outputNames = type->getDeviceNames(false);
        const auto inputNames = type->getDeviceNames(true);
        if (outputNames.isEmpty()) outputNames = inputNames;
        if (outputNames.isEmpty())
            return "ASIO is enabled, but no ASIO device is installed";

        manager.setCurrentAudioDeviceType(type->getTypeName(), true);
        auto preferredDevice = outputNames[0];
        for (const auto& name : outputNames)
            if (name.containsIgnoreCase("Focusrite USB")) { preferredDevice = name; break; }
        if (! preferredDevice.containsIgnoreCase("Focusrite USB"))
            for (const auto& name : outputNames)
                if (name.containsIgnoreCase("Focusrite")) { preferredDevice = name; break; }

        auto setup = manager.getAudioDeviceSetup();
        setup.outputDeviceName = preferredDevice;
        setup.inputDeviceName = inputNames.contains(preferredDevice)
            ? preferredDevice : (inputNames.isEmpty() ? juce::String {} : inputNames[0]);
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;
        if (const auto error = manager.setAudioDeviceSetup(setup, true); error.isNotEmpty())
            return preferredDevice + " could not be opened: " + error;
        if (manager.getCurrentAudioDevice() != nullptr
            && manager.getCurrentAudioDeviceType() == type->getTypeName())
            return "ASIO backend: " + manager.getCurrentAudioDevice()->getName();

        return "The ASIO device could not be opened";
    }
    return "No ASIO backend is registered";
#else
    juce::ignoreUnused(manager);
    return {};
#endif
}

class AudioSettingsPanel final : public juce::Component,
                                 private juce::ChangeListener
{
public:
    AudioSettingsPanel(juce::AudioDeviceManager& deviceManager, juce::PropertiesFile& settingsFile)
        : manager(deviceManager), settings(settingsFile),
          selector(deviceManager, 1, 2, 2, 2, false, false, false, false)
    {
        routingLabel.setText("Guitar input", juce::dontSendNotification);
        routingLabel.setJustificationType(juce::Justification::centredRight);
        routingHelp.setJustificationType(juce::Justification::centredLeft);
        routingHelp.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        routing.onChange = [this]
        {
            if (! updating) applyRouting();
        };
        selector.setItemHeight(26);
        for (auto* component : { static_cast<juce::Component*>(&routingLabel),
                                 static_cast<juce::Component*>(&routing),
                                 static_cast<juce::Component*>(&routingHelp),
                                 static_cast<juce::Component*>(&selector) })
            addAndMakeVisible(component);
        manager.addChangeListener(this);
        rebuildRoutingChoices(true);
    }

    ~AudioSettingsPanel() override
    {
        manager.removeChangeListener(this);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto routingRow = area.removeFromTop(44).reduced(8, 4);
        routingLabel.setBounds(routingRow.removeFromLeft(110));
        routing.setBounds(routingRow.removeFromLeft(250).reduced(4, 0));
        routingHelp.setBounds(routingRow);
        selector.setBounds(area);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        rebuildRoutingChoices(false);
    }

    void rebuildRoutingChoices(bool applySavedChoice)
    {
        if (updating) return;
        const juce::ScopedValueSetter guard(updating, true);
        const auto previousId = applySavedChoice
            ? settings.getIntValue(inputRoutingKey, 1) : routing.getSelectedId();
        routing.clear(juce::dontSendNotification);
        auto* device = manager.getCurrentAudioDevice();
        if (device == nullptr)
        {
            routing.setEnabled(false);
            routingHelp.setText("Choose an audio device below", juce::dontSendNotification);
            return;
        }

        const auto inputNames = device->getInputChannelNames();
        for (int channel = 0; channel < inputNames.size(); ++channel)
            routing.addItem("Mono: " + inputNames[channel], channel + 1);
        if (inputNames.size() >= 2)
            routing.addItem("Stereo: inputs 1 + 2", nts::standalone::stereoInputRoutingId);
        routing.setEnabled(! inputNames.isEmpty());
        auto selectedId = previousId;
        if (routing.indexOfItemId(selectedId) < 0) selectedId = inputNames.isEmpty() ? 0 : 1;
        routing.setSelectedId(selectedId, juce::dontSendNotification);
        if (applySavedChoice && selectedId != 0)
        {
            settings.setValue(inputRoutingKey, selectedId);
            applyRoutingSelection(selectedId);
        }
        else
        {
            updateRoutingHelp(selectedId, {});
        }
    }

    void applyRouting()
    {
        const juce::ScopedValueSetter guard(updating, true);
        const auto selectedId = routing.getSelectedId();
        if (selectedId == 0) return;
        settings.setValue(inputRoutingKey, selectedId);
        applyRoutingSelection(selectedId);
    }

    void applyRoutingSelection(int selectedId)
    {
        auto* device = manager.getCurrentAudioDevice();
        if (device == nullptr) return;
        const auto inputCount = device->getInputChannelNames().size();
        if (inputCount == 0) return;

        auto setup = manager.getAudioDeviceSetup();
        setup.inputChannels.clear();
        setup.useDefaultInputChannels = false;
        setup.inputChannels = nts::standalone::inputChannelsForRouting(selectedId, inputCount);

        const auto error = manager.setAudioDeviceSetup(setup, true);
        updateRoutingHelp(selectedId, error);
    }

    void updateRoutingHelp(int selectedId, const juce::String& error)
    {
        if (error.isNotEmpty())
        {
            routingHelp.setColour(juce::Label::textColourId, juce::Colours::orange);
            routingHelp.setText("Input routing failed: " + error, juce::dontSendNotification);
            return;
        }
        routingHelp.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        routingHelp.setText(selectedId == nts::standalone::stereoInputRoutingId
            ? "Stereo inputs remain separate"
            : "Selected input is duplicated to left and right headphones",
            juce::dontSendNotification);
    }

    juce::AudioDeviceManager& manager;
    juce::PropertiesFile& settings;
    juce::Label routingLabel;
    juce::ComboBox routing;
    juce::Label routingHelp;
    juce::AudioDeviceSelectorComponent selector;
    bool updating {};
};

juce::String requestMinimumBufferSize(juce::AudioDeviceManager& manager)
{
    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
        return "No audio device is active";

    const auto availableSizes = device->getAvailableBufferSizes();
    const auto minimumCallbackSamples = std::max(1, static_cast<int>(
        std::ceil(device->getCurrentSampleRate() * 0.00065)));
    int minimumSize = 0;
    for (const auto size : availableSizes)
        if (size >= minimumCallbackSamples && (minimumSize == 0 || size < minimumSize))
            minimumSize = size;

    if (minimumSize == 0)
        for (const auto size : availableSizes)
            if (size > minimumSize)
                minimumSize = size;

    if (minimumSize == 0)
        return "The audio driver did not advertise a buffer size";

    const auto originalSetup = manager.getAudioDeviceSetup();
    if (originalSetup.bufferSize != minimumSize)
    {
        auto lowLatencySetup = originalSetup;
        lowLatencySetup.bufferSize = minimumSize;
        if (const auto error = manager.setAudioDeviceSetup(lowLatencySetup, true); error.isNotEmpty())
        {
            static_cast<void>(manager.setAudioDeviceSetup(originalSetup, true));
            return "Minimum buffer unavailable: " + error;
        }
    }

    return "Minimum stable buffer: " + juce::String(minimumSize) + " samples";
}

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
        else
        {
            if (settings.getIntValue(audioBackendVersionKey, 0) < currentAudioBackendVersion)
            {
                const auto asioStatus = requestAsioBackend(deviceManager);
                if (asioStatus.isNotEmpty())
                    statusMessage = asioStatus + "; ";
                settings.setValue(minimumLatencyBackendConfiguredKey, true);
                settings.setValue(audioBackendVersionKey, currentAudioBackendVersion);
            }
            statusMessage += requestMinimumBufferSize(deviceManager);
        }

        if (const auto encodedState = settings.getValue(processorStateKey); encodedState.isNotEmpty())
        {
            juce::MemoryBlock state;
            if (state.fromBase64Encoding(encodedState))
                processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
        if (! settings.getBoolValue(minimumLatencyDspConfiguredKey, false))
        {
            if (auto* oversampling = processor->getParameters().getParameter("oversampling"))
                oversampling->setValueNotifyingHost(oversampling->convertTo0to1(0.0f));
            settings.setValue(minimumLatencyDspConfiguredKey, true);
        }

        player.setProcessor(processor.get());
        deviceManager.addAudioCallback(&player);
        deviceManager.addChangeListener(this);

        editor.reset(processor->createEditor());
        audioSettings = std::make_unique<AudioSettingsPanel>(deviceManager, settings);

        tabs.addTab("Tone", juce::Colour::fromRGB(28, 32, 39), editor.get(), false);
        tabs.addTab("Audio settings", juce::Colour::fromRGB(28, 32, 39), audioSettings.get(), false);
        addAndMakeVisible(tabs);

        if (statusMessage.isNotEmpty())
        {
            status.setText(statusMessage, juce::dontSendNotification);
            status.setColour(juce::Label::textColourId,
                             statusMessage.containsIgnoreCase("Minimum stable buffer")
                                 ? juce::Colours::lightgreen
                                 : juce::Colours::orange);
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
    std::unique_ptr<AudioSettingsPanel> audioSettings;
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
    const juce::String getApplicationVersion() override { return "0.10.0"; }
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
    void unhandledException(const std::exception* exception,
                            const juce::String& sourceFilename,
                            int lineNumber) override
    {
        nts::ecosystem::CrashContext context;
        context.reportId = juce::Uuid().toString().toStdString();
        context.timestampUtc = juce::Time::getCurrentTime().toISO8601(true).toStdString();
        context.mode = nts::ecosystem::RuntimeMode::standalone;
        context.operatingSystem = juce::SystemStats::getOperatingSystemName().toStdString();
        context.exceptionCode = exception != nullptr ? exception->what() : "unknown exception";
        context.stackTrace = juce::SystemStats::getStackBacktrace().toStdString();
        context.recentEvents.push_back("unhandled exception at " + sourceFilename.toStdString()
                                       + ":" + std::to_string(lineNumber));
        const auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("TubeForge").getChildFile("crashes");
        if (directory.createDirectory())
            (void) directory.getChildFile(juce::String(context.reportId) + ".json")
                .replaceWithText(nts::ecosystem::crashReportJson(context, true));
        juce::JUCEApplication::unhandledException(exception, sourceFilename, lineNumber);
    }

private:
    juce::ApplicationProperties properties;
    std::unique_ptr<MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION(TubeForgeApplication)
