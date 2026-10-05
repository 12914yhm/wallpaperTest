#include <windows.h>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "WallpaperHost.h"

// 手动集成测试：只读窗口结构和少量桌面像素，不注入鼠标/键盘，不修改系统设置。
// 用真实桌面画面验证“播放器就绪”不是被静态壁纸遮住的假成功。
// 不保存屏幕图像；输出仅有数量、尺寸与变化像素数。发布目录不包含这个工具。
struct Sample {POINT point;COLORREF color;};
struct Screen {RECT rect;std::vector<Sample> samples;};

bool OnDesktop(POINT point) {
    HWND window=WindowFromPoint(point);
    window=GetAncestor(window,GA_ROOT);
    wchar_t cls[128]{};GetClassNameW(window,cls,128);
    return wcscmp(cls,L"Progman")==0 || wcscmp(cls,L"WorkerW")==0;
}

int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::vector<Screen> screens;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT rect,LPARAM data)->BOOL {
        reinterpret_cast<std::vector<Screen>*>(data)->push_back({*rect,{}});return TRUE;
    },reinterpret_cast<LPARAM>(&screens));
    struct Outputs {int count{},invalid{};} outputs;
    EnumWindows([](HWND root,LPARAM data)->BOOL {
        wchar_t cls[128]{};GetClassNameW(root,cls,128);
        if(wcscmp(cls,L"Progman")!=0 && wcscmp(cls,L"WorkerW")!=0) return TRUE;
        EnumChildWindows(root,[](HWND window,LPARAM data)->BOOL {
            wchar_t cls[128]{};GetClassNameW(window,cls,128);
            if(wcscmp(cls,L"DWDNativeWallpaper")!=0) return TRUE;
            auto* outputs=reinterpret_cast<Outputs*>(data);++outputs->count;
            HWND parent=GetParent(window);GetClassNameW(parent,cls,128);
            RECT bounds{};GetWindowRect(window,&bounds);
            MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info);
            const bool valid=wcscmp(cls,L"WorkerW")==0 && IsWindowVisible(window) && EqualRect(&bounds,&info.rcMonitor);
            if(!valid) ++outputs->invalid;
            wprintf(L"Output %d: %ldx%ld parent=%s visible=%d geometry=%s\n",outputs->count,
                bounds.right-bounds.left,bounds.bottom-bounds.top,cls,IsWindowVisible(window),valid?L"OK":L"FAIL");
            return TRUE;
        },data);
        return TRUE;
    },reinterpret_cast<LPARAM>(&outputs));
    HDC desktop=GetDC(nullptr);
    for(auto& screen:screens) {
        for(int y=1;y<8;++y) for(int x=1;x<10;++x) {
            POINT point{screen.rect.left+(screen.rect.right-screen.rect.left)*x/10,
                screen.rect.top+(screen.rect.bottom-screen.rect.top)*y/8};
            if(OnDesktop(point)) screen.samples.push_back({point,GetPixel(desktop,point.x,point.y)});
        }
    }
    // 一秒差值，排除有前台应用覆盖、鼠标/悬浮球经过的采样点。
    Sleep(1000);
    int movingScreens{};
    int exposedScreens{},nonblackScreens{};
    for(size_t index=0;index<screens.size();++index) {
        int valid{},changed{},nonblack{};
        for(const auto& sample:screens[index].samples) {
            if(!OnDesktop(sample.point)) continue;
            const COLORREF next=GetPixel(desktop,sample.point.x,sample.point.y);
            if(next==CLR_INVALID || sample.color==CLR_INVALID) continue;
            ++valid;if(next!=sample.color) ++changed;
            if(GetRValue(next)+GetGValue(next)+GetBValue(next)>15) ++nonblack;
        }
        const auto& rect=screens[index].rect;
        wprintf(L"Screen %zu (%ld,%ld %ldx%ld): exposed=%d moving=%d nonblack=%d\n",index+1,
            rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,valid,changed,nonblack);
        if(changed>3 && nonblack>3) ++movingScreens;
        if(valid>3) ++exposedScreens;
        if(nonblack>3) ++nonblackScreens;
    }
    ReleaseDC(nullptr,desktop);
    wprintf(L"Outputs=%d invalid=%d visibleMovingScreens=%d\n",outputs.count,outputs.invalid,movingScreens);
    if(argc>1 && wcscmp(argv[1],L"--static")==0) return outputs.count==0 && movingScreens==0?0:1;
    if(argc>1 && wcscmp(argv[1],L"--paused")==0)
        return outputs.count>0 && outputs.invalid==0 && exposedScreens>0 &&
            movingScreens==0 && nonblackScreens==exposedScreens?0:1;
    return outputs.count>0 && outputs.invalid==0 && movingScreens>0?0:1;
}
