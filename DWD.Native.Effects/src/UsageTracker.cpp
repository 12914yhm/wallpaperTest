#include "UsageTracker.h"

#include <psapi.h>
#include <shlwapi.h>
#include <winver.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace {
UsageTracker* g_activeTracker{};
// WinEvent 需要一个普通 C 回调；这两个值只负责把通知转发到 UI 线程。
HWND g_usageNotificationWindow{};
UINT g_usageForegroundMessage{};

// WinEvent 回调应尽量短：把真正的进程查询工作投递回 UI 线程，避免阻塞系统事件队列。
void CALLBACK ForegroundEventCallback(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG,
                                      DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && g_activeTracker) {
        // UsageTracker::Start 已把目标窗口和消息保存起来；这里通过一个只读辅助接口不值得
        // 暴露内部状态，因此发送到当前进程的主窗口由下面两个静态值承接。
        // 它们的生命周期与 g_activeTracker 完全一致，并且只在 UI 线程中修改。
        if (g_usageNotificationWindow && g_usageForegroundMessage) {
            PostMessageW(g_usageNotificationWindow, g_usageForegroundMessage, 0, 0);
        }
    }
}

std::wstring CurrentLocalDateKey() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t value[16]{};
    swprintf_s(value, L"%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
    return value;
}

std::wstring SanitizeField(std::wstring value) {
    for (wchar_t& character : value) {
        if (character == L'\t' || character == L'\r' || character == L'\n') character = L' ';
    }
    return value;
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<BYTE> version(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, version.data())) return {};

    struct Translation { WORD language; WORD codePage; };
    Translation* translations{};
    UINT translationBytes{};
    if (!VerQueryValueW(version.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translations), &translationBytes) ||
        translationBytes < sizeof(Translation)) {
        return {};
    }

    const size_t count = translationBytes / sizeof(Translation);
    for (size_t index = 0; index < count; ++index) {
        wchar_t query[80]{};
        swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                   translations[index].language, translations[index].codePage);
        wchar_t* description{};
        UINT characters{};
        if (VerQueryValueW(version.data(), query, reinterpret_cast<void**>(&description),
                           &characters) && description && characters > 1) {
            return description;
        }
    }
    return {};
}

std::wstring QueryProcessPath(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    std::vector<wchar_t> path(32768);
    DWORD length = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    return queried ? std::wstring(path.data(), length) : std::wstring{};
}

DWORD ForegroundProcessId(HWND foreground) {
    DWORD processId{};
    GetWindowThreadProcessId(foreground, &processId);
    if (!processId) return 0;

    // 旧式 UWP 窗口可能由 ApplicationFrameHost 承载。只有遇到这个明确的宿主时，
    // 才查找它的子窗口；普通浏览器拥有很多渲染子进程，绝不能用相同规则替换。
    std::wstring hostPath = QueryProcessPath(processId);
    std::wstring fileName = std::filesystem::path(hostPath).filename().wstring();
    std::transform(fileName.begin(), fileName.end(), fileName.begin(), towlower);
    if (fileName != L"applicationframehost.exe") return processId;

    struct ChildSearch { DWORD host{}; DWORD result{}; } search{processId, 0};
    EnumChildWindows(foreground,
        [](HWND child, LPARAM data) -> BOOL {
            auto* search = reinterpret_cast<ChildSearch*>(data);
            DWORD childProcess{};
            GetWindowThreadProcessId(child, &childProcess);
            if (childProcess && childProcess != search->host &&
                !QueryProcessPath(childProcess).empty()) {
                search->result = childProcess;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
    return search.result ? search.result : processId;
}

std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start{};
    while (start < text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        if (end == std::wstring::npos) end = text.size();
        if (end > start) lines.push_back(text.substr(start, end - start));
        start = end;
        while (start < text.size() && (text[start] == L'\r' || text[start] == L'\n')) ++start;
    }
    return lines;
}
}

bool UsageTracker::Start(HWND notificationWindow, UINT foregroundChangedMessage,
                         const std::wstring& portableDataDirectory) {
    Stop();
    notificationWindow_ = notificationWindow;
    foregroundChangedMessage_ = foregroundChangedMessage;
    dataDirectory_ = portableDataDirectory;
    todayKey_ = CurrentLocalDateKey();
    std::error_code error;
    std::filesystem::create_directories(dataDirectory_, error);
    LoadToday();

    g_activeTracker = this;
    g_usageNotificationWindow = notificationWindow_;
    g_usageForegroundMessage = foregroundChangedMessage_;
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                      nullptr, ForegroundEventCallback, 0, 0,
                                      WINEVENT_OUTOFCONTEXT);
    SelectCurrentForegroundApp();
    lastTick_ = lastSaveTick_ = GetTickCount64();
    QueryUnbiasedInterruptTime(&lastAwakeTime_);
    return foregroundHook_ != nullptr;
}

