#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace nts::diagnostics
{
class BackgroundWorker
{
public:
    using Job = std::function<void(std::stop_token)>;
    using FailureHandler = std::function<void()>;

    explicit BackgroundWorker(FailureHandler onFailure = {})
        : failureHandler(std::move(onFailure)), thread([this](std::stop_token token) { run(token); })
    {
    }

    ~BackgroundWorker()
    {
        thread.request_stop();
        wakeup.notify_all();
    }

    void submit(Job job)
    {
        {
            std::scoped_lock lock(mutex);
            jobs.push_back(std::move(job));
        }
        wakeup.notify_one();
    }

private:
    void run(std::stop_token token)
    {
        while (! token.stop_requested())
        {
            Job job;
            {
                std::unique_lock lock(mutex);
                wakeup.wait(lock, token, [this] { return ! jobs.empty(); });
                if (jobs.empty())
                    continue;
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            try
            {
                job(token);
            }
            catch (...)
            {
                if (failureHandler)
                    failureHandler();
            }
        }
    }

    FailureHandler failureHandler;
    std::mutex mutex;
    std::condition_variable_any wakeup;
    std::deque<Job> jobs;
    std::jthread thread;
};
} // namespace nts::diagnostics
