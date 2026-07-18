#pragma once

#include "DiagnosticsCollector.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace nts::diagnostics
{
enum class LogSeverity : std::uint8_t
{
    debug,
    info,
    warning,
    error
};

struct LogEvent
{
    std::chrono::system_clock::time_point timestamp { std::chrono::system_clock::now() };
    LogSeverity severity { LogSeverity::info };
    std::string subsystem;
    std::string eventCode;
    std::string message;
    std::string context;
};

class StructuredLogger
{
public:
    explicit StructuredLogger(std::filesystem::path logFile, DiagnosticsCollector* audioSource = nullptr);
    ~StructuredLogger();

    StructuredLogger(const StructuredLogger&) = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;

    void log(LogEvent event);
    void drainAudioEvents(DiagnosticsCollector& collector);

private:
    void run(std::stop_token stopToken);
    static std::string serialize(const LogEvent& event);
    static LogEvent convert(const AudioEvent& event);

    std::filesystem::path destination;
    DiagnosticsCollector* diagnosticsSource {};
    std::mutex queueMutex;
    std::condition_variable_any wakeup;
    std::deque<LogEvent> queue;
    std::jthread worker;
};
} // namespace nts::diagnostics
