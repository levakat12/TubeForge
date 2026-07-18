#include "nts/diagnostics/ProcessMemory.h"

#if defined(_WIN32)
 #define NOMINMAX
 #include <windows.h>
 #include <psapi.h>
#elif defined(__APPLE__)
 #include <mach/mach.h>
#else
 #include <fstream>
 #include <unistd.h>
#endif

namespace nts::diagnostics
{
ProcessMemorySnapshot sampleProcessMemory() noexcept
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0)
        return {};
    return { static_cast<std::uint64_t>(counters.WorkingSetSize),
             static_cast<std::uint64_t>(counters.PrivateUsage) };
#elif defined(__APPLE__)
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return {};
    return { static_cast<std::uint64_t>(info.resident_size),
             static_cast<std::uint64_t>(info.virtual_size) };
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages {};
    std::uint64_t residentPages {};
    if (! (statm >> totalPages >> residentPages))
        return {};
    const auto pageSize = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    return { residentPages * pageSize, totalPages * pageSize };
#endif
}
} // namespace nts::diagnostics
