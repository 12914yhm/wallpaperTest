#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <functional>

// UI 只提交命令，不调用可能等待解码器/驱动的 MFPlay API。
// 媒体线程串行管理播放器；回调只投递事件，旧代次不能覆盖新选择。
class PreviewPlayer {
public:
    PreviewPlayer(HWND notifyWindow, UINT readyMessage, UINT endedMessage, UINT errorMessage);
    ~PreviewPlayer();
    PreviewPlayer(const PreviewPlayer&) = delete;
    PreviewPlayer& operator=(const PreviewPlayer&) = delete;
    // prepare 仅在媒体线程执行，必须只捕获值，不能引用 UI 对象。
    uint64_t OpenAsync(const std::wstring& path, HWND videoWindow,
                       std::function<std::wstring()> prepare = {});
    void CancelPending();
    void Play();
    void Pause();
    void Restart();
    void UpdateVideo();
    void Seek(int milliseconds);
    void PollPosition();
    int PositionMs() const;
    int DurationMs() const;
    bool GetNativeVideoSize(SIZE& size) const;
    // 异步释放，收回托盘无需等 Shutdown 返回。
    void Stop();
private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread worker_;
};
