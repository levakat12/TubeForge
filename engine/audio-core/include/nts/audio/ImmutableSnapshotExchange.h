#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace nts::audio
{
template <typename State>
class ImmutableSnapshotExchange
{
public:
    class AudioRead
    {
    public:
        explicit AudioRead(const ImmutableSnapshotExchange& owner) noexcept
            : exchange(owner)
        {
            exchange.audioReaders.fetch_add(1, std::memory_order_seq_cst);
            value = exchange.current.load(std::memory_order_seq_cst);
        }

        ~AudioRead()
        {
            exchange.audioReaders.fetch_sub(1, std::memory_order_seq_cst);
        }

        AudioRead(const AudioRead&) = delete;
        AudioRead& operator=(const AudioRead&) = delete;
        [[nodiscard]] const State* get() const noexcept { return value; }
        [[nodiscard]] const State& operator*() const noexcept { return *value; }
        [[nodiscard]] const State* operator->() const noexcept { return value; }

    private:
        const ImmutableSnapshotExchange& exchange;
        const State* value {};
    };

    explicit ImmutableSnapshotExchange(std::unique_ptr<const State> initial)
        : active(std::move(initial)), current(active.get())
    {
    }

    [[nodiscard]] AudioRead readForAudio() const noexcept { return AudioRead(*this); }

    // Called only from the owning non-audio thread.
    void publish(std::unique_ptr<const State> next)
    {
        auto previous = std::move(active);
        active = std::move(next);
        current.store(active.get(), std::memory_order_seq_cst);
        retired.push_back(std::move(previous));
        reclaimRetired();
    }

    // Called only from the owning non-audio thread.
    void reclaimRetired() noexcept
    {
        if (audioReaders.load(std::memory_order_seq_cst) == 0)
            retired.clear();
    }

private:
    std::unique_ptr<const State> active;
    std::vector<std::unique_ptr<const State>> retired;
    std::atomic<const State*> current {};
    mutable std::atomic<unsigned int> audioReaders {};

    static_assert(std::atomic<const State*>::is_always_lock_free);
    static_assert(std::atomic<unsigned int>::is_always_lock_free);
};
} // namespace nts::audio
