#include "nts/diagnostics/StructuredLogger.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace nts::diagnostics
{
namespace
{
std::string escapeJson(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const auto character : text)
    {
        switch (character)
        {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += character; break;
        }
    }
    return escaped;
}

const char* severityName(LogSeverity severity) noexcept
{
    switch (severity)
    {
        case LogSeverity::debug: return "debug";
        case LogSeverity::info: return "info";
        case LogSeverity::warning: return "warning";
        case LogSeverity::error: return "error";
    }
    return "unknown";
}
} // namespace

StructuredLogger::StructuredLogger(std::filesystem::path logFile, DiagnosticsCollector* audioSource)
    : destination(std::move(logFile)), diagnosticsSource(audioSource),
      worker([this](std::stop_token token) { run(token); })
{
}

StructuredLogger::~StructuredLogger()
{
    worker.request_stop();
    wakeup.notify_all();
}

void StructuredLogger::log(LogEvent event)
{
    {
        std::scoped_lock lock(queueMutex);
        queue.push_back(std::move(event));
    }
    wakeup.notify_one();
}

void StructuredLogger::drainAudioEvents(DiagnosticsCollector& collector)
{
    AudioEvent audioEvent;
    while (collector.pollEvent(audioEvent))
        log(convert(audioEvent));
}

void StructuredLogger::run(std::stop_token stopToken)
{
    std::error_code error;
    if (const auto parent = destination.parent_path(); ! parent.empty())
        std::filesystem::create_directories(parent, error);

    std::ofstream output(destination, std::ios::app);
    while (! stopToken.stop_requested())
    {
        std::deque<LogEvent> pending;
        {
            std::unique_lock lock(queueMutex);
            wakeup.wait_for(lock, stopToken, std::chrono::milliseconds(100), [this] { return ! queue.empty(); });
            pending.swap(queue);
        }

        if (diagnosticsSource != nullptr)
        {
            AudioEvent audioEvent;
            while (diagnosticsSource->pollEvent(audioEvent))
                pending.push_back(convert(audioEvent));
        }

        for (const auto& event : pending)
            output << serialize(event) << '\n';
        output.flush();
    }

    std::scoped_lock lock(queueMutex);
    for (const auto& event : queue)
        output << serialize(event) << '\n';
}

LogEvent StructuredLogger::convert(const AudioEvent& event)
{
    return { std::chrono::system_clock::now(),
             event.code == AudioEventCode::callbackOverrun ? LogSeverity::warning : LogSeverity::info,
             "audio",
             std::to_string(static_cast<unsigned int>(event.code)),
             "Audio-thread diagnostic event",
             "samplePosition=" + std::to_string(event.samplePosition)
                 + ",valueA=" + std::to_string(event.valueA)
                 + ",valueB=" + std::to_string(event.valueB) };
}

std::string StructuredLogger::serialize(const LogEvent& event)
{
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        event.timestamp.time_since_epoch()).count();
    std::ostringstream stream;
    stream << "{\"timestampMs\":" << millis
           << ",\"severity\":\"" << severityName(event.severity)
           << "\",\"subsystem\":\"" << escapeJson(event.subsystem)
           << "\",\"eventCode\":\"" << escapeJson(event.eventCode)
           << "\",\"message\":\"" << escapeJson(event.message)
           << "\",\"context\":\"" << escapeJson(event.context) << "\"}";
    return stream.str();
}
} // namespace nts::diagnostics
