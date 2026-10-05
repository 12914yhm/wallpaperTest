#include "UsageTracker.h"
#include "DirectoryWatcher.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

// 只测试自己创建的聚合数据，不访问或修改用户的 UsageData。
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int g_notifications{};
LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_APP + 2) { ++g_notifications; return 0; }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
void WriteFixture(const std::filesystem::path& path, const std::wstring& text) {
    std::ofstream file(path, std::ios::binary);
    const wchar_t bom = 0xFEFF;
    file.write(reinterpret_cast<const char*>(&bom), sizeof(bom));
    file.write(reinterpret_cast<const char*>(text.data()),
               static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    Require(file.good(), "fixture write failed");
}
int main() {
    const auto folder = std::filesystem::current_path() /
        (L"UsageTrackerFixture_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(folder);
    UsageTracker tracker;
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = TestWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"DWDOfflineTest";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowW(windowClass.lpszClassName, L"DWD Usage Test", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    int result{};
    try {
        WriteFixture(folder / L"2000-01-01.dwdusage",
            L"DWD_USAGE_V2\r\n2000\t3000\tApp A\tC:\\Test\\A.exe\r\n"
            L"500\t4000\tApp B\tC:\\Test\\B.exe\r\n"
            L"0\t0\tEmpty\tC:\\Test\\Empty.exe\r\n"
            L"-1\t2\tNegative\tC:\\Test\\Bad.exe\r\n"
            L"90000000\t90000000\tInvalid\tC:\\Test\\Huge.exe\r\n");
        WriteFixture(folder / L"2000-01-02.dwdusage",
            L"DWD_USAGE_V1\r\n1500\tLegacy\tC:\\Test\\Legacy.exe\r\n");
        Require(window != nullptr, "hidden window failed");
        Require(tracker.Start(window, WM_APP + 1, folder.wstring()), "hook failed");
        tracker.SetEnabled(false);
        const auto active = tracker.RecordsForDay(L"2000-01-01", UsageMode::Active);
        const auto viewing = tracker.RecordsForDay(L"2000-01-01", UsageMode::Viewing);
        Require(active.size() == 2 && viewing.size() == 2, "invalid or empty records accepted");
        Require(active[0].displayName == L"App A", "active ranking incorrect");
        Require(viewing[0].displayName == L"App B", "viewing ranking incorrect");
        Require(active[0].activeMilliseconds == 2000, "duration changed");
        const auto legacy = tracker.RecordsForDay(L"2000-01-02", UsageMode::Viewing);
        Require(legacy.size() == 1 && legacy[0].viewingMilliseconds == 1500, "V1 failed");
        Require(tracker.RecordsForDay(L"../2000-01-01", UsageMode::Active).empty(), "unsafe date accepted");
        const auto days = tracker.AvailableDays();
        Require(days.size() == 3 && days.front() == tracker.TodayKey(), "dates incorrect");
        tracker.SetSuspended(true); tracker.Tick(); tracker.SetSuspended(false);
        DirectoryWatcher watcher;
        watcher.Start(folder.wstring(), window, WM_APP + 2);
        WriteFixture(folder / L"watch.fixture", L"directory change");
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (!g_notifications && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        }
        Require(g_notifications > 0, "directory notification failed");
        watcher.Stop();
        std::cout << "UsageTracker history, modes, validation and V1 compatibility passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; result = 1;
    }
    const auto today = tracker.TodayKey();
    tracker.Stop();
    if (window) DestroyWindow(window);
    // 只删除本次创建的明确文件，不递归删除目录。
    std::error_code error;
    std::filesystem::remove(folder / L"watch.fixture", error);
    for (const auto& name : {L"2000-01-01", L"2000-01-02", today.c_str()}) {
        std::filesystem::remove(folder / (std::wstring(name) + L".dwdusage"), error);
        std::filesystem::remove(folder / (std::wstring(name) + L".dwdusage.tmp"), error);
    }
    std::filesystem::remove(folder, error);
    return result;
}
