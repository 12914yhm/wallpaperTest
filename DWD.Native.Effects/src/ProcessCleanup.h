#pragma once
#include "MemoryMetrics.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// 将旧进程的所有句柄移交给独立清理任务。UI 不等待 3/5 秒退出超时；
// 保留句柄到 waiter 结束，避免与转码完成线程发生 CloseHandle 竞争。
class ProcessCleanup {
    struct Job {
        std::mutex mutex;
        HANDLE process{}, stop{}, ready{}, pause{};
        std::wstring temporary;
        std::thread waiter, worker;
        std::atomic_bool done{};
    };
public:
    ~ProcessCleanup() { Drain(); }
    void Retire(HANDLE process, HANDLE stop, HANDLE ready = nullptr, HANDLE pause = nullptr,
                std::thread waiter = {}, std::wstring temporary = {}, DWORD timeout = 3000) {
        Reap();
        if (stop) SetEvent(stop);
        auto job = std::make_shared<Job>();
        job->process = process; job->stop = stop; job->ready = ready; job->pause = pause;
        job->waiter = std::move(waiter); job->temporary = std::move(temporary);
        job->worker = std::thread([job, timeout] {
            if (job->process && WaitForSingleObject(job->process, timeout) == WAIT_TIMEOUT) {
                TerminateProcess(job->process, ERROR_CANCELLED);
                WaitForSingleObject(job->process, 1000);
            }
            if (job->waiter.joinable()) job->waiter.join();
            {
                std::lock_guard lock(job->mutex);
                for (HANDLE handle : {job->process, job->stop, job->ready, job->pause})
                    if (handle) CloseHandle(handle);
                job->process = nullptr;
            }
            if (!job->temporary.empty()) DeleteFileW(job->temporary.c_str());
            job->done = true;
        });
        jobs_.push_back(std::move(job));
    }
    void Reap() {
        std::erase_if(jobs_, [](const auto& job) {
            if (!job->done) return false;
            job->worker.join(); return true;
        });
    }
    bool AddPrivateBytes(SIZE_T& total) {
        Reap();
        bool complete = true;
        for (const auto& job : jobs_) {
            std::lock_guard lock(job->mutex);
            SIZE_T bytes{};
            complete = PrivateResidentBytes(job->process, bytes) && complete;
            total += bytes;
        }
        return complete;
    }
    void Drain() {
        for (auto& job : jobs_) if (job->worker.joinable()) job->worker.join();
        jobs_.clear();
    }
private:
    std::vector<std::shared_ptr<Job>> jobs_;
};
