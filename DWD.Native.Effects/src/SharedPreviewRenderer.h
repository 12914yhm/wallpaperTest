#pragma once

#include <windows.h>
#include <memory>
#include <string>
#include "WallpaperEffects.h"

// 应用壁纸以后，前台不再另开一条 MFPlay 解码管线。
// 后台播放器把已经解码并缩放好的 720p BGRA 画面写入命名 D3D11 共享纹理，
// 本类只负责把这张纹理复制到预览子窗口，因此保留动态效果但几乎不增加视频解码内存。
class SharedPreviewRenderer {
public:
    SharedPreviewRenderer();
    ~SharedPreviewRenderer();

    SharedPreviewRenderer(const SharedPreviewRenderer&) = delete;
    SharedPreviewRenderer& operator=(const SharedPreviewRenderer&) = delete;

    bool Open(const std::wstring& sharedTextureName, HWND targetWindow);
    bool Render();
    void SetEffects(WallpaperEffects effects,SIZE source);
    void Stop();
    bool IsOpen() const;

private:
    struct State;
    std::unique_ptr<State> state_;
    WallpaperEffects effects_;
    SIZE source_{};
};
