#pragma once
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// UI 只提交不可变的 UTF-16 快照。一个写线程按路径合并更新，避免点击时等待磁盘。
// 原子替换和 FlushFileBuffers 仍保留：优化的是调用位置，不牺牲文件完整性。
class AsyncFileStore {
public:
    ~AsyncFileStore() {
        Flush();
        { std::lock_guard lock(mutex_); stopping_ = true; }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void Submit(const std::wstring& path, std::wstring text, unsigned delayMs = 600) {
        std::lock_guard lock(mutex_);
        auto& entry = pending_[path];
        entry = {std::move(text), std::chrono::steady_clock::now() +
            std::chrono::milliseconds(delayMs)};
        if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
        wake_.notify_all();
    }
    // 正常交互不调用 Flush；退出/测试时排空，不丢最后一次变更或跨日记录。
    bool Flush() {
        std::unique_lock lock(mutex_);
        flushing_ = true;
        wake_.notify_all();
        drained_.wait(lock, [this] { return pending_.empty() && !writing_; });
        flushing_ = false;
        return !failed_;
    }
    bool TakeFailure() {
        std::lock_guard lock(mutex_);
        return std::exchange(failed_, false);
    }
    static bool WriteAtomic(const std::wstring& path, const std::wstring& text) {
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        if (error) return false;
        const auto temp = path + L".tmp";
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const wchar_t bom = 0xFEFF;
        DWORD written{};
        bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
        if (ok) {
            const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
            ok = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes;
        }
        if (ok) ok = FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
        if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!ok) DeleteFileW(temp.c_str());
        return ok;
    }
private:
    struct Entry { std::wstring text; std::chrono::steady_clock::time_point due; };
    void Run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty()) return;
            auto next = pending_.begin();
            for (auto it = pending_.begin(); it != pending_.end(); ++it)
                if (it->second.due < next->second.due) next = it;
            if (!flushing_ && std::chrono::steady_clock::now() < next->second.due) {
                const auto due = next->second.due;
                wake_.wait_until(lock, due);
                continue;
            }
            auto node = pending_.extract(next);
            writing_ = true;
            lock.unlock();
            const bool ok = WriteAtomic(node.key(), node.mapped().text);
            lock.lock();
            failed_ = failed_ || !ok;
            writing_ = false;
            if (pending_.empty()) drained_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_, drained_;
    std::map<std::wstring, Entry> pending_;
    std::thread worker_;
    bool writing_{}, stopping_{}, flushing_{}, failed_{};
};