void UsageTracker::Stop() {
    if (!notificationWindow_ && !foregroundHook_) return;
    Tick();
    SaveToday(true);
    if (foregroundHook_) UnhookWinEvent(foregroundHook_);
    foregroundHook_ = nullptr;
    if (g_activeTracker == this) {
        g_activeTracker = nullptr;
        g_usageNotificationWindow = nullptr;
        g_usageForegroundMessage = 0;
    }
    notificationWindow_ = nullptr;
    foregroundChangedMessage_ = 0;
    currentKey_.clear();
    currentDisplayName_.clear();
    store_.Flush();
}

void UsageTracker::Tick(std::uint64_t idleThresholdMs) {
    if (!notificationWindow_) return;
    if (store_.TakeFailure()) dirty_ = true; // 磁盘暂不可写时保留数据，下次采样重新提交。
    RollToCurrentDayIfNeeded();
    const ULONGLONG now = GetTickCount64();
    // Unbiased 时钟不包含系统睡眠时间；正常加载视频造成的几秒 UI 等待不再丢掉。
    ULONGLONG awake{};
    QueryUnbiasedInterruptTime(&awake);
    const ULONGLONG elapsed = lastAwakeTime_ && awake >= lastAwakeTime_
        ? (awake - lastAwakeTime_) / 10'000 : 0;
    lastAwakeTime_ = awake;
    lastTick_ = now;

    LASTINPUTINFO input{sizeof(input)};
    const DWORD tick32 = GetTickCount();
    const bool recentlyActive = GetLastInputInfo(&input) != FALSE &&
                                static_cast<DWORD>(tick32 - input.dwTime) <= idleThresholdMs;
    // 异常长的调试/卡顿间隔仍保守丢弃，避免把未知期间全部算给原前台应用。
    if (enabled_ && !suspended_ && !sessionLocked_ &&
        !currentKey_.empty() && elapsed > 0 && elapsed <= 30'000) {
        auto found = records_.find(currentKey_);
        if (found != records_.end()) {
            // 始终同时维护两种统计口径。观看时长只要求应用处于前台；活跃时长
            // 还要求最近 60 秒内有键鼠输入。切换 UI 模式因此只影响展示，不会造成
            // 一天中前后两段使用不同规则、最后却混成一个数字。
            found->second.viewingMilliseconds += elapsed;
            if (recentlyActive) found->second.activeMilliseconds += elapsed;
            dirty_ = true;
        }
    }
    if (dirty_ && now - lastSaveTick_ >= 30'000) SaveToday(false);
}

void UsageTracker::HandleForegroundChanged() {
    Tick();
    SelectCurrentForegroundApp();
}

void UsageTracker::SetSessionLocked(bool locked) {
    Tick();
    sessionLocked_ = locked;
    if (!locked) SelectCurrentForegroundApp();
}

void UsageTracker::SetSuspended(bool suspended) {
    Tick();
    suspended_ = suspended;
    if (!suspended) SelectCurrentForegroundApp();
}

void UsageTracker::SetEnabled(bool enabled) {
    Tick();
    enabled_ = enabled;
    if (!enabled) SaveToday(true);
}

std::vector<std::wstring> UsageTracker::AvailableDays() const {
    std::vector<std::wstring> days{todayKey_};
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dataDirectory_,
             std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) break;
        const std::wstring date = entry.path().stem().wstring();
        if (entry.path().extension() == L".dwdusage" && date.size() == 10 &&
            date[4] == L'-' && date[7] == L'-' &&
            std::all_of(date.begin(), date.end(), [](wchar_t ch) {
                return (ch >= L'0' && ch <= L'9') || ch == L'-';
            })) days.push_back(date);
    }
    std::sort(days.begin(), days.end(), std::greater<>());
    days.erase(std::unique(days.begin(), days.end()), days.end());
    return days;
}

