#include <windows.h>
#include <cstdio>
#include <memory>
#include <string>
#include <filesystem>
#include "PreviewPlayer.h"

// 独立测试窗口，只操作自己的 PreviewPlayer，不操作用户应用或桌面。
// 参数是已有视频路径，不修改原视频，不进入安装包。
constexpr UINT ready=WM_APP+1,error=WM_APP+2,ended=WM_APP+3;
uint64_t readyGeneration{},errorGeneration{};
int failures{};
ULONGLONG longestSubmission{};
std::unique_ptr<PreviewPlayer> preview;
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==ready) {readyGeneration=wp;return 0;}
    if(message==error) {errorGeneration=wp;wprintf(L"Media error 0x%08lX\n",static_cast<ULONG>(lp));return 0;}
    if(message==ended) {if(preview) preview->Restart();return 0;}
    return DefWindowProcW(window,message,wp,lp);
}
void Check(bool condition,const wchar_t* label) {
    wprintf(L"%s: %s\n",condition?L"PASS":L"FAIL",label);if(!condition) ++failures;
}
void Pump(DWORD duration) {
    const auto until=GetTickCount64()+duration;
    do {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
    } while(GetTickCount64()<until);
}
bool Wait(uint64_t generation,bool expectError=false) {
    const auto start=GetTickCount64();
    while(GetTickCount64()-start<15000) {
        Pump(20);
        if(expectError && errorGeneration==generation) return true;
        if(!expectError && readyGeneration==generation) return true;
        if(!expectError && errorGeneration==generation) return false;
    }
    return false;
}
template<class Function> auto Submit(Function operation) {
    const auto start=GetTickCount64();auto result=operation();
    const auto elapsed=GetTickCount64()-start;
    if(elapsed>longestSubmission) longestSubmission=elapsed;
    Check(elapsed<100,L"UI command submission under 100ms");return result;
}
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=3 && argc!=4) {wprintf(L"Usage: DWDPreviewTests.exe videoA videoB [libraryFolder]\n");return 2;}
    WNDCLASSW type{};type.lpfnWndProc=Procedure;type.hInstance=GetModuleHandleW(nullptr);
    type.lpszClassName=L"DWDPreviewRegression";RegisterClassW(&type);
    HWND window=CreateWindowW(type.lpszClassName,L"DWD Preview Regression",WS_OVERLAPPEDWINDOW,
        50,50,680,430,nullptr,nullptr,type.hInstance,nullptr);
    HWND surface=CreateWindowW(L"STATIC",nullptr,WS_CHILD|WS_VISIBLE|SS_BLACKRECT,
        0,0,640,360,window,nullptr,type.hInstance,nullptr);
    ShowWindow(window,SW_SHOWNOACTIVATE);
    preview=std::make_unique<PreviewPlayer>(window,ready,ended,error);
    auto open=[&](const wchar_t* path) {return Submit([&]{return preview->OpenAsync(path,surface);});};
    auto command=[&](auto action) {Submit([&]{action();return 0;});};
    uint64_t generation=open(argv[1]);
    Check(Wait(generation),L"initial preview ready");
    SIZE size{};Check(preview->GetNativeVideoSize(size) && size.cx>100 && size.cy>100,L"native pixel dimensions cached (not aspect ratio)");
    command([&]{preview->Pause();});Pump(200);
    command([&]{preview->Play();});Pump(200);
    command([&]{preview->UpdateVideo();});
    // 12 次未等加载完成的快速替换，只允许最新请求接管。
    for(int index=0;index<12;++index) {open(index%2?argv[1]:argv[2]);Pump(35);}
    generation=open(argv[2]);Check(Wait(generation),L"rapid switches reach latest request");
    for(int index=0;index<4;++index) {
        command([&]{preview->Stop();});
        generation=open(index%2?argv[1]:argv[2]);
        Check(Wait(generation),L"stop followed by immediate reopen");
    }
    open(argv[1]);command([&]{preview->CancelPending();});command([&]{preview->Stop();});
    generation=open(argv[2]);Check(Wait(generation),L"cancel during loading and reopen");
    generation=open(L"D:\\__DWD_missing_regression_video__.mp4");
    Check(Wait(generation,true),L"missing media reports error");
    generation=open(argv[1]);Check(Wait(generation),L"recover after error");
    command([&]{preview->Restart();});Pump(300);
    if(argc==4) {
        int count{};std::error_code ec;
        for(const auto& entry:std::filesystem::directory_iterator(argv[3],ec)) {
            if(!entry.is_regular_file(ec) || entry.path().extension()!=L".mp4") continue;
            const auto path=entry.path().wstring();const auto loadStart=GetTickCount64();
            generation=open(path.c_str());const bool loaded=Wait(generation);
            size={};const bool dimensions=preview->GetNativeVideoSize(size);
            wprintf(L"Library #%d: ready=%d size=%ldx%ld latency=%llu ms\n",++count,
                loaded,size.cx,size.cy,GetTickCount64()-loadStart);
            Check(loaded && dimensions && size.cx>100 && size.cy>100,L"library media ready and pixel dimensions available");
            Pump(100);
        }
        Check(count>0 && !ec,L"library enumeration succeeded");
    }
    const auto start=GetTickCount64();preview.reset();
    Check(GetTickCount64()-start<2500,L"bounded shutdown");
    DestroyWindow(window);
    wprintf(L"Failures=%d; longest UI submission=%llu ms\n",failures,longestSubmission);
    return failures?1:0;
}
