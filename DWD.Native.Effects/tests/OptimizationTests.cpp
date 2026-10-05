#include "AsyncFileStore.h"
#include "MemoryMetrics.h"
#include "LibraryScanner.h"
#include "ScaledBitmapCache.h"
#include "FrameTiming.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

void Require(bool ok, const char* name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) throw std::runtime_error(name);
}
std::wstring Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size < 2) return {};
    std::wstring result(static_cast<size_t>(size) / 2, L'\0');
    file.seekg(0); file.read(reinterpret_cast<char*>(result.data()), size);
    return result;
}
int main() {
    const auto folder = std::filesystem::current_path() /
        (L"OptimizationFixture_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    try {
        {
            AsyncFileStore store;
            const auto before = GetTickCount64();
            for (int i = 0; i < 1000; ++i) store.Submit((folder / L"config.ini").wstring(), std::to_wstring(i));
            Require(GetTickCount64() - before < 500, "1000 saves enqueue without disk wait");
            store.Submit((folder / L"dayA.usage").wstring(), L"day A", 0);
            store.Submit((folder / L"dayB.usage").wstring(), L"day B", 0);
            Require(store.Flush(), "flush pending saves");
            Require(Read(folder / L"config.ini") == L"\xFEFF" L"999", "latest snapshot wins with UTF16 BOM");
            Require(Read(folder / L"dayA.usage") == L"\xFEFF" L"day A" &&
                    Read(folder / L"dayB.usage") == L"\xFEFF" L"day B", "cross-day files not coalesced together");
            Require(!std::filesystem::exists(folder / L"config.ini.tmp"), "atomic replacement removes temporary file");
            std::filesystem::create_directory(folder / L"blocked.ini");
            store.Submit((folder / L"blocked.ini").wstring(), L"fail", 0);
            Require(!store.Flush() && store.TakeFailure(), "write failures reported");
            store.Submit((folder / L"config.ini").wstring(), L"recovered", 0);
            Require(store.Flush(), "writer remains usable after failure");
        }
        {
            SIZE_T bytes{};
            Require(PrivateResidentBytes(GetCurrentProcess(), bytes) && bytes > 0, "private resident memory available");
            PROCESS_MEMORY_COUNTERS counters{};
            Require(GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != FALSE,
                    "total resident memory available");
            Require(bytes <= counters.WorkingSetSize, "private resident does not include shared pages");
            std::cout << "private=" << bytes << " total=" << counters.WorkingSetSize << '\n';
            Require(!PrivateResidentBytes(reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(0x1234)), bytes),
                    "invalid process is not reported as valid zero");
        }
        {
            std::ofstream(folder / L"video2.mp4").put('a');
            std::ofstream(folder / L"video10.mp4").put('b');
            std::ofstream(folder / L"ignore.txt").put('c');
            LibraryScanner scanner;
            const auto generation = scanner.Request(nullptr, 0, folder.wstring(), L"", false, false);
            std::unique_ptr<LibrarySnapshot> result;
            const auto until = GetTickCount64() + 5000;
            while (!(result = scanner.Take()) && GetTickCount64() < until) Sleep(5);
            Require(result && result->generation == generation && !result->error && result->entries.size() == 2,
                    "background library scan filters videos");
            Require(result->entries[0].title == L"video2", "natural ordering");
            for (int i = 0; i < 20; ++i) scanner.Request(nullptr, 0, (folder / L"missing").wstring(), L"", false, false);
            const auto latest = scanner.Request(nullptr, 0, folder.wstring(), L"", false, false);
            result.reset();
            const auto retryUntil = GetTickCount64() + 5000;
            while (!(result = scanner.Take()) && GetTickCount64() < retryUntil) Sleep(5);
            Require(result && result->generation == latest && !result->error, "obsolete scan results discarded");
        }
        {
            Require(!SkipLatePresentation(200, 33, false, 0), "never drop first frame");
            Require(!SkipLatePresentation(20, 33, true, 0), "on-time frames retained");
            Require(SkipLatePresentation(200, 33, true, 0), "late presentation skipped");
            Require(!SkipLatePresentation(200, 33, true, 5), "bounded skipping prevents starvation");
            HDC screen = GetDC(nullptr);
            HBITMAP source = CreateCompatibleBitmap(screen, 320, 180);
            ScaledBitmapCache cache;
            const RECT crop{0, 0, 320, 180};
            const auto a = cache.Get(screen, source, 160, 90, crop);
            Require(a && a == cache.Get(screen, source, 160, 90, crop), "scaled thumbnail reused");
            cache.Forget(source);
            DeleteObject(source); ReleaseDC(nullptr, screen);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(folder); return 1;
    }
    std::filesystem::remove_all(folder);
    return 0;
}
