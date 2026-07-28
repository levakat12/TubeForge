#include "TestHarness.h"
#include "AudioInputRouting.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <memory>

namespace
{
constexpr auto mockTypeName = "TubeForgeMock";
constexpr auto mockInputName = "Mock Input";
constexpr auto mockOutputName = "Mock Output";

class MockAudioDevice final : public juce::AudioIODevice
{
public:
    MockAudioDevice(juce::String outputName, juce::String inputName)
        : AudioIODevice(outputName.isNotEmpty() ? outputName : inputName, mockTypeName)
    {
    }

    juce::StringArray getOutputChannelNames() override { return { "Output 1", "Output 2" }; }
    juce::StringArray getInputChannelNames() override { return { "Input 1", "Input 2" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0, 96000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 32, 64, 128, 256, 512, 2048 }; }
    int getDefaultBufferSize() override { return 256; }

    juce::String open(const juce::BigInteger& requestedInputs,
                      const juce::BigInteger& requestedOutputs,
                      double requestedSampleRate,
                      int requestedBufferSize) override
    {
        if (! getAvailableSampleRates().contains(requestedSampleRate))
            return "Unsupported sample rate";
        if (! getAvailableBufferSizes().contains(requestedBufferSize))
            return "Unsupported buffer size";

        inputs = requestedInputs;
        outputs = requestedOutputs;
        sampleRate = requestedSampleRate;
        bufferSize = requestedBufferSize;
        opened = true;
        return {};
    }

    void close() override
    {
        stop();
        opened = false;
    }

    bool isOpen() override { return opened; }

    void start(juce::AudioIODeviceCallback* newCallback) override
    {
        callback = newCallback;
        if (callback != nullptr)
            callback->audioDeviceAboutToStart(this);
        playing = opened;
    }

    void stop() override
    {
        if (playing && callback != nullptr)
            callback->audioDeviceStopped();
        playing = false;
        callback = nullptr;
    }

    bool isPlaying() override { return playing; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSize; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outputs; }
    juce::BigInteger getActiveInputChannels() const override { return inputs; }
    int getOutputLatencyInSamples() override { return bufferSize; }
    int getInputLatencyInSamples() override { return bufferSize; }

private:
    juce::AudioIODeviceCallback* callback {};
    juce::BigInteger inputs;
    juce::BigInteger outputs;
    double sampleRate { 48000.0 };
    int bufferSize { 256 };
    bool opened {};
    bool playing {};
};

class MockAudioDeviceType final : public juce::AudioIODeviceType
{
public:
    MockAudioDeviceType() : AudioIODeviceType(mockTypeName) {}

    void scanForDevices() override {}

    juce::StringArray getDeviceNames(bool wantInputNames) const override
    {
        if (! available)
            return {};
        return { wantInputNames ? mockInputName : mockOutputName };
    }

