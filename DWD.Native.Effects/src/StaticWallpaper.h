#pragma once
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "RuntimeDiagnostics.h"

// DWD 的视频窗口不会替换 Windows 的静态壁纸设置。退出时只让 Shell 重新加载
// 已经选好的图片；原图被移动/删除时，使用 Windows 自己保存的转码副本兜底。
namespace StaticWallpaper {
inline bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::wstring LegacyWallpaperPath() {
    wchar_t path[32768]{};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, static_cast<UINT>(std::size(path)), path, 0))
        return {};
    return path;
}

inline std::wstring WindowsCachedImage() {
    PWSTR roaming{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return {};
    const std::wstring path = std::wstring(roaming) + L"\\Microsoft\\Windows\\Themes\\TranscodedWallpaper";
    CoTaskMemFree(roaming);
    return FileExists(path) ? path : std::wstring{};
}

inline std::wstring RecoveryImage() {
    const std::wstring source = WindowsCachedImage();
    if (source.empty()) return {};
    HANDLE file = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BYTE signature[8]{};
    DWORD read{};
    const bool valid = ReadFile(file, signature, sizeof(signature), &read, nullptr) && read >= 3;
    CloseHandle(file);
    if (!valid) return {};
    const wchar_t* extension = nullptr;
    if (signature[0] == 0xff && signature[1] == 0xd8 && signature[2] == 0xff) extension = L".jpg";
    else if (read >= 8 && signature[0] == 0x89 && signature[1] == 'P' && signature[2] == 'N' &&
             signature[3] == 'G') extension = L".png";
    else if (signature[0] == 'B' && signature[1] == 'M') extension = L".bmp";
    if (!extension) return {};

    std::vector<wchar_t> executable(32768);
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return {};
    std::wstring folder(executable.data(), length);
    folder.resize(folder.find_last_of(L"\\/"));
    folder += L"\\Cache";
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    const std::wstring destination = folder + L"\\StaticWallpaperRecovery" + extension;
    // 这是独立的恢复副本，不会被封面/视频缓存清理功能删除。
    if (!CopyFileW(source.c_str(), destination.c_str(), FALSE) && !FileExists(destination)) return {};
    return destination;
}

inline bool Restore() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDesktopWallpaper> desktop;
    const HRESULT created = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                             IID_PPV_ARGS(&desktop));
    bool usedRecovery = false;
    std::wstring recovery;
    if (SUCCEEDED(created)) {
        DESKTOP_SLIDESHOW_STATE slideshow{};
        if (SUCCEEDED(desktop->GetStatus(&slideshow)) && (slideshow & DSS_SLIDESHOW)) {
            // 不能用 SetWallpaper 覆盖用户配置的幻灯片放映。
            const HRESULT enabled = desktop->Enable(TRUE);
            RuntimeLog(L"Static wallpaper slideshow refresh HRESULT=" + DiagnosticResult(enabled));
            return SUCCEEDED(enabled);
        }
        UINT count{};
        if (SUCCEEDED(desktop->GetMonitorDevicePathCount(&count)) && count) {
            bool allRestored = true;
            const std::wstring legacy = LegacyWallpaperPath();
            for (UINT index = 0; index < count; ++index) {
                LPWSTR id{};
                if (FAILED(desktop->GetMonitorDevicePathAt(index, &id)) || !id) {
                    allRestored = false;
                    continue;
                }
                LPWSTR current{};
                const HRESULT read = desktop->GetWallpaper(id, &current);
                std::wstring image;
                if (SUCCEEDED(read) && current && FileExists(current)) image = current;
                const bool missingOriginal = current && *current && !FileExists(current);
                if (current) CoTaskMemFree(current);
                if (image.empty() && (missingOriginal || !legacy.empty())) {
                    if (recovery.empty()) recovery = RecoveryImage();
                    image = recovery;
                    usedRecovery = !image.empty();
                }
                HRESULT restored = image.empty() && legacy.empty() && !missingOriginal
                    ? desktop->Enable(TRUE) // 用户选择纯色背景，不把旧缓存图片强加回去。
                    : (image.empty() ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                                     : desktop->SetWallpaper(id, image.c_str()));
                if (FAILED(restored) && image != recovery) {
                    if (recovery.empty()) recovery = RecoveryImage();
                    if (FileExists(recovery)) {
                        restored = desktop->SetWallpaper(id, recovery.c_str());
                        usedRecovery = SUCCEEDED(restored);
                    }
                }
                if (FAILED(restored)) {
                    RuntimeLog(L"Static wallpaper restore failed on monitor " + std::to_wstring(index + 1) +
                               L" HRESULT=" + DiagnosticResult(restored));
                    allRestored = false;
                }
                CoTaskMemFree(id);
            }
            if (allRestored) RuntimeLog(usedRecovery ? L"Static wallpaper restored from Windows cached image"
                                                    : L"Static wallpaper restored on every monitor");
            return allRestored;
        }
    }

    // 旧系统或桌面壁纸 COM 服务不可用时，退回单壁纸 Win32 API。
    const std::wstring current = LegacyWallpaperPath();
    std::wstring image = FileExists(current) ? current : std::wstring{};
    if (image.empty() && !current.empty()) { image = RecoveryImage(); usedRecovery = !image.empty(); }
    const bool restored = image.empty() ? current.empty() &&
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0,
                            SMTO_ABORTIFHUNG, 1000, nullptr) != 0
        : SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, image.data(), SPIF_SENDCHANGE) != FALSE;
    RuntimeLog(restored ? (usedRecovery ? L"Static wallpaper restored from Windows cached image (legacy API)"
                                    : L"Static wallpaper restored (legacy API)")
                        : L"Static wallpaper restore failed: no usable image or Shell wallpaper service");
    return restored;
}
}
