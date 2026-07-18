#pragma once

#include <nts/diagnostics/BackgroundWorker.h>
#include <nts/dsp/Convolution.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <string>

namespace nts::ir
{
struct CabinetLoadResult
{
    nts::dsp::ImpulseResponse impulse;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty() && ! impulse.channels.empty(); }
};

class CabinetIrLoader
{
public:
    using Completion = std::function<void(CabinetLoadResult)>;

    CabinetIrLoader();
    void loadAsync(juce::File file, double targetSampleRate,
                   nts::dsp::ImpulsePreparationOptions options, Completion completion);

private:
    juce::AudioFormatManager formats;
    nts::diagnostics::BackgroundWorker worker;
};
} // namespace nts::ir