    int getDefaultDeviceIndex(bool) const override { return 0; }

    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override
    {
        if (device == nullptr)
            return -1;
        return device->getName() == (asInput ? mockInputName : mockOutputName) ? 0 : -1;
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice(const juce::String& outputName,
                                      const juce::String& inputName) override
    {
        if (! available)
            return nullptr;
        const auto validOutput = outputName.isEmpty() || outputName == mockOutputName;
        const auto validInput = inputName.isEmpty() || inputName == mockInputName;
        if (! validOutput || ! validInput || (outputName.isEmpty() && inputName.isEmpty()))
            return nullptr;
        return new MockAudioDevice(outputName, inputName);
    }

    void setAvailable(bool shouldBeAvailable)
    {
        available = shouldBeAvailable;
        callDeviceChangeListeners();
    }

private:
    bool available { true };
};

juce::AudioDeviceManager::AudioDeviceSetup validSetup()
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = mockInputName;
    setup.outputDeviceName = mockOutputName;
    setup.sampleRate = 48000.0;
    setup.bufferSize = 256;
    setup.inputChannels.setRange(0, 2, true);
    setup.outputChannels.setRange(0, 2, true);
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    return setup;
}

void testNoDeviceStartup(TestHarness& tests)
{
    juce::AudioDeviceManager manager;
    const auto error = manager.initialise(0, 0, nullptr, false);
    tests.expect(error.isEmpty(), "standalone device manager starts without requesting a device");
    tests.expect(manager.getCurrentAudioDevice() == nullptr, "zero-channel startup leaves audio safely disabled");
}

void testDeviceConfigurationAndRecovery(TestHarness& tests)
{
    juce::AudioDeviceManager manager;
    auto type = std::make_unique<MockAudioDeviceType>();
    auto* typePointer = type.get();
    manager.addAudioDeviceType(std::move(type));
    manager.setCurrentAudioDeviceType(mockTypeName, false);

    auto setup = validSetup();
    auto error = manager.initialise(2, 2, nullptr, false, {}, &setup);
    tests.expect(error.isEmpty(), "mock device opens through AudioDeviceManager");
    tests.expect(manager.getCurrentAudioDevice() != nullptr, "selected device becomes active");

    setup = manager.getAudioDeviceSetup();
    setup.sampleRate = 96000.0;
    setup.bufferSize = 32;
    error = manager.setAudioDeviceSetup(setup, true);
    tests.expect(error.isEmpty(), "device sample rate and buffer size can change");
    tests.expectNear(manager.getAudioDeviceSetup().sampleRate, 96000.0, 0.1,
                     "device manager publishes the new sample rate");
    tests.expectEqual(manager.getAudioDeviceSetup().bufferSize, 32,
                      "device manager publishes the new buffer size");

    const auto savedState = manager.createStateXml();
    tests.expect(savedState != nullptr, "last valid device configuration is serializable");

    for (int iteration = 0; iteration < 100; ++iteration)
    {
        manager.closeAudioDevice();
        tests.expect(manager.getCurrentAudioDevice() == nullptr, "device closes cleanly");
        manager.restartLastAudioDevice();
        tests.expect(manager.getCurrentAudioDevice() != nullptr, "last device restarts cleanly");
    }

    const auto lastKnownValidSetup = manager.getAudioDeviceSetup();
    typePointer->setAvailable(false);
    manager.closeAudioDevice();
    manager.restartLastAudioDevice();
    tests.expect(manager.getCurrentAudioDevice() == nullptr,
                 "device disappearance fails safely without an active device");

    typePointer->setAvailable(true);
    error = manager.setAudioDeviceSetup(lastKnownValidSetup, true);
    tests.expect(error.isEmpty() && manager.getCurrentAudioDevice() != nullptr,
                 "saved valid setup recovers the device when it returns");

    setup = manager.getAudioDeviceSetup();
    setup.sampleRate = 12345.0;
    error = manager.setAudioDeviceSetup(setup, true);
    const auto unsupportedWasNotActivated = error.isNotEmpty()
        || std::abs(manager.getAudioDeviceSetup().sampleRate - 12345.0) > 0.5;
    tests.expect(unsupportedWasNotActivated,
                 "unsupported device configuration errors or falls back to a supported value");

    error = manager.setAudioDeviceSetup(validSetup(), true);
    tests.expect(error.isEmpty() && manager.getCurrentAudioDevice() != nullptr,
                 "last known valid configuration can be restored after failure");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    TestHarness tests;
    testNoDeviceStartup(tests);
    testDeviceConfigurationAndRecovery(tests);
    const auto inputOne = nts::standalone::inputChannelsForRouting(1, 2);
    const auto inputTwo = nts::standalone::inputChannelsForRouting(2, 2);
    const auto stereo = nts::standalone::inputChannelsForRouting(
        nts::standalone::stereoInputRoutingId, 2);
    tests.expect(inputOne.countNumberOfSetBits() == 1 && inputOne[0] && ! inputOne[1],
                 "mono input 1 routing excludes the noisy second hardware input");
    tests.expect(inputTwo.countNumberOfSetBits() == 1 && ! inputTwo[0] && inputTwo[1],
                 "mono input 2 routing selects only the second hardware input");
    tests.expect(stereo.countNumberOfSetBits() == 2 && stereo[0] && stereo[1],
                 "stereo routing explicitly enables both hardware inputs");
    return tests.result();
}
