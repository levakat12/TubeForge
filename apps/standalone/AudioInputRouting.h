#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>

namespace nts::standalone
{
constexpr int stereoInputRoutingId = 1001;

inline juce::BigInteger inputChannelsForRouting(int routingId, int availableInputChannels)
{
    juce::BigInteger channels;
    if (availableInputChannels <= 0) return channels;
    if (routingId == stereoInputRoutingId)
        channels.setRange(0, std::min(2, availableInputChannels), true);
    else
        channels.setBit(std::clamp(routingId - 1, 0, availableInputChannels - 1));
    return channels;
}
} // namespace nts::standalone
