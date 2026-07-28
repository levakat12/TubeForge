#include "nts/ml/NeuralAmpProcessor.h"

#include <algorithm>
#include <cmath>

namespace nts::ml
{
namespace
{
float amplitudeToDb(float value) noexcept
{
    return 20.0f * std::log10(std::max(value, 1.0e-6f));
}
float dbToAmplitude(float value) noexcept { return std::pow(10.0f, value / 20.0f); }
}

void InputCalibrationMonitor::prepare(double sampleRate) noexcept
{
    smoothingCoefficient = std::exp(-1.0 / (std::max(1.0, sampleRate) * 0.4)); reset();
}
void InputCalibrationMonitor::reset() noexcept { energy = 0.0; peak = 0.0f; }
void InputCalibrationMonitor::setExpectedRmsDb(float expected) noexcept
{
    expectedRmsDb = std::clamp(expected, -60.0f, 0.0f);
}
void InputCalibrationMonitor::process(std::span<const float> samples) noexcept
{
    for (const auto sample : samples)
    {
        const auto value = std::isfinite(sample) ? sample : 0.0f;
        energy = smoothingCoefficient * energy + (1.0 - smoothingCoefficient) * value * value;
        peak = std::max(peak * 0.9995f, std::abs(value));
    }
}
CalibrationReading InputCalibrationMonitor::reading() const noexcept
{
    const auto rms = amplitudeToDb(static_cast<float>(std::sqrt(std::max(energy, 0.0))));
    const auto mismatch = rms - expectedRmsDb;
    return { rms, amplitudeToDb(peak), mismatch, std::clamp(-mismatch, -12.0f, 12.0f),
             std::abs(mismatch) > 3.0f };
}

void NeuralAmpProcessor::prepare(double sampleRate, std::size_t maximumBlockSize, std::size_t channels)
{
    preparedSampleRate = std::max(1.0, sampleRate); preparedBlockSize = std::max<std::size_t>(1, maximumBlockSize);
    preparedChannels = std::clamp(channels, std::size_t { 1 }, maximumChannels);
    for (std::size_t channel = 0; channel < maximumChannels; ++channel)
    {
        dryScratch[channel].assign(preparedBlockSize, 0.0f);
        oldScratch[channel].assign(preparedBlockSize, 0.0f);
        newScratch[channel].assign(preparedBlockSize, 0.0f);
    }
    calibration.prepare(preparedSampleRate); reset();
}

void NeuralAmpProcessor::reset() noexcept
{
    for (auto& slot : slots) for (auto& model : slot.models) model.reset();
    calibration.reset(); fadePosition = 0; fadingFromSlot = -1; resetRequested.store(false, std::memory_order_relaxed);
}

bool NeuralAmpProcessor::stageModel(std::span<const std::byte> bytes,
                                    std::span<const float> testInput,
                                    std::span<const float> expectedOutput,
                                    float maximumError,
                                    float expectedInputRmsDb,
                                    std::string& error)
{
    if (testInput.empty() || testInput.size() != expectedOutput.size() || maximumError <= 0.0f)
    { error = "Model test vectors are invalid"; return false; }
    const auto active = activeSlot.load(std::memory_order_acquire);
    const auto pending = pendingSlot.load(std::memory_order_acquire);
    if (pending >= 0) { error = "A model is already pending activation"; return false; }
    const auto destination = active == 0 ? 1 : 0;
    if (destination == fadingFromSlot) { error = "Previous model is still crossfading"; return false; }
    Slot candidate;
    for (std::size_t channel = 0; channel < preparedChannels; ++channel)
    {
        if (! candidate.models[channel].load(bytes, error)) return false;
        if (candidate.models[channel].sampleRate() != static_cast<int>(std::lround(preparedSampleRate)))
        { error = "Model sample rate does not match the audio device"; return false; }
    }
    std::vector<float> actual(testInput.size());
    if (! candidate.models[0].process(testInput, actual)) { error = "Model test vector could not run"; return false; }
    for (std::size_t index = 0; index < actual.size(); ++index)
        if (! std::isfinite(actual[index]) || std::abs(actual[index] - expectedOutput[index]) > maximumError)
        { error = "Model test-vector validation failed"; return false; }
    for (auto& model : candidate.models) model.reset();
    candidate.expectedRmsDb = std::clamp(expectedInputRmsDb, -60.0f, 0.0f);
    slots[static_cast<std::size_t>(destination)] = std::move(candidate);
    pendingSlot.store(destination, std::memory_order_release); error.clear(); return true;
}

void NeuralAmpProcessor::setControls(std::span<const float> values) noexcept
{
    if (values.size() != controlCount) return;
    for (std::size_t index = 0; index < controlCount; ++index)
        if (std::isfinite(values[index])) controls[index].store(std::clamp(values[index], -1.0f, 1.0f), std::memory_order_relaxed);
}

void NeuralAmpProcessor::process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept
{
    const auto count = std::min({ channelCount, preparedChannels, maximumChannels });
    if (samples > preparedBlockSize || count == 0) return;
    if (resetRequested.exchange(false, std::memory_order_acq_rel))
    {
        const auto current = activeSlot.load(std::memory_order_acquire);
        if (current >= 0)
            for (auto& model : slots[static_cast<std::size_t>(current)].models) model.reset();
        const auto previous = fadingFromSlot.load(std::memory_order_acquire);
        if (previous >= 0 && previous != current)
            for (auto& model : slots[static_cast<std::size_t>(previous)].models) model.reset();
        calibration.reset();
    }
    const auto pending = pendingSlot.exchange(-1, std::memory_order_acq_rel);
    if (pending >= 0)
    {
        fadingFromSlot = activeSlot.exchange(pending, std::memory_order_acq_rel); fadePosition = 0;
        for (auto& model : slots[static_cast<std::size_t>(pending)].models) model.reset();
        calibration.setExpectedRmsDb(slots[static_cast<std::size_t>(pending)].expectedRmsDb);
    }
    const auto active = activeSlot.load(std::memory_order_acquire);
    if (active < 0) return;
    std::array<float, controlCount> controlValues {};
    for (std::size_t index = 0; index < controlCount; ++index) controlValues[index] = controls[index].load(std::memory_order_relaxed);
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        std::copy_n(channels[channel], samples, dryScratch[channel].begin());
        calibration.process(std::span(dryScratch[channel].data(), samples));
    }
    auto compensation = 1.0f;
    if (compensateInput.load(std::memory_order_relaxed)) compensation = dbToAmplitude(calibration.reading().suggestedCompensationDb);
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        auto& activeModel = slots[static_cast<std::size_t>(active)].models[channel]; activeModel.setControls(controlValues);
        for (std::size_t sample = 0; sample < samples; ++sample) newScratch[channel][sample] = dryScratch[channel][sample] * compensation;
        activeModel.process(std::span(newScratch[channel].data(), samples), std::span(newScratch[channel].data(), samples));
        if (fadingFromSlot >= 0)
        {
            auto& oldModel = slots[static_cast<std::size_t>(fadingFromSlot)].models[channel]; oldModel.setControls(controlValues);
            std::copy_n(dryScratch[channel].begin(), samples, oldScratch[channel].begin());
            oldModel.process(std::span(oldScratch[channel].data(), samples), std::span(oldScratch[channel].data(), samples));
        }
    }
    const auto mode = monitorMode.load(std::memory_order_relaxed);
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
        const auto crossfade = fadingFromSlot < 0 ? 1.0f
            : std::min(1.0f, static_cast<float>(fadePosition + sample) / static_cast<float>(fadeSamples));
        for (std::size_t channel = 0; channel < count; ++channel)
        {
            auto modelOutput = fadingFromSlot < 0 ? newScratch[channel][sample]
                : oldScratch[channel][sample] + crossfade * (newScratch[channel][sample] - oldScratch[channel][sample]);
            if (mode == NeuralMonitorMode::bypassDi) modelOutput = dryScratch[channel][sample];
            else if (mode == NeuralMonitorMode::loudnessMatchedModel)
                modelOutput *= dbToAmplitude(std::clamp(-calibration.reading().mismatchDb, -6.0f, 6.0f));
            channels[channel][sample] = std::isfinite(modelOutput) ? modelOutput : 0.0f;
        }
    }
    if (fadingFromSlot >= 0)
    {
        fadePosition += samples;
        if (fadePosition >= fadeSamples) { fadingFromSlot = -1; fadePosition = 0; }
    }
}

std::size_t NeuralAmpProcessor::modelMemoryBytes() const noexcept
{
    const auto active = activeSlot.load(std::memory_order_acquire);
    if (active < 0) return 0;
    std::size_t bytes {};
    for (const auto& model : slots[static_cast<std::size_t>(active)].models) bytes += model.memoryBytes();
    return bytes;
}
int NeuralAmpProcessor::modelSampleRate() const noexcept
{
    const auto active = activeSlot.load(std::memory_order_acquire);
    return active < 0 ? 0 : slots[static_cast<std::size_t>(active)].models[0].sampleRate();
}
} // namespace nts::ml
