#pragma once

#include "LatencyBudget.h"
#include "SpscRingBuffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace nts::diagnostics
{
enum class AudioEventCode : std::uint16_t
{
    callbackOverrun,
    sampleRateMismatch,
    eventQueueOverflow,
    stateRejected,
    backgroundJobFailure,
    modelLoadStatus,
    irLoadStatus
};

enum class AssetLoadStatus : std::uint8_t
{
    unavailable,
    idle,
    loading,
    ready,
    failed
};

struct AudioEvent
{
    AudioEventCode code {};
    std::uint16_t reserved {};
    std::uint32_t valueA {};
    std::uint32_t valueB {};
    std::uint64_t samplePosition {};
};

struct DiagnosticsSnapshot
{
    double callbackMilliseconds {};
    double maximumCallbackMilliseconds {};
    double deadlineMilliseconds {};
    double cpuLoadPercent {};
    std::uint64_t dropoutCount {};
    std::uint64_t queueOverflowCount {};
    int currentGraphLatencySamples {};
    std::uint64_t workingSetBytes {};
    std::uint64_t privateBytes {};
    AssetLoadStatus modelLoadStatus { AssetLoadStatus::unavailable };
    AssetLoadStatus irLoadStatus { AssetLoadStatus::unavailable };
    std::uint64_t backgroundJobFailureCount {};
};

class DiagnosticsCollector
{
public:
    using Clock = std::chrono::steady_clock;
    using StartTime = Clock::time_point;

    void prepare(double sampleRate, std::uint32_t blockSize) noexcept;
    [[nodiscard]] StartTime beginCallback() const noexcept;
    void endCallback(StartTime startedAt,
                     std::uint32_t renderedSamples,
                     std::uint64_t absoluteSamplePosition) noexcept;
    void verifySampleRate(double actualSampleRate, std::uint64_t samplePosition) noexcept;
    void setLatencyBudget(const LatencyBudget& budget) noexcept;
    void setProcessMemory(std::uint64_t workingSetBytes, std::uint64_t privateBytes) noexcept;
    void setModelLoadStatus(AssetLoadStatus status) noexcept;
    void setIrLoadStatus(AssetLoadStatus status) noexcept;
    void reportBackgroundJobFailure(std::uint32_t jobCode = 0) noexcept;
    bool pollEvent(AudioEvent& event) noexcept;
    [[nodiscard]] DiagnosticsSnapshot snapshot() const noexcept;

private:
    static std::uint64_t toNanoseconds(Clock::duration duration) noexcept;
    void pushAudioEvent(const AudioEvent& event) noexcept;

    SpscRingBuffer<AudioEvent, 256> events;
    std::atomic<std::uint64_t> callbackNanoseconds {};
    std::atomic<std::uint64_t> maximumCallbackNanoseconds {};
    std::atomic<std::uint64_t> deadlineNanoseconds {};
    std::atomic<std::uint64_t> dropoutCount {};
    std::atomic<std::uint64_t> queueOverflowCount {};
    std::atomic<int> graphLatencySamples {};
    std::atomic<std::uint64_t> processWorkingSetBytes {};
    std::atomic<std::uint64_t> processPrivateBytes {};
    std::atomic<AssetLoadStatus> currentModelLoadStatus { AssetLoadStatus::unavailable };
    std::atomic<AssetLoadStatus> currentIrLoadStatus { AssetLoadStatus::unavailable };
    std::atomic<std::uint64_t> backgroundJobFailureCount {};
    std::atomic<double> expectedSampleRate { 44100.0 };
};
} // namespace nts::diagnostics
