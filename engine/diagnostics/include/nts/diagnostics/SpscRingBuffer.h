#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <type_traits>

namespace nts::diagnostics
{
template <typename Value, std::size_t Capacity>
requires std::is_trivially_copyable_v<Value>
class SpscRingBuffer
{
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    bool push(const Value& value) noexcept
    {
        const auto currentWrite = writeIndex.load(std::memory_order_relaxed);
        const auto nextWrite = increment(currentWrite);
        if (nextWrite == readIndex.load(std::memory_order_acquire))
            return false;

        storage[currentWrite] = value;
        writeIndex.store(nextWrite, std::memory_order_release);
        return true;
    }

    bool pop(Value& value) noexcept
    {
        const auto currentRead = readIndex.load(std::memory_order_relaxed);
        if (currentRead == writeIndex.load(std::memory_order_acquire))
            return false;

        value = storage[currentRead];
        readIndex.store(increment(currentRead), std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return readIndex.load(std::memory_order_acquire) == writeIndex.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t increment(std::size_t index) noexcept
    {
        return (index + 1) & (Capacity - 1);
    }

    alignas(64) std::array<Value, Capacity> storage {};
    alignas(64) std::atomic<std::size_t> writeIndex {};
    alignas(64) std::atomic<std::size_t> readIndex {};
};
} // namespace nts::diagnostics
