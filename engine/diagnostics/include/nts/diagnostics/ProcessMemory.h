#pragma once

#include <cstdint>

namespace nts::diagnostics
{
struct ProcessMemorySnapshot
{
    std::uint64_t workingSetBytes {};
    std::uint64_t privateBytes {};
};

// Non-real-time API. Call only from UI, diagnostics, or worker threads.
[[nodiscard]] ProcessMemorySnapshot sampleProcessMemory() noexcept;
} // namespace nts::diagnostics
