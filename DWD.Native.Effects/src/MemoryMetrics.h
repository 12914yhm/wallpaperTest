#pragma once
#include <windows.h>
#include <psapi.h>
#include <vector>

// 私有物理工作集，不是 PrivateUsage（提交量），也不是含共享页的 WorkingSetSize。
// 查询失败要向上报告，不能偷偷换成另一个口径或当成正常的 0 MB。
inline bool PrivateResidentBytes(HANDLE process, SIZE_T& bytes) {
    bytes = 0;
    if (!process) return true;
    DWORD exitCode{};
    if (GetExitCodeProcess(process, &exitCode) && exitCode != STILL_ACTIVE) return true;
    PROCESS_MEMORY_COUNTERS_EX2 memory{}; memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                            sizeof(memory)) && memory.PrivateWorkingSetSize) {
        bytes = memory.PrivateWorkingSetSize;
        return true;
    }
    // 老系统回退：只统计工作集里 Shared=0 的页，不把共享组件重复累计。
    std::vector<ULONG_PTR> buffer(16384);
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (QueryWorkingSet(process, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(ULONG_PTR)))) {
            const auto* info = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
            SYSTEM_INFO system{}; GetSystemInfo(&system);
            for (ULONG_PTR n = 0; n < info->NumberOfEntries; ++n)
                if (!info->WorkingSetInfo[n].Shared) bytes += system.dwPageSize;
            return true;
        }
        if (GetLastError() != ERROR_BAD_LENGTH) return false;
        buffer.resize(buffer.size() * 2);
    }
    return false;
}