std::vector<UsageRecord> UsageTracker::RecordsForDay(
    const std::wstring& date, UsageMode mode) const {
    if (date == todayKey_) return TodayRecords(mode);
    // 日期只允许从自己的数据文件列表选择，防止拼接路径越出 UsageData。
    const auto days = AvailableDays();
    if (std::find(days.begin(), days.end(), date) == days.end()) return {};
    const auto records = LoadRecords(date);
    std::vector<UsageRecord> result;
    for (const auto& [_, record] : records) {
        if (mode == UsageMode::Active ? record.activeMilliseconds : record.viewingMilliseconds)
            result.push_back(record);
    }
    std::sort(result.begin(), result.end(), [mode](const auto& a, const auto& b) {
        const auto av = mode == UsageMode::Active ? a.activeMilliseconds : a.viewingMilliseconds;
        const auto bv = mode == UsageMode::Active ? b.activeMilliseconds : b.viewingMilliseconds;
        return av != bv ? av > bv : a.displayName < b.displayName;
    });
    return result;
}

std::vector<UsageRecord> UsageTracker::TodayRecords(UsageMode mode) const {
    std::vector<UsageRecord> result;
    result.reserve(records_.size());
    // 前台切换事件可能先建立一个应用条目，但用户尚未产生有效输入；零时长条目不应该
    // 在排行榜里伪装成“少于 1 分钟”。真正累计过哪怕一个采样片段后才展示。
    for (const auto& [_, record] : records_) {
        const std::uint64_t milliseconds = mode == UsageMode::Active
            ? record.activeMilliseconds : record.viewingMilliseconds;
        if (milliseconds) result.push_back(record);
    }
    std::sort(result.begin(), result.end(), [mode](const UsageRecord& left,
                                                   const UsageRecord& right) {
        const std::uint64_t leftMilliseconds = mode == UsageMode::Active
            ? left.activeMilliseconds : left.viewingMilliseconds;
        const std::uint64_t rightMilliseconds = mode == UsageMode::Active
            ? right.activeMilliseconds : right.viewingMilliseconds;
        if (leftMilliseconds != rightMilliseconds) {
            return leftMilliseconds > rightMilliseconds;
        }
        return left.displayName < right.displayName;
    });
    return result;
}

std::uint64_t UsageTracker::TodayTotalMilliseconds(UsageMode mode) const {
    std::uint64_t total{};
    for (const auto& [_, record] : records_) {
        total += mode == UsageMode::Active
            ? record.activeMilliseconds : record.viewingMilliseconds;
    }
    return total;
}

void UsageTracker::SelectCurrentForegroundApp() {
    currentKey_.clear();
    currentDisplayName_.clear();
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return;
    const DWORD processId = ForegroundProcessId(foreground);
    std::wstring path = QueryProcessPath(processId);
    if (path.empty()) return;

    std::wstring key = path;
    std::transform(key.begin(), key.end(), key.begin(), towlower);
    auto [found, inserted] = records_.try_emplace(key);
    // 切回已见过的应用直接复用显示名，不反复读取 EXE 的版本资源。
    std::wstring displayName = found->second.displayName;
    if (displayName.empty()) displayName = FileDescription(path);
    if (displayName.empty()) displayName = std::filesystem::path(path).stem().wstring();
    if (processId == GetCurrentProcessId()) displayName = L"DWD";
    if (displayName.empty()) displayName = L"未知应用";
    if (inserted) {
        found->second.path = std::move(path);
        found->second.displayName = displayName;
    } else if (found->second.displayName.empty() || processId == GetCurrentProcessId()) {
        found->second.displayName = displayName;
    }
    currentKey_ = std::move(key);
    currentDisplayName_ = std::move(displayName);
}

void UsageTracker::RollToCurrentDayIfNeeded() {
    const std::wstring current = CurrentLocalDateKey();
    if (current == todayKey_) return;
    SaveToday(true);
    records_.clear();
    currentKey_.clear();
    currentDisplayName_.clear();
    todayKey_ = current;
    dirty_ = false;
    LoadToday();
    SelectCurrentForegroundApp();
}

