#include "TestHarness.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>

int main()
{
    TestHarness tests;
    std::unique_ptr<juce::AudioIODeviceType> asio(
        juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    tests.expect(asio != nullptr, "JUCE registers the ASIO device backend");
    if (asio != nullptr)
        tests.expectEqual(asio->getTypeName().toStdString(), std::string("ASIO"),
                          "ASIO backend reports the expected type name");
    return tests.result();
}
