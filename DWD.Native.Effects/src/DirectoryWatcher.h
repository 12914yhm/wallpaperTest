#pragma once
#include <windows.h>
#include <string>
#include <thread>

// 目录没有变化时线程完全等待事件，不扫描磁盘。UI 收到通知后再去抖、检查文件稳定。
// FindFirstChangeNotification 是 Windows 自带的文件系统通知，无需管理员权限。
class DirectoryWatcher {
public:
    ~DirectoryWatcher() { Stop(); }
    void Start(const std::wstring& folder, HWND owner, UINT message) {
        Stop();
        change_ = FindFirstChangeNotificationW(folder.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (change_ == INVALID_HANDLE_VALUE) { change_ = nullptr; return; }
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) { FindCloseChangeNotification(change_); change_ = nullptr; return; }
        worker_ = std::thread([this, owner, message] {
            const HANDLE events[]{stop_, change_};
            while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                PostMessageW(owner, message, 0, 0);
                if (!FindNextChangeNotification(change_)) break;
            }
        });
    }
    void Stop() {
        if (stop_) SetEvent(stop_);
        if (worker_.joinable()) worker_.join();
        if (stop_) CloseHandle(stop_);
        if (change_) FindCloseChangeNotification(change_);
        stop_ = change_ = nullptr;
    }
private:
    HANDLE stop_{};
    HANDLE change_{};
    std::thread worker_;
};
