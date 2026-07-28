#include <nts/circuit/CircuitProcessor.h>

#include <algorithm>

namespace nts::circuit
{
void CircuitProcessor::prepare(const nts::dsp::ProcessSpec& spec)
{
    currentSpec = spec;
    currentSpec.channels = std::clamp<std::size_t>(spec.channels, 1, 2);
    oldOutput.resize(spec.maximumBlockSize);
    newOutput.resize(spec.maximumBlockSize);
    reset();
}

void CircuitProcessor::reset() noexcept
{
    for (auto& slot : slots) if (slot) for (auto& channel : slot->channels) if (channel) channel->reset();
    crossfadePosition = crossfadeLength;
    fadingFromSlot = -1;
    crossfadeActive.store(false, std::memory_order_release);
}

ValidationReport CircuitProcessor::stageGraph(const CircuitGraphDescription& graph, const NeuralModelBytes& neuralModels)
{
    ValidationReport report;
    if (pendingSlot.load(std::memory_order_acquire) >= 0 || crossfadeActive.load(std::memory_order_acquire))
    {
        report.messages.push_back({ ValidationMessage::Severity::warning, {}, "A graph publication is already pending" });
        return report;
    }
    const auto active = activeSlot.load(std::memory_order_acquire);
    const auto target = active == 0 ? 1 : 0;
    auto candidate = std::make_unique<Slot>();
    for (std::size_t channel = 0; channel < currentSpec.channels; ++channel)
    {
        ValidationReport channelReport;
        candidate->channels[channel] = CircuitCompiler::compile(graph, { currentSpec.sampleRate, currentSpec.maximumBlockSize, 1 },
                                                                 neuralModels, channelReport);
        report.messages.insert(report.messages.end(), channelReport.messages.begin(), channelReport.messages.end());
        if (!channelReport.isValid()) return report;
    }
    slots[static_cast<std::size_t>(target)] = std::move(candidate);
    pendingSlot.store(target, std::memory_order_release);
    return report;
}

void CircuitProcessor::publishPendingWithoutCrossfade() noexcept
{
    const auto pending = pendingSlot.exchange(-1, std::memory_order_acq_rel);
    if (pending >= 0) activeSlot.store(pending, std::memory_order_release);
    fadingFromSlot = -1;
    crossfadePosition = crossfadeLength;
    crossfadeActive.store(false, std::memory_order_release);
}

void CircuitProcessor::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min(samples, currentSpec.maximumBlockSize);
    const auto pending = pendingSlot.exchange(-1, std::memory_order_acq_rel);
    if (pending >= 0)
    {
        fadingFromSlot = activeSlot.exchange(pending, std::memory_order_acq_rel);
        crossfadePosition = fadingFromSlot >= 0 ? 0 : crossfadeLength;
        crossfadeActive.store(fadingFromSlot >= 0, std::memory_order_release);
    }
    const auto active = activeSlot.load(std::memory_order_acquire);
    if (active < 0 || slots[static_cast<std::size_t>(active)] == nullptr) return;
    const auto fadeStart = crossfadePosition;
    for (std::size_t channel = 0; channel < std::min(channelCount, currentSpec.channels); ++channel)
    {
        auto input = std::span<const float>(channels[channel], count);
        auto destination = std::span<float>(channels[channel], count);
        auto fresh = std::span<float>(newOutput).first(count);
        slots[static_cast<std::size_t>(active)]->channels[channel]->process(input, fresh);
        if (fadingFromSlot >= 0 && fadeStart < crossfadeLength
            && slots[static_cast<std::size_t>(fadingFromSlot)] != nullptr)
        {
            auto previous = std::span<float>(oldOutput).first(count);
            slots[static_cast<std::size_t>(fadingFromSlot)]->channels[channel]->process(input, previous);
            for (std::size_t sample = 0; sample < count; ++sample)
            {
                const auto mix = std::min(1.0f, static_cast<float>(fadeStart + sample) / static_cast<float>(crossfadeLength));
                destination[sample] = previous[sample] + (fresh[sample] - previous[sample]) * mix;
            }
        }
        else std::copy(fresh.begin(), fresh.end(), destination.begin());
    }
    if (fadingFromSlot >= 0)
    {
        crossfadePosition = std::min(crossfadeLength, crossfadePosition + count);
        if (crossfadePosition >= crossfadeLength)
        {
            fadingFromSlot = -1;
            crossfadeActive.store(false, std::memory_order_release);
        }
    }
}

std::vector<NodeTelemetry> CircuitProcessor::telemetrySnapshot() const
{
    const auto active = activeSlot.load(std::memory_order_acquire);
    if (active < 0 || slots[static_cast<std::size_t>(active)] == nullptr
        || slots[static_cast<std::size_t>(active)]->channels[0] == nullptr) return {};
    return slots[static_cast<std::size_t>(active)]->channels[0]->telemetrySnapshot();
}

std::size_t CircuitProcessor::latencySamples() const noexcept
{
    const auto active = activeSlot.load(std::memory_order_acquire);
    if (active < 0 || slots[static_cast<std::size_t>(active)] == nullptr
        || slots[static_cast<std::size_t>(active)]->channels[0] == nullptr) return 0;
    return slots[static_cast<std::size_t>(active)]->channels[0]->latencySamples();
}
} // namespace nts::circuit
