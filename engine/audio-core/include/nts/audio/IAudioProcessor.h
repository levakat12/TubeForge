#pragma once

#include "AudioProcessContext.h"

#include <cstddef>

namespace nts::audio
{
class IAudioProcessor
{
public:
    virtual ~IAudioProcessor() = default;

    virtual void prepare(double sampleRate,
                         std::size_t maxBlockSize,
                         std::size_t numInputChannels,
                         std::size_t numOutputChannels) = 0;
    virtual void reset() noexcept = 0;
    virtual void process(AudioProcessContext& context) noexcept = 0;
};
} // namespace nts::audio
