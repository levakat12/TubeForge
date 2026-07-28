#pragma once

#include "PackedTanhModel.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace nts::ml
{
struct CalibrationReading
{
    float rmsDb { -120.0f };
    float peakDb { -120.0f };
    float mismatchDb {};
    float suggestedCompensationDb {};
    bool warning {};
};

class InputCalibrationMonitor
{
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setExpectedRmsDb(float expected) noexcept;
    void process(std::span<const float> samples) noexcept;
    [[nodiscard]] CalibrationReading reading() const noexcept;

private:
    double smoothingCoefficient {};
    double energy {};
    float peak {};
    float expectedRmsDb { -21.0f };
};

enum class NeuralMonitorMode { model, bypassDi, loudnessMatchedModel };

class NeuralAmpProcessor
{
public:
    static constexpr std::size_t maximumChannels = 2;
    static constexpr std::size_t controlCount = 5;

    void prepare(double sampleRate, std::size_t maximumBlockSize, std::size_t channels);
    void reset() noexcept;
    bool stageModel(std::span<const std::byte> bytes,
                    std::span<const float> testInput,
                    std::span<const float> expectedOutput,
                    float maximumError,
                    float expectedInputRmsDb,
                    std::string& error);
    void process(float* const* channels, std::size_t channelCount, std::size_t samples) noexcept;
    void setControls(std::span<const float> values) noexcept;
    void setMonitorMode(NeuralMonitorMode mode) noexcept { monitorMode.store(mode, std::memory_order_relaxed); }
    void setInputCompensationEnabled(bool enabled) noexcept { compensateInput.store(enabled, std::memory_order_relaxed); }
    void requestStateReset() noexcept { resetRequested.store(true, std::memory_order_release); }

    [[nodiscard]] bool hasActiveModel() const noexcept { return activeSlot.load(std::memory_order_acquire) >= 0; }
    [[nodiscard]] CalibrationReading calibrationReading() const noexcept { return calibration.reading(); }
    [[nodiscard]] std::size_t modelMemoryBytes() const noexcept;
    [[nodiscard]] int modelSampleRate() const noexcept;

private:
    struct Slot { std::array<PackedTanhModel, maximumChannels> models; float expectedRmsDb { -21.0f }; };
    std::array<Slot, 2> slots;
    std::array<std::vector<float>, maximumChannels> dryScratch;
    std::array<std::vector<float>, maximumChannels> oldScratch;
    std::array<std::vector<float>, maximumChannels> newScratch;
    std::array<std::atomic<float>, controlCount> controls {};
    InputCalibrationMonitor calibration;
    std::atomic<int> activeSlot { -1 };
    std::atomic<int> pendingSlot { -1 };
    std::atomic<bool> resetRequested {};
    std::atomic<bool> compensateInput {};
    std::atomic<NeuralMonitorMode> monitorMode { NeuralMonitorMode::model };
    std::atomic<int> fadingFromSlot { -1 };
    std::size_t fadePosition {};
    std::size_t fadeSamples { 1024 };
    std::size_t preparedChannels { 1 };
    std::size_t preparedBlockSize {};
    double preparedSampleRate { 48000.0 };
};
} // namespace nts::ml
