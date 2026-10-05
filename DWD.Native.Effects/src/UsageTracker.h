#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "AsyncFileStore.h"

// 一条按“应用”聚合的当日活跃使用记录。
//
// DWD 只保存可执行文件路径、用于显示的程序名和累计毫秒数；不会保存窗口标题、
// 键盘内容、鼠标坐标，也不会截屏。path 同时作为稳定标识，避免两个同名程序混在一起。
struct UsageRecord {
    std::wstring path;
    std::wstring displayName;
    std::uint64_t activeMilliseconds{};
    // “观看”口径不要求持续产生输入：只要应用仍是前台且会话未锁定就累计。
    // 两个计数器同时维护，用户切换展示模式时不会把两种口径混在同一个数字里。
    std::uint64_t viewingMilliseconds{};
};

enum class UsageMode {
    Active,
    Viewing
};

// 轻量、事件驱动的应用使用时长统计器。
//
// SetWinEventHook 只在前台窗口变化时通知我们；两秒一次的 Tick 只读取系统最近输入时间，
// 因而不需要全局键鼠钩子，也不会持续枚举全部进程。所有公开方法均在 UI 线程调用。
class UsageTracker {
public:
    bool Start(HWND notificationWindow, UINT foregroundChangedMessage,
               const std::wstring& portableDataDirectory);
    void Stop();

    // 由主窗口的低频定时器调用。只有当前应用位于前台、会话未锁定，且用户在最近
    // idleThresholdMs 内有过输入时，才把这一小段时间累计到该应用。
    void Tick(std::uint64_t idleThresholdMs = 60'000);
    void HandleForegroundChanged();
    void SetSessionLocked(bool locked);
    void SetSuspended(bool suspended);
    void SetEnabled(bool enabled);
    // 历史只读取 DWD 自己的聚合记录，不伪造安装前的系统历史。
    std::vector<UsageRecord> RecordsForDay(const std::wstring& date, UsageMode mode) const;
    std::vector<std::wstring> AvailableDays() const;

    std::vector<UsageRecord> TodayRecords(UsageMode mode) const;
    std::uint64_t TodayTotalMilliseconds(UsageMode mode) const;
    const std::wstring& TodayKey() const { return todayKey_; }
    const std::wstring& CurrentAppName() const { return currentDisplayName_; }

private:
    void SelectCurrentForegroundApp();
    void RollToCurrentDayIfNeeded();
    void LoadToday();
    std::map<std::wstring, UsageRecord> LoadRecords(const std::wstring& date) const;
    void SaveToday(bool force);
    std::wstring TodayFilePath() const;

    HWND notificationWindow_{};
    UINT foregroundChangedMessage_{};
    HWINEVENTHOOK foregroundHook_{};
    std::wstring dataDirectory_;
    std::wstring todayKey_;
    std::map<std::wstring, UsageRecord> records_;
    std::wstring currentKey_;
    std::wstring currentDisplayName_;
    ULONGLONG lastTick_{};
    ULONGLONG lastSaveTick_{};
    ULONGLONG lastAwakeTime_{};
    bool sessionLocked_{};
    bool suspended_{};
    bool enabled_{true};
    bool dirty_{};
    AsyncFileStore store_;
};
