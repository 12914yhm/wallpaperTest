#pragma once
#include <windows.h>

namespace WallpaperHost {
inline bool Covers(const RECT& client,const RECT& desktop) {
    return client.right>client.left && client.bottom>client.top &&
        client.left<=desktop.left && client.top<=desktop.top &&
        client.right>=desktop.right && client.bottom>=desktop.bottom;
}
// Explorer 的 WorkerW 也用于许多隐藏工具窗口，不能只按类名取第一个。
inline bool Usable(HWND window, DWORD shellPid) {
    if(!window || !IsWindowVisible(window)) return false;
    DWORD pid{};GetWindowThreadProcessId(window,&pid);
    RECT client{};GetClientRect(window,&client);
    MapWindowPoints(window,HWND_DESKTOP,reinterpret_cast<POINT*>(&client),2);
    const int x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    const RECT desktop{x,y,x+GetSystemMetrics(SM_CXVIRTUALSCREEN),y+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    return pid==shellPid && Covers(client,desktop);
}

inline HWND Find() {
    HWND progman=FindWindowW(L"Progman",nullptr);
    if(!progman) return nullptr;
    DWORD shellPid{};GetWindowThreadProcessId(progman,&shellPid);
    DWORD_PTR result{};
    SendMessageTimeoutW(progman,0x052C,0,0,SMTO_ABORTIFHUNG,1000,&result);
    // 新版 Windows：静态壁纸 WorkerW 已变成 Progman 的子窗口。
    // 将视频挂到这个壁纸宿主里面，不能退回 Progman 再 HWND_BOTTOM，
    // 否则视频会被整块静态壁纸挡住。宿主自身仍在透明桌面图标层下方。
    for(HWND child=FindWindowExW(progman,nullptr,L"WorkerW",nullptr);child;
        child=FindWindowExW(progman,child,L"WorkerW",nullptr)) {
        if(Usable(child,shellPid)) return child;
    }
    // 旧版 Windows：图标宿主与视频宿主是两个相邻的顶层窗口。
    struct State {DWORD pid;HWND result{};} state{shellPid};
    EnumWindows([](HWND top,LPARAM data)->BOOL {
        auto* state=reinterpret_cast<State*>(data);
        DWORD pid{};GetWindowThreadProcessId(top,&pid);
        if(pid==state->pid && FindWindowExW(top,nullptr,L"SHELLDLL_DefView",nullptr)) {
            const HWND worker=FindWindowExW(nullptr,top,L"WorkerW",nullptr);
            if(Usable(worker,state->pid)) {state->result=worker;return FALSE;}
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&state));
    // 不再将未知的 Progman 层级当作成功，宁可保留原壁纸并报告失败。
    return state.result;
}
}