std::wstring UsageTracker::TodayFilePath() const {
    return (std::filesystem::path(dataDirectory_) / (todayKey_ + L".dwdusage")).wstring();
}

void UsageTracker::LoadToday() {
    records_ = LoadRecords(todayKey_);
}

std::map<std::wstring, UsageRecord> UsageTracker::LoadRecords(const std::wstring& date) const {
    std::map<std::wstring, UsageRecord> records;
    const auto path = (std::filesystem::path(dataDirectory_) / (date + L".dwdusage")).wstring();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return records;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 2 || size.QuadPart > 4 * 1024 * 1024 ||
        (size.QuadPart % sizeof(wchar_t)) != 0) {
        CloseHandle(file);
        return records;
    }
    std::vector<wchar_t> content(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)) + 1);
    DWORD read{};
    const bool loaded = ReadFile(file, content.data(), static_cast<DWORD>(size.QuadPart),
                                 &read, nullptr) != FALSE && read == size.QuadPart;
    CloseHandle(file);
    if (!loaded) return records;
    size_t offset = content[0] == 0xFEFF ? 1 : 0;
    const std::wstring text(content.data() + offset,
                            static_cast<size_t>(size.QuadPart / sizeof(wchar_t)) - offset);
    const auto lines = SplitLines(text);
    if (lines.empty() || (lines[0] != L"DWD_USAGE_V1" && lines[0] != L"DWD_USAGE_V2")) return records;
    const bool legacyV1 = lines[0] == L"DWD_USAGE_V1";
    for (size_t index = 1; index < lines.size(); ++index) {
        const size_t first = lines[index].find(L'\t');
        const size_t second = first == std::wstring::npos
            ? std::wstring::npos : lines[index].find(L'\t', first + 1);
        if (first == std::wstring::npos || second == std::wstring::npos) continue;
        const std::wstring activeText = lines[index].substr(0, first);
        wchar_t* end{};
        const std::uint64_t activeMilliseconds =
            _wcstoui64(activeText.c_str(), &end, 10);
        if (!end || *end || activeText.empty() || activeText[0] == L'-' ||
            activeMilliseconds > 86'400'000) continue;

        size_t nameStart = first + 1;
        std::uint64_t viewingMilliseconds = activeMilliseconds;
        if (!legacyV1) {
            const std::wstring viewingText = lines[index].substr(nameStart, second - nameStart);
            viewingMilliseconds = _wcstoui64(viewingText.c_str(), &end, 10);
            if (!end || *end || viewingText.empty() || viewingText[0] == L'-' ||
                viewingMilliseconds > 86'400'000) continue;
            nameStart = second + 1;
        }
        const size_t nameEnd = legacyV1 ? second : lines[index].find(L'\t', nameStart);
        if (nameEnd == std::wstring::npos) continue;

        UsageRecord record;
        record.displayName = lines[index].substr(nameStart, nameEnd - nameStart);
        record.path = lines[index].substr(nameEnd + 1);
        record.activeMilliseconds = activeMilliseconds;
        // V1 没有观看时长。用已有活跃值作为保守下限，避免升级当天切到观看模式
        // 后出现总时长倒退；从本版本启动后，两种口径会各自准确累计。
        record.viewingMilliseconds = viewingMilliseconds;
        if (record.path.empty()) continue;
        std::wstring key = record.path;
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        records[std::move(key)] = std::move(record);
    }
    return records;
}

void UsageTracker::SaveToday(bool force) {
    if (!dirty_ && !force) return;
    std::wstring content = L"DWD_USAGE_V2\r\n";
    for (const auto& [_, record] : records_) {
        if (!record.activeMilliseconds && !record.viewingMilliseconds) continue;
        content += std::to_wstring(record.activeMilliseconds) + L"\t" +
                   std::to_wstring(record.viewingMilliseconds) + L"\t" +
                   SanitizeField(record.displayName) + L"\t" +
                   SanitizeField(record.path) + L"\r\n";
    }
    store_.Submit(TodayFilePath(), std::move(content), force ? 0 : 600);
    dirty_ = false;
    lastSaveTick_ = GetTickCount64();
}
