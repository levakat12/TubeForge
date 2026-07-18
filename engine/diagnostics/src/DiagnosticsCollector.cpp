#include "nts/diagnostics/DiagnosticsCollector.h"

#include <algorithm>
#include <cmath>

namespace nts::diagnostics
{
void DiagnosticsCollector::prepare(double sampleRate, std::uint32_t blockSize) noexcept
{
    expectedSampleRate.store(sampleRate, std::memory_order_relaxed);
    const auto deadline = std::chrono::duration<double>(static_cast<double>(blockSize) / sampleRate);
    deadlineNanoseconds.store(toNanoseconds(std::chrono::duration_cast<Clock::duration>(deadline)),
                              std::memory_order_relaxed);
    callbackNanoseconds.store(0, std::memory_order_relaxed);
    maximumCallbackNanoseconds.store(0, std::memory_order_relaxed);
}

DiagnosticsCollector::StartTime DiagnosticsCollector::beginCallback() const noexcept
{
    return Clock::now();
}

void DiagnosticsCollector::endCallback(StartTime startedAt,
                                       std::uint32_t renderedSamples,
                                       std::uint64_t absoluteSamplePosition) noexcept
{
    const auto elapsed = toNanoseconds(Clock::now() - startedAt);
    callbackNanoseconds.store(elapsed, std::memory_order_relaxed);

    auto previousMaximum = maximumCallbackNanoseconds.load(std::memory_order_relaxed);
    while (elapsed > previousMaximum
           && ! maximumCallbackNanoseconds.compare_exchange_weak(previousMaximum, elapsed,
                                                                  std::memory_order_relaxed))
    {
    }

    const auto sampleRate = expectedSampleRate.load(std::memory_order_relaxed);
    const auto deadline = toNanoseconds(std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(static_cast<double>(renderedSamples) / sampleRate)));
    deadlineNanoseconds.store(deadline, std::memory_order_relaxed);

    if (elapsed > deadline)
    {
        dropoutCount.fetch_add(1, std::memory_order_relaxed);
        pushAudioEvent({ AudioEventCode::callbackOverrun, 0,
                         static_cast<std::uint32_t>(std::min<std::uint64_t>(elapsed / 1000, UINT32_MAX)),
                         static_cast<std::uint32_t>(std::min<std::uint64_t>(deadline / 1000, UINT32_MAX)),
                         absoluteSamplePosition });
    }
}

void DiagnosticsCollector::verifySampleRate(double actualSampleRate, std::uint64_t samplePosition) noexcept
{
    const auto expected = expectedSampleRate.load(std::memory_order_relaxed);
    if (std::abs(expected - actualSampleRate) < 0.5)
        return;

    pushAudioEvent({ AudioEventCode::sampleRateMismatch, 0,
                     static_cast<std::uint32_t>(expected),
                     static_cast<std::uint32_t>(actualSampleRate),
                     samplePosition });
}

void DiagnosticsCollector::setLatencyBudget(const LatencyBudget& budget) noexcept
{
    graphLatencySamples.store(budget.processingSamples(), std::memory_order_relaxed);
}

void DiagnosticsCollector::setProcessMemory(std::uint64_t workingSetBytes,
                                            std::uint64_t privateBytes) noexcept
{
    processWorkingSetBytes.store(workingSetBytes, std::memory_order_relaxed);
    processPrivateBytes.store(privateBytes, std::memory_order_relaxed);
}

void DiagnosticsCollector::setModelLoadStatus(AssetLoadStatus status) noexcept
{
    currentModelLoadStatus.store(status, std::memory_order_relaxed);
}

void DiagnosticsCollector::setIrLoadStatus(AssetLoadStatus status) noexcept
{
    currentIrLoadStatus.store(status, std::memory_order_relaxed);
}

void DiagnosticsCollector::reportBackgroundJobFailure(std::uint32_t jobCode) noexcept
{
    static_cast<void>(jobCode);
    backgroundJobFailureCount.fetch_add(1, std::memory_order_relaxed);
}

void DiagnosticsCollector::pushAudioEvent(const AudioEvent& event) noexcept
{
    if (! events.push(event))
        queueOverflowCount.fetch_add(1, std::memory_order_relaxed);
}

bool DiagnosticsCollector::pollEvent(AudioEvent& event) noexcept
{
    return events.pop(event);
}

DiagnosticsSnapshot DiagnosticsCollector::snapshot() const noexcept
{
    constexpr auto nanosPerMillisecond = 1.0e6;
    const auto callback = static_cast<double>(callbackNanoseconds.load(std::memory_order_relaxed));
    const auto maximum = static_cast<double>(maximumCallbackNanoseconds.load(std::memory_order_relaxed));
    const auto deadline = static_cast<double>(deadlineNanoseconds.load(std::memory_order_relaxed));

    return {
        callback / nanosPerMillisecond,
        maximum / nanosPerMillisecond,
        deadline / nanosPerMillisecond,
        deadline > 0.0 ? callback * 100.0 / deadline : 0.0,
        dropoutCount.load(std::memory_order_relaxed),
        queueOverflowCount.load(std::memory_order_relaxed),
        graphLatencySamples.load(std::memory_order_relaxed),
        processWorkingSetBytes.load(std::memory_order_relaxed),
        processPrivateBytes.load(std::memory_order_relaxed),
        currentModelLoadStatus.load(std::memory_order_relaxed),
        currentIrLoadStatus.load(std::memory_order_relaxed),
        backgroundJobFailureCount.load(std::memory_order_relaxed)
    };
}

std::uint64_t DiagnosticsCollector::toNanoseconds(Clock::duration duration) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}
} // namespace nts::diagnostics
