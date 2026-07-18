#include "nts/ir/CabinetIrLoader.h"

#include <algorithm>
#include <memory>

namespace nts::ir
{
CabinetIrLoader::CabinetIrLoader() { formats.registerBasicFormats(); }

void CabinetIrLoader::loadAsync(juce::File file, double targetSampleRate,
                                nts::dsp::ImpulsePreparationOptions options, Completion completion)
{
    worker.submit([this, file = std::move(file), targetSampleRate, options,
                   completion = std::move(completion)](std::stop_token stopToken) mutable
    {
        if (stopToken.stop_requested()) return;
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr)
        {
            completion({ {}, "Unable to decode cabinet IR: " + file.getFullPathName().toStdString() });
            return;
        }
        const auto channels = std::clamp<std::size_t>(reader->numChannels, 1, nts::dsp::maximumChannels);
        const auto length = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples, 4'000'000));
        juce::AudioBuffer<float> decoded(static_cast<int>(channels), length);
        if (! reader->read(&decoded, 0, length, 0, true, true))
        {
            completion({ {}, "Cabinet IR read failed" });
            return;
        }
        nts::dsp::ImpulseResponse raw;
        raw.sampleRate = reader->sampleRate;
        raw.channels.resize(channels);
        for (std::size_t channel = 0; channel < channels; ++channel)
            raw.channels[channel].assign(decoded.getReadPointer(static_cast<int>(channel)),
                                          decoded.getReadPointer(static_cast<int>(channel)) + length);
        if (stopToken.stop_requested()) return;
        completion({ nts::dsp::prepareImpulseResponse(raw, targetSampleRate, options), {} });
    });
}
} // namespace nts::ir
