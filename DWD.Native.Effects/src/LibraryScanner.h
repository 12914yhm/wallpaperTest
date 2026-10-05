#pragma once
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "ImageSource.h"

struct LibraryEntry {
    std::wstring path, title, searchTitle;
    uint64_t size{}, modified{};
};
struct LibrarySnapshot {
    unsigned generation{};
    std::wstring folder, preferred;
    bool saveSetting{}, automatic{}, writable{};
    uint64_t signature{};
    std::error_code error;
    std::vector<LibraryEntry> entries;
};

// 单个扫描线程、一个待办槽位、一个结果槽位。重复刷新合并，不创建线程风暴。
// WM_APP 仅作通知，结果由本对象持有，窗口关闭后不会遗留堆指针消息。
class LibraryScanner {
public:
    ~LibraryScanner() { Stop(); }
    unsigned Request(HWND owner, UINT message, const std::wstring& folder,
                     const std::wstring& preferred, bool saveSetting, bool automatic) {
        std::lock_guard lock(mutex_);
        LibrarySnapshot request;
        request.generation = ++generation_; request.folder = folder; request.preferred = preferred;
        request.saveSetting = saveSetting; request.automatic = automatic;
        request_ = std::move(request); result_.reset(); owner_ = owner; message_ = message;
        if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
        wake_.notify_one();
        return generation_;
    }
    std::unique_ptr<LibrarySnapshot> Take() {
        std::lock_guard lock(mutex_); return std::move(result_);
    }
    void Stop() {
        { std::lock_guard lock(mutex_); stopping_ = true; ++generation_; request_.reset(); }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        result_.reset();
    }
    static bool IsVideo(const std::filesystem::path& path) {
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        return extension == L".mp4" || extension == L".mov" || extension == L".m4v" ||
            extension == L".wmv" || extension == L".avi" || extension == L".mkv" || IsWallpaperImage(path.wstring());
    }
private:
    void Scan(LibrarySnapshot& snapshot) {
        std::error_code error;
        std::filesystem::directory_iterator it(snapshot.folder,
            std::filesystem::directory_options::skip_permission_denied, error), end;
        if (error) { snapshot.error = error; return; }
        for (; it != end && !error; it.increment(error)) {
            if (snapshot.generation != generation_) return;
            const auto& file = *it;
            std::error_code entryError;
            if (!IsVideo(file.path()) || !file.is_regular_file(entryError)) continue;
            LibraryEntry entry;
            entry.path = file.path().wstring(); entry.title = file.path().stem().wstring();
            entry.searchTitle = entry.title;
            std::transform(entry.searchTitle.begin(), entry.searchTitle.end(), entry.searchTitle.begin(), towlower);
            entry.size = file.file_size(entryError);
            if (entryError) continue;
            const auto modified = file.last_write_time(entryError);
            if (entryError) continue;
            entry.modified = static_cast<uint64_t>(modified.time_since_epoch().count());
            uint64_t hash = 1469598103934665603ULL;
            for (wchar_t c : entry.path) { hash ^= c; hash *= 1099511628211ULL; }
            snapshot.signature ^= hash ^ (entry.size * 1099511628211ULL) ^ entry.modified;
            if (snapshot.automatic) {
                HANDLE probe = CreateFileW(entry.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (probe == INVALID_HANDLE_VALUE) {
                    // 永久权限不足不应触发无限刷新；只等真正的复制/写入占用。
                    const DWORD code = GetLastError();
                    snapshot.writable |= code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
                } else CloseHandle(probe);
            }
            snapshot.entries.push_back(std::move(entry));
            if (snapshot.entries.size() >= 10000) break;
        }
        snapshot.error = error;
        std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const auto& a, const auto& b) {
            return StrCmpLogicalW(a.title.c_str(), b.title.c_str()) < 0;
        });
    }
    void Run() {
        for (;;) {
            std::unique_ptr<LibrarySnapshot> snapshot;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || request_.has_value(); });
                if (stopping_) return;
                snapshot = std::make_unique<LibrarySnapshot>(std::move(*request_)); request_.reset();
            }
            Scan(*snapshot);
            std::lock_guard lock(mutex_);
            if (stopping_ || snapshot->generation != generation_) continue;
            result_ = std::move(snapshot);
            PostMessageW(owner_, message_, 0, 0);
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<LibrarySnapshot> request_;
    std::unique_ptr<LibrarySnapshot> result_;
    std::thread worker_;
    std::atomic_uint generation_{};
    HWND owner_{};
    UINT message_{};
    bool stopping_{};
};
