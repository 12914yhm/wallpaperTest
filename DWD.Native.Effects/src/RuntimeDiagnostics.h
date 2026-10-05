#pragma once
#include <windows.h>
#include <string>

// 只记录启动、挂载、首帧与错误，不逐帧写盘；最多保留 1MB 本地诊断。
// 日志位于程序目录，不访问网络，也不修改系统显示设置。
inline void RuntimeLog(const std::wstring& message) {
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    std::wstring file(path);
    file.resize(file.find_last_of(L"\\/") + 1);
    file += L"DWDNative.log";
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};GetFileSizeEx(handle,&size);
    if(size.QuadPart > 1024*1024) {SetFilePointer(handle,0,nullptr,FILE_BEGIN);SetEndOfFile(handle);}
    SetFilePointer(handle,0,nullptr,FILE_END);
    SYSTEMTIME now{};GetLocalTime(&now);
    wchar_t prefix[96]{};
    swprintf_s(prefix,L"%04u-%02u-%02u %02u:%02u:%02u pid=%lu ",now.wYear,now.wMonth,
        now.wDay,now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId());
    const std::wstring line=std::wstring(prefix)+message+L"\r\n";
    const int length=WideCharToMultiByte(CP_UTF8,0,line.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string bytes(length,'\0');WideCharToMultiByte(CP_UTF8,0,line.c_str(),-1,bytes.data(),length,nullptr,nullptr);
    DWORD written{};WriteFile(handle,bytes.data(),length-1,&written,nullptr);CloseHandle(handle);
}

inline std::wstring WindowDiagnostic(HWND window) {
    wchar_t cls[128]{};GetClassNameW(window,cls,128);
    RECT rect{},client{};GetWindowRect(window,&rect);GetClientRect(window,&client);
    wchar_t text[384]{};
    swprintf_s(text,L"hwnd=%p %s parent=%p visible=%d rect=%ld,%ld,%ld,%ld client=%ldx%ld ex=%08llX",
        window,cls,GetParent(window),IsWindowVisible(window),rect.left,rect.top,rect.right,rect.bottom,
        client.right,client.bottom,static_cast<unsigned long long>(GetWindowLongPtrW(window,GWL_EXSTYLE)));
    return text;
}

inline void LogDesktopTree() {
    EnumWindows([](HWND window,LPARAM)->BOOL {
        wchar_t cls[128]{};GetClassNameW(window,cls,128);
        if(wcscmp(cls,L"Progman")==0 || wcscmp(cls,L"WorkerW")==0) {
            RuntimeLog(L"Desktop "+WindowDiagnostic(window));
            EnumChildWindows(window,[](HWND child,LPARAM)->BOOL {
                wchar_t name[128]{};GetClassNameW(child,name,128);
                if(wcscmp(name,L"WorkerW")==0 || wcscmp(name,L"SHELLDLL_DefView")==0 ||
                   wcscmp(name,L"DWDNativeWallpaper")==0) RuntimeLog(L"  child "+WindowDiagnostic(child));
                return TRUE;
            },0);
        }
        return TRUE;
    },0);
}

inline constexpr wchar_t kDevelopmentExitEvent[]=L"Local\\DWDNative_DevelopmentExit";

inline std::wstring DiagnosticResult(HRESULT result) {
    wchar_t value[16]{};swprintf_s(value,L"0x%08lX",static_cast<unsigned long>(result));return value;
}
