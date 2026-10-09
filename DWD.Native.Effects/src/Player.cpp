#include <windows.h>
#include <wtsapi32.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "RuntimeDiagnostics.h"
#include "WallpaperHost.h"
#include "FrameTiming.h"
#include "EffectsRenderer.h"
#include "ImageSource.h"
#include "StaticWallpaper.h"

// 本文件实现 DWDNative.exe 的“后台播放器模式”。总体数据流：
//
// MP4 文件 -> Media Foundation Source Reader（硬件解码为 NV12 D3D11 纹理）
//          -> D3D11 Video Processor（按每块屏幕独立裁剪、缩放）
//          -> 每块屏幕自己的 DXGI SwapChain -> WorkerW 桌面窗口
//
// 关键点是只有一个 Source Reader 和一个解码线程。双屏增加的是输出交换链，
// 而不是再解码一遍视频，因此内存和 GPU 解码占用明显低于“双播放器”方案。
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kWallpaperClass[] = L"DWDNativeWallpaper";
constexpr UINT kReadyMessage = WM_APP + 1;
constexpr UINT kDecodeFailed = WM_APP + 2;
constexpr UINT kWallpaperErrorMessage = WM_APP + 21;
int g_fullscreenRule{};
HWND g_previewParent{};
EffectChannel g_effectChannel;
std::wstring g_requestedMonitor;
std::map<std::wstring,std::pair<double,double>> g_screenFocus;
constexpr UINT kSharedPreviewFrameMessage = WM_APP + 16;
constexpr UINT_PTR kEnergyTimer = 1;
// 短暂 Alt+Tab 或切入全屏不值得重建解码器；连续 30 秒不可见后再释放重资源。
constexpr ULONGLONG kDecoderReleaseDelayMs = 30'000;
constexpr ULONGLONG kDisplayRebuildAfterChangeMs = 1'500;

std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    }
    return {};
}

HWND FindWallpaperParent() {
    return WallpaperHost::Find();
}

std::vector<RECT> MonitorRects() {
    std::vector<RECT> monitors;
    RECT primary{};
    std::pair<std::vector<RECT>*,RECT*> output{&monitors,&primary};
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
        auto* output=reinterpret_cast<std::pair<std::vector<RECT>*,RECT*>*>(data);
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(GetMonitorInfoW(monitor,&info)) {
            if(info.dwFlags&MONITORINFOF_PRIMARY) *output->second=info.rcMonitor;
            if(g_requestedMonitor.empty() || g_requestedMonitor==info.szDevice)
                output->first->push_back(info.rcMonitor);
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&output));
    if(monitors.empty() && primary.right>primary.left) monitors.push_back(primary);
    return monitors;
}

bool IsFullscreenForeground(int rule=g_fullscreenRule) {
    // 每秒检查一次前台窗口是否覆盖它所在的整个显示器。全屏游戏或视频出现时
    // 暂停后台解码，既省电，也避免与前台程序争用硬件视频解码器。
    if(rule==2) return false;
    HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindowVisible(foreground) || IsIconic(foreground)) return false;
    wchar_t className[128]{};
    GetClassNameW(foreground, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Progman") == 0 || _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Shell_TrayWnd") == 0 || _wcsicmp(className, kWallpaperClass) == 0) {
        return false;
    }
    RECT windowRect{};
    GetWindowRect(foreground, &windowRect);
    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    constexpr int tolerance = 2;
    if(rule==1) {
        for(const RECT& screen : MonitorRects()) {
            struct State {RECT screen;bool fullscreen{};} state{screen};
            EnumWindows([](HWND window,LPARAM data)->BOOL {
                auto* state=reinterpret_cast<State*>(data);
                if(!IsWindowVisible(window) || IsIconic(window) ||
                   (GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)) return TRUE;
                wchar_t cls[128]{};GetClassNameW(window,cls,128);
                if(_wcsicmp(cls,L"Progman")==0 || _wcsicmp(cls,L"WorkerW")==0 ||
                   _wcsicmp(cls,L"Shell_TrayWnd")==0 || _wcsicmp(cls,L"Shell_SecondaryTrayWnd")==0 ||
                   _wcsicmp(cls,kWallpaperClass)==0) return TRUE;
                RECT bounds{},intersection{};GetWindowRect(window,&bounds);
                if(!IntersectRect(&intersection,&bounds,&state->screen)) return TRUE;
                const auto area=static_cast<int64_t>(intersection.right-intersection.left)*(intersection.bottom-intersection.top);
                const auto screenArea=static_cast<int64_t>(state->screen.right-state->screen.left)*(state->screen.bottom-state->screen.top);
                if(area<screenArea/4) return TRUE;
                state->fullscreen=bounds.left<=state->screen.left+2 && bounds.top<=state->screen.top+2 &&
                    bounds.right>=state->screen.right-2 && bounds.bottom>=state->screen.bottom-2;
                return FALSE;
            },reinterpret_cast<LPARAM>(&state));
            if(!state.fullscreen) return false;
        }
        return true;
    }
    return windowRect.left <= info.rcMonitor.left + tolerance &&
           windowRect.top <= info.rcMonitor.top + tolerance &&
           windowRect.right >= info.rcMonitor.right - tolerance &&
           windowRect.bottom >= info.rcMonitor.bottom - tolerance;
}

struct RenderTarget {
    // 一块显示器对应一个壁纸 HWND 和一个输出 SwapChain。
    // enumerator/processor/outputView 都与该显示器的输出尺寸绑定。
    HWND window{};
    UINT width{};
    UINT height{};
    double focusX{0.5},focusY{0.5};
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    ComPtr<ID3D11Texture2D> backBuffer;
    EffectsRenderer effects;
};

struct SharedPreviewTarget {
    static constexpr UINT width = 1280;
    static constexpr UINT height = 720;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> keyedMutex;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    HANDLE sharedHandle{};
};

class NativePlayer {
public:
    HRESULT Open(const std::wstring& path, const std::vector<HWND>& windows,
                 double focusX, double focusY, const std::wstring& previewTextureName,
                 HWND previewNotifyWindow, HWND readyNotifyWindow) {
        // 焦点是 0~1 的归一化坐标，因此不依赖视频具体分辨率。
        focusX_ = std::clamp(focusX, 0.0, 1.0);
        focusY_ = std::clamp(focusY, 0.0, 1.0);
        previewNotifyWindow_ = previewNotifyWindow;
        readyNotifyWindow_ = readyNotifyWindow;

        // VIDEO_SUPPORT 让 D3D11 设备暴露视频处理接口；BGRA_SUPPORT 是交换链输出需要。
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selected{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                       &device_, &selected, &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                                   &device_, &selected, &context_);
        }
        if (FAILED(hr)) return hr;

        // 视频设备/上下文属于整个播放器生命周期，缓存一次即可。
        // RenderFrame 每帧重新 QueryInterface 虽不会泄漏，但会产生不必要的 COM 开销。
        hr = device_.As(&videoDevice_);
        if (SUCCEEDED(hr)) hr = context_.As(&videoContext_);
        if (FAILED(hr)) return hr;

        ComPtr<IDXGIDevice1> dxgiDevice;
        if (SUCCEEDED(device_.As(&dxgiDevice))) dxgiDevice->SetMaximumFrameLatency(1);

        UINT resetToken{};
        // 把 D3D11 设备交给 Media Foundation。硬件解码器由此可以直接输出 GPU 纹理，
        // 不必先复制到 CPU 内存，再上传回 GPU。
        hr = MFCreateDXGIDeviceManager(&resetToken, &deviceManager_);
        if (SUCCEEDED(hr)) hr = deviceManager_->ResetDevice(device_.Get(), resetToken);
        path_ = path;
        resumeTimestamp_ = 0;
        lastTimestamp_ = 0;
        if (SUCCEEDED(hr)) hr = CreateDecoder();
        if (FAILED(hr)) return hr;

        hr = CreateRenderTargets(windows);
        if (FAILED(hr)) return hr;
        if (!previewTextureName.empty()) {
            // 共享预览失败不影响桌面壁纸本身；控制器会自动回退到 MFPlay 预览。
            CreateSharedPreviewTarget(previewTextureName);
        }
        windows_ = windows;
        firstFrameRendered_ = false;
        StartDecodeWorker();
        return S_OK;
    }

    void Pause() {
        { std::lock_guard lock(playbackMutex_); if (decoderLoaded_) paused_ = true; }
        playbackWake_.notify_all();
    }

    void Resume() {
        // 暂停期间真实时间仍在前进；恢复时必须重置时间基准，否则会瞬间追帧。
        {
            std::lock_guard lock(playbackMutex_);
            if (paused_) { timelineReset_ = true; paused_ = false; }
        }
        playbackWake_.notify_all();
    }

    HRESULT ReleaseDecoder() {
        if (!decoderLoaded_) return S_FALSE;
        StopDecodeWorker();
        resumeTimestamp_ = lastTimestamp_.load();
        // 先隐藏壁纸窗口，让 Windows 原来的静态壁纸接管；随后才能安全释放双屏
        // 交换链。共享预览纹理仍保留，因此控制器不会失去跨进程资源句柄。
        for (HWND window : windows_) ShowWindow(window, SW_HIDE);
        targets_.clear();
        outputsReleased_ = true;
        reader_.Reset();
        imageSample_.Reset();imageTexture_.Reset();
        decoderLoaded_ = false;
        context_->ClearState();
        context_->Flush();
        return S_OK;
    }

    void TrimReleasedMemory() {
        // 通知 DXGI 当前进入长时不可见状态，显卡驱动可以归还已释放资源的驻留内存。
        ComPtr<IDXGIDevice3> dxgiDevice;
        if (SUCCEEDED(device_.As(&dxgiDevice))) dxgiDevice->Trim();
        // Media Foundation/显卡驱动会把已经不用的页暂留在工作集中以便复用。
        // 深度休眠明确以恢复延迟换内存，因此请求 Windows 立即回收这些物理页。
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                 static_cast<SIZE_T>(-1));
    }

    HRESULT RestoreDecoder(const std::vector<HWND>& windows) {
        if (decoderLoaded_) return S_FALSE;
        HRESULT hr = CreateDecoder();
        if (FAILED(hr)) return hr;
        if (outputsReleased_) {
            windows_ = windows;
            hr = CreateRenderTargets(windows_);
            if (FAILED(hr)) {
                targets_.clear();
                reader_.Reset();
                decoderLoaded_ = false;
                return hr;
            }
            outputsReleased_ = false;
            // 首帧真正提交后，DecodeLoop 才通过 kReadyMessage 重新显示壁纸窗口。
            firstFrameRendered_ = false;
        }
        sharedPreviewFrameCounter_ = 0;
        timelineReset_ = true;
        StartDecodeWorker();
        return S_OK;
    }

    void ForgetWindows() { windows_.clear(); }

    void Shutdown() {
        ReleaseDecoder();
        targets_.clear();
        ResetSharedPreview();
        deviceManager_.Reset();
        videoContext_.Reset();
        videoDevice_.Reset();
        context_.Reset();
        device_.Reset();
    }

private:
    HRESULT CreateDecoder() {
        if (path_.empty() || !deviceManager_) return E_UNEXPECTED;
        if(IsWallpaperImage(path_)) {
            HRESULT hr=LoadImageTexture(device_.Get(),path_,&imageTexture_,videoWidth_,videoHeight_);
            if(FAILED(hr))return hr;
            ComPtr<IMFMediaBuffer> buffer;
            hr=MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),imageTexture_.Get(),0,FALSE,&buffer);
            if(SUCCEEDED(hr))hr=MFCreateSample(&imageSample_);
            if(SUCCEEDED(hr))hr=imageSample_->AddBuffer(buffer.Get());
            frameRateNumerator_=30;frameRateDenominator_=1;
            decoderLoaded_=SUCCEEDED(hr);return hr;
        }

        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 5);
        if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,
                                                        deviceManager_.Get());
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                                                       TRUE);
        // 缩放和颜色转换交给 D3D11 Video Processor，Source Reader 不重复处理。
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                                                       FALSE);
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

        ComPtr<IMFSourceReader> nextReader;
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path_.c_str(), attributes.Get(),
                                                             &nextReader);
        if (FAILED(hr)) return hr;

        // 动态壁纸不播放声音，所以关闭所有流后只选择第一条视频流。
        nextReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        hr = nextReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                             TRUE);

        ComPtr<IMFMediaType> outputType;
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&outputType);
        if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        // NV12 是多数 Windows 硬件解码器的原生输出格式，体积也小于 BGRA。
        if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (SUCCEEDED(hr)) hr = nextReader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.Get());

        ComPtr<IMFMediaType> currentType;
        if (SUCCEEDED(hr)) hr = nextReader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &currentType);
        if (SUCCEEDED(hr)) hr = MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE,
                                                    &videoWidth_, &videoHeight_);
        if (SUCCEEDED(hr) && FAILED(MFGetAttributeRatio(currentType.Get(), MF_MT_FRAME_RATE,
                                                        &frameRateNumerator_,
                                                        &frameRateDenominator_))) {
            frameRateNumerator_ = 30;
            frameRateDenominator_ = 1;
        }
        if (SUCCEEDED(hr) && FAILED(MFGetAttributeRatio(currentType.Get(), MF_MT_PIXEL_ASPECT_RATIO,
                                                        &pixelAspectNumerator_,
                                                        &pixelAspectDenominator_))) {
            pixelAspectNumerator_ = 1;
            pixelAspectDenominator_ = 1;
        }
        if (FAILED(hr)) return hr;

        if (resumeTimestamp_ > 0) {
            // 从暂停前的位置附近恢复；压缩视频会由 MF 自动回退到最近关键帧。
            PROPVARIANT position;
            PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = resumeTimestamp_;
            nextReader->SetCurrentPosition(GUID_NULL, position);
            PropVariantClear(&position);
        }

        reader_ = std::move(nextReader);
        if(auto* data=g_effectChannel.Data()) {
            PROPVARIANT duration;PropVariantInit(&duration);
            if(SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&duration)) && duration.vt==VT_UI8)
                InterlockedExchange(&data->durationMs,static_cast<LONG>((std::min)(duration.uhVal.QuadPart/10000,ULONGLONG(LONG_MAX))));
            PropVariantClear(&duration);
        }
        decoderLoaded_ = true;
        return S_OK;
    }

    void StartDecodeWorker() {
        paused_ = false;
        running_ = true;
        worker_ = std::thread([this] { DecodeLoop(); });
    }

    void StopDecodeWorker() {
        { std::lock_guard lock(playbackMutex_); running_ = false; paused_ = false; }
        playbackWake_.notify_all();
        if (reader_) reader_->Flush(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
        if (worker_.joinable()) worker_.join();
    }

    void ResetSharedPreview() {
        if (sharedPreview_.sharedHandle) CloseHandle(sharedPreview_.sharedHandle);
        sharedPreview_ = {};
    }

    HRESULT CreateSharedPreviewTarget(const std::wstring& name) {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = SharedPreviewTarget::width;
        textureDesc.Height = SharedPreviewTarget::height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                                D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        HRESULT hr = device_->CreateTexture2D(&textureDesc, nullptr, &sharedPreview_.texture);
        if (SUCCEEDED(hr)) hr = sharedPreview_.texture.As(&sharedPreview_.keyedMutex);

        ComPtr<IDXGIResource1> resource;
        if (SUCCEEDED(hr)) hr = sharedPreview_.texture.As(&resource);
        if (SUCCEEDED(hr)) {
            hr = resource->CreateSharedHandle(nullptr,
                                              DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                              name.c_str(), &sharedPreview_.sharedHandle);
        }
        // 命名 NT 句柄必须保持打开；提前 CloseHandle 会让控制器无法按名称找到资源。

        if (SUCCEEDED(hr) && targets_.empty()) hr = E_UNEXPECTED;
        if (SUCCEEDED(hr)) {
            // 共享预览复用第一块显示器已有的 Video Processor。另建一套处理器在部分
            // 显卡驱动上会额外分配约 70~90MB 的内部表面，实际并没有必要。
            sharedPreview_.enumerator = targets_.front().enumerator;
            sharedPreview_.processor = targets_.front().processor;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputDesc.Texture2D.MipSlice = 0;
        if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessorOutputView(
            sharedPreview_.texture.Get(), sharedPreview_.enumerator.Get(),
            &outputDesc, &sharedPreview_.outputView);
        if (FAILED(hr)) ResetSharedPreview();
        return hr;
    }

    HRESULT CreateRenderTargets(const std::vector<HWND>& windows) {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        HRESULT hr = device_.As(&dxgiDevice);
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;

        if (!videoDevice_ || !videoContext_) return E_UNEXPECTED;

        for (HWND window : windows) {
            RECT rect{};
            GetClientRect(window, &rect);
            RenderTarget target;
            target.window = window;
            target.focusX=focusX_;target.focusY=focusY_;
            MONITORINFOEXW info{};info.cbSize=sizeof(info);
            if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info)) {
                const auto found=g_screenFocus.find(info.szDevice);
                if(found!=g_screenFocus.end()) {target.focusX=found->second.first;target.focusY=found->second.second;}
            }
            target.width = static_cast<UINT>(rect.right - rect.left);
            target.height = static_cast<UINT>(rect.bottom - rect.top);

            DXGI_SWAP_CHAIN_DESC1 swapDesc{};
            swapDesc.Width = target.width;
            swapDesc.Height = target.height;
            swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            swapDesc.SampleDesc.Count = 1;
            swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            // Flip 模型双缓冲是现代 Windows 推荐路径，Present 时复制更少。
            swapDesc.BufferCount = 2;
            swapDesc.Scaling = DXGI_SCALING_STRETCH;
            swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            hr = factory->CreateSwapChainForHwnd(device_.Get(), window, &swapDesc, nullptr, nullptr,
                                                  &target.swapChain);
            if (FAILED(hr)) return hr;

            ComPtr<ID3D11Texture2D> backBuffer;
            hr = target.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
            if (FAILED(hr)) return hr;
            target.backBuffer=backBuffer;

            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputFrameRate.Numerator = frameRateNumerator_;
            content.InputFrameRate.Denominator = frameRateDenominator_;
            content.InputWidth = videoWidth_;
            content.InputHeight = videoHeight_;
            content.OutputFrameRate = content.InputFrameRate;
            content.OutputWidth = target.width;
            content.OutputHeight = target.height;
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            hr = videoDevice_->CreateVideoProcessorEnumerator(&content, &target.enumerator);
            if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessor(target.enumerator.Get(), 0,
                                                                        &target.processor);

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outputDesc.Texture2D.MipSlice = 0;
            if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessorOutputView(backBuffer.Get(),
                                                                                  target.enumerator.Get(),
                                                                                  &outputDesc,
                                                                                  &target.outputView);
            if (FAILED(hr)) return hr;
            targets_.push_back(std::move(target));
        }
        return targets_.empty() ? E_FAIL : S_OK;
    }

    RECT SourceCrop(UINT targetWidth, UINT targetHeight,double focusX,double focusY) const {
        // “铺满”算法：源比目标宽就裁左右，源比目标高就裁上下。
        // 裁剪框围绕用户选择的 focusX/focusY，再限制在视频边界内。
        const double sourceAspect = (static_cast<double>(videoWidth_) * pixelAspectNumerator_) /
                                    (static_cast<double>(videoHeight_) * pixelAspectDenominator_);
        const double targetAspect = static_cast<double>(targetWidth) / targetHeight;
        RECT source{0, 0, static_cast<LONG>(videoWidth_), static_cast<LONG>(videoHeight_)};
        if (sourceAspect > targetAspect) {
            const LONG visibleWidth = static_cast<LONG>(videoHeight_ * targetAspect *
                                                        pixelAspectDenominator_ / pixelAspectNumerator_);
            const LONG maximumLeft = static_cast<LONG>(videoWidth_) - visibleWidth;
            source.left = std::clamp(static_cast<LONG>(focusX * videoWidth_ - visibleWidth / 2.0),
                                     0L, maximumLeft);
            source.right = source.left + visibleWidth;
        } else if (sourceAspect < targetAspect) {
            const LONG visibleHeight = static_cast<LONG>(videoWidth_ * pixelAspectNumerator_ /
                                                         (targetAspect * pixelAspectDenominator_));
            const LONG maximumTop = static_cast<LONG>(videoHeight_) - visibleHeight;
            source.top = std::clamp(static_cast<LONG>(focusY * videoHeight_ - visibleHeight / 2.0),
                                    0L, maximumTop);
            source.bottom = source.top + visibleHeight;
        }
        // NV12 的色度平面按 2x2 像素采样，裁剪边界必须是偶数，否则部分驱动会失败。
        source.left &= ~1;
        source.top &= ~1;
        source.right &= ~1;
        source.bottom &= ~1;
        return source;
    }

    RECT PreviewDestination(UINT targetWidth, UINT targetHeight) const {
        // 控制窗口要看到完整原视频，便于用户拖动取景框；与桌面“铺满裁剪”不同，
        // 共享预览使用 contain，比例不同时由 Video Processor 留黑边。
        const double sourceAspect = (static_cast<double>(videoWidth_) * pixelAspectNumerator_) /
                                    (static_cast<double>(videoHeight_) * pixelAspectDenominator_);
        const double targetAspect = static_cast<double>(targetWidth) / targetHeight;
        RECT destination{0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
        if (sourceAspect > targetAspect) {
            const LONG height = static_cast<LONG>(targetWidth / sourceAspect + 0.5);
            destination.top = (static_cast<LONG>(targetHeight) - height) / 2;
            destination.bottom = destination.top + height;
        } else if (sourceAspect < targetAspect) {
            const LONG width = static_cast<LONG>(targetHeight * sourceAspect + 0.5);
            destination.left = (static_cast<LONG>(targetWidth) - width) / 2;
            destination.right = destination.left + width;
        }
        return destination;
    }

    HRESULT RenderFrame(IMFSample* sample) {
        // 在硬件解码路径中，sample 的底层缓冲实现了 IMFDXGIBuffer。
        // GetResource 取得的是同一张 GPU 纹理，不会复制整帧像素。
        ComPtr<IMFMediaBuffer> buffer;
        DWORD bufferCount{};
        HRESULT hr = sample->GetBufferCount(&bufferCount);
        // 硬件解码通常只有一个 DXGI 缓冲，直接取得它，避免让 MF 检查并拼接连续缓冲。
        if (SUCCEEDED(hr) && bufferCount == 1) {
            hr = sample->GetBufferByIndex(0, &buffer);
        } else if (SUCCEEDED(hr)) {
            hr = sample->ConvertToContiguousBuffer(&buffer);
        }
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        if (SUCCEEDED(hr)) hr = buffer.As(&dxgiBuffer);
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(hr)) hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
        UINT subresource{};
        if (SUCCEEDED(hr)) hr = dxgiBuffer->GetSubresourceIndex(&subresource);
        if (FAILED(hr)) return hr;

        for (auto& target : targets_) {
            // 同一张解码纹理依次喂给每块屏幕的 Video Processor。
            // 每个 target 调用 SourceCrop，所以不同比例的显示器可以独立取景。
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
            inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            inputDesc.Texture2D.ArraySlice = subresource;
            inputDesc.Texture2D.MipSlice = 0;
            ComPtr<ID3D11VideoProcessorInputView> inputView;
            hr = videoDevice_->CreateVideoProcessorInputView(texture.Get(), target.enumerator.Get(),
                                                              &inputDesc, &inputView);
            if (FAILED(hr)) return hr;

            RECT source = g_previewParent ? RECT{0,0,LONG(videoWidth_),LONG(videoHeight_)} :
                SourceCrop(target.width, target.height,target.focusX,target.focusY);
            RECT destination{0, 0, static_cast<LONG>(target.width), static_cast<LONG>(target.height)};
            const RECT output=destination;
            if(g_previewParent)destination=PreviewDestination(target.width,target.height);
            videoContext_->VideoProcessorSetStreamFrameFormat(target.processor.Get(), 0,
                                                               D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            videoContext_->VideoProcessorSetStreamSourceRect(target.processor.Get(), 0, TRUE, &source);
            videoContext_->VideoProcessorSetStreamDestRect(target.processor.Get(), 0, TRUE, &destination);
            videoContext_->VideoProcessorSetOutputTargetRect(target.processor.Get(), TRUE, &output);
            D3D11_VIDEO_COLOR background{};background.RGBA.A=1;
            videoContext_->VideoProcessorSetOutputBackgroundColor(target.processor.Get(),FALSE,&background);

            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = inputView.Get();
            hr = videoContext_->VideoProcessorBlt(target.processor.Get(), target.outputView.Get(), 0, 1,
                                                   &stream);
            if (FAILED(hr)) return hr;
            effectSettings_=g_effectChannel.Read(effectSettings_);
            hr=target.effects.Render(device_.Get(),context_.Get(),target.backBuffer.Get(),effectSettings_,
                destination,float(lastTimestamp_.load()/10000000.0));
            if(FAILED(hr))return hr;
        }
        const UINT previewStride = (std::max)(1U,
            static_cast<UINT>((static_cast<uint64_t>(frameRateNumerator_) +
                               static_cast<uint64_t>(frameRateDenominator_) * 30 - 1) /
                              (static_cast<uint64_t>(frameRateDenominator_) * 30)));
        const bool previewFrame = (sharedPreviewFrameCounter_++ % previewStride) == 0;
        if (previewFrame && sharedPreview_.texture && sharedPreview_.keyedMutex &&
            sharedPreview_.keyedMutex->AcquireSync(0, 0) == S_OK) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
            inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            inputDesc.Texture2D.ArraySlice = subresource;
            inputDesc.Texture2D.MipSlice = 0;
            ComPtr<ID3D11VideoProcessorInputView> inputView;
            hr = videoDevice_->CreateVideoProcessorInputView(texture.Get(),
                                                              sharedPreview_.enumerator.Get(),
                                                              &inputDesc, &inputView);
            if (SUCCEEDED(hr)) {
                const RECT source{0, 0, static_cast<LONG>(videoWidth_),
                                  static_cast<LONG>(videoHeight_)};
                const RECT destination = PreviewDestination(SharedPreviewTarget::width,
                                                             SharedPreviewTarget::height);
                const RECT output{0, 0, static_cast<LONG>(SharedPreviewTarget::width),
                                  static_cast<LONG>(SharedPreviewTarget::height)};
                D3D11_VIDEO_COLOR background{};
                background.RGBA.A = 1.0f;
                videoContext_->VideoProcessorSetStreamFrameFormat(
                    sharedPreview_.processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
                videoContext_->VideoProcessorSetStreamSourceRect(
                    sharedPreview_.processor.Get(), 0, TRUE, &source);
                videoContext_->VideoProcessorSetStreamDestRect(
                    sharedPreview_.processor.Get(), 0, TRUE, &destination);
                videoContext_->VideoProcessorSetOutputTargetRect(
                    sharedPreview_.processor.Get(), TRUE, &output);
                videoContext_->VideoProcessorSetOutputBackgroundColor(
                    sharedPreview_.processor.Get(), FALSE, &background);
                D3D11_VIDEO_PROCESSOR_STREAM stream{};
                stream.Enable = TRUE;
                stream.pInputSurface = inputView.Get();
                hr = videoContext_->VideoProcessorBlt(sharedPreview_.processor.Get(),
                                                       sharedPreview_.outputView.Get(),
                                                       0, 1, &stream);
            }
            // 即使某帧处理失败也必须归还 key，否则两端会永久等待。
            sharedPreview_.keyedMutex->ReleaseSync(1);
            if (FAILED(hr)) return hr;
            if (previewNotifyWindow_) {
                PostMessageW(previewNotifyWindow_, kSharedPreviewFrameMessage, 0, 0);
            }
        }
        for (auto& target : targets_) {
            // 解码循环已经按视频时间戳限速。双屏逐个等待垂直同步会串行阻塞，
            // 非阻塞 Present 能避免不同刷新率显示器相互拖慢。
            // 首帧/恢复首帧不能被“GPU 忙”直接跳过后仍通知 ready；只在此帧
            // 等待提交，日常双屏播放仍保持非阻塞，避免不同刷新率相互拖慢。
            hr = target.swapChain->Present(0, firstFrameRendered_ ? DXGI_PRESENT_DO_NOT_WAIT : 0);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }

    void DecodeLoop() {
        // Source Reader 是阻塞式读取，放在工作线程可避免卡住壁纸窗口消息循环。
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        LONGLONG baseTimestamp = -1;
        auto baseClock = std::chrono::steady_clock::now();
        unsigned skippedFrames{};
        LONGLONG imageTime{};
        while (running_) {
            if (paused_) {
                std::unique_lock lock(playbackMutex_);
                playbackWake_.wait(lock, [this] { return !running_ || !paused_; });
                continue;
            }

            DWORD actualStream{};
            DWORD flags{};
            LONGLONG timestamp{};
            ComPtr<IMFSample> sample;
            auto* channel=g_effectChannel.Data();
            if(reader_ && channel) {
                const LONG seek=InterlockedExchange(&channel->seekMs,-1);
                if(seek>=0){PROPVARIANT position;PropVariantInit(&position);position.vt=VT_I8;
                    position.hVal.QuadPart=LONGLONG(seek)*10000;
                    const HRESULT seekResult=reader_->SetCurrentPosition(GUID_NULL,position);PropVariantClear(&position);
                    if(SUCCEEDED(seekResult))baseTimestamp=-1;}
            }
            HRESULT hr=S_OK;
            if(imageSample_){sample=imageSample_;timestamp=imageTime;imageTime+=333333;}
            else hr=reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&actualStream,&flags,&timestamp,&sample);
            if (FAILED(hr)) {
                if(running_ && readyNotifyWindow_) PostMessageW(readyNotifyWindow_,kDecodeFailed,
                    static_cast<WPARAM>(hr),0);
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                // 到文件末尾后直接把读取位置设为 0，形成无缝循环。
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart = 0;
                hr=reader_->SetCurrentPosition(GUID_NULL, position);
                PropVariantClear(&position);
                if(FAILED(hr)) {
                    if(running_ && readyNotifyWindow_) PostMessageW(readyNotifyWindow_,kDecodeFailed,
                        static_cast<WPARAM>(hr),0);
                    break;
                }
                baseTimestamp = -1;
                lastTimestamp_ = 0;
                sharedPreviewFrameCounter_ = 0;
                continue;
            }
            if (!sample) continue;
            lastTimestamp_ = timestamp;
            if(channel)InterlockedExchange(&channel->positionMs,static_cast<LONG>((std::min)(timestamp/10000,LONGLONG(LONG_MAX))));

            if (baseTimestamp < 0 || timelineReset_.exchange(false)) {
                // 视频时间戳单位为 100ns。用第一帧的时间戳和 steady_clock 建立映射，
                // 后续每帧睡眠到目标时间，避免无节制地把视频跑满 CPU/GPU。
                baseTimestamp = timestamp;
                baseClock = std::chrono::steady_clock::now();
            }
            const auto targetTime = baseClock + std::chrono::nanoseconds((timestamp - baseTimestamp) * 100);
            {
                std::unique_lock lock(playbackMutex_);
                playbackWake_.wait_until(lock, targetTime,
                    [this] { return !running_ || paused_ || timelineReset_; });
            }
            if (!running_) break;
            if (paused_) {
                timelineReset_ = true;
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto late = std::chrono::duration_cast<std::chrono::milliseconds>(now - targetTime).count();
            const int64_t frameMs = 1000LL * frameRateDenominator_ / (std::max)(1U, frameRateNumerator_);
            if (SkipLatePresentation(late, frameMs, firstFrameRendered_, skippedFrames)) {
                ++skippedFrames; continue;
            }
            // 长期算力不足时也保持画面前进，不无限追赶旧时间轴。
            if (skippedFrames >= 5) { baseTimestamp = timestamp; baseClock = now; }
            skippedFrames = 0;

            hr = RenderFrame(sample.Get());
            if(channel){if(SUCCEEDED(hr))InterlockedIncrement(&channel->frames);else InterlockedExchange(&channel->error,hr);}
            if (FAILED(hr)) {
                if(running_ && readyNotifyWindow_) PostMessageW(readyNotifyWindow_,kDecodeFailed,
                    static_cast<WPARAM>(hr),0);
                break;
            }
            if (!firstFrameRendered_.exchange(true) && readyNotifyWindow_) {
                // 只有首帧真正 Present 成功后才通知控制器“新壁纸已就绪”。
                // 控制器收到 ready 后再停止旧播放器，这是避免切换黑屏的核心。
                PostMessageW(readyNotifyWindow_, kReadyMessage, 0, 0);
            }
            if(imageSample_ && !effectSettings_.Particles()) {
                // 无动画图片只在设置改变时重画，平时不执行 GPU 提交。
                std::unique_lock lock(playbackMutex_);
                while(running_ && !paused_ && g_effectChannel.Read(effectSettings_)==effectSettings_)
                    playbackWake_.wait_for(lock,std::chrono::milliseconds(200));
                baseTimestamp=-1;
            }
        }
        CoUninitialize();
    }

    double focusX_{0.5};
    double focusY_{0.5};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<ID3D11Texture2D> imageTexture_;
    ComPtr<IMFSample> imageSample_;
    WallpaperEffects effectSettings_;
    std::wstring path_;
    std::vector<RenderTarget> targets_;
    SharedPreviewTarget sharedPreview_;
    std::vector<HWND> windows_;
    std::thread worker_;
    std::mutex playbackMutex_;
    std::condition_variable playbackWake_;
    std::atomic_bool running_{false};
    std::atomic_bool paused_{false};
    std::atomic_bool timelineReset_{false};
    std::atomic_bool firstFrameRendered_{false};
    std::atomic<LONGLONG> lastTimestamp_{0};
    LONGLONG resumeTimestamp_{};
    bool decoderLoaded_{};
    bool outputsReleased_{};
    HWND previewNotifyWindow_{};
    HWND readyNotifyWindow_{};
    uint64_t sharedPreviewFrameCounter_{};
    UINT videoWidth_{};
    UINT videoHeight_{};
    UINT frameRateNumerator_{30};
    UINT frameRateDenominator_{1};
    UINT pixelAspectNumerator_{1};
    UINT pixelAspectDenominator_{1};
};

NativePlayer g_player;
std::vector<HWND> g_windows;
HANDLE g_readyEvent{};
HANDLE g_pauseEvent{};
HWND g_controllerWindow{};
bool g_playbackFailed{};
bool g_hadReadyFrame{};
HINSTANCE g_instance{};
HWND g_coordinatorWindow{};
bool g_sessionLocked{};
bool g_systemSuspended{};
bool g_decoderReleasedForIdle{};
ULONGLONG g_inactiveSince{};
bool g_displayRebuildPending{};
ULONGLONG g_displayRebuildAfter{};

bool CreateWallpaperWindows() {
    if(g_previewParent){
        if(!IsWindow(g_previewParent))return false;
        // 只用于效果预览，绝不挂接桌面。独立 HWND 避免 MFPlay/DXGI 争用同一表面。
        RECT rect{};GetClientRect(g_previewParent,&rect);
        HWND window=CreateWindowExW(WS_EX_NOACTIVATE,kWallpaperClass,L"DWD Effect Preview",WS_CHILD,
            0,0,rect.right,rect.bottom,g_previewParent,nullptr,g_instance,nullptr);
        if(!window)return false;g_windows={window};return true;
    }
    HWND parent = FindWallpaperParent();
    LogDesktopTree();
    RuntimeLog(L"Selected wallpaper parent "+WindowDiagnostic(parent));
    RECT parentRect{};
    if (!parent || !GetWindowRect(parent, &parentRect)) return false;

    std::vector<HWND> created;
    const auto monitors=MonitorRects();
    for (const RECT& monitor : monitors) {
        const int width = monitor.right - monitor.left;
        const int height = monitor.bottom - monitor.top;
        POINT origin{monitor.left,monitor.top};
        MapWindowPoints(HWND_DESKTOP,parent,&origin,1);
        HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kWallpaperClass,
                                      L"DWD Wallpaper", WS_CHILD,
                                      origin.x, origin.y,
                                      width, height, parent, nullptr, g_instance, nullptr);
        if (window) created.push_back(window);
        RuntimeLog(L"Created output "+WindowDiagnostic(window));
    }
    if (created.size()!=monitors.size()) {
        for(HWND window:created) DestroyWindow(window);
        return false;
    }
    if (created.empty()) return false;
    g_windows = std::move(created);
    return true;
}

void DestroyWallpaperWindows() {
    for (HWND window : g_windows) {
        if (IsWindow(window)) DestroyWindow(window);
    }
    g_windows.clear();
    g_player.ForgetWindows();
}

void ScheduleDisplayRebuild(ULONGLONG delayMs) {
    g_displayRebuildPending = true;
    g_displayRebuildAfter = GetTickCount64() + delayMs;
}

void RebuildWallpaperOutputs() {
    // 显示拓扑稳定后，按照系统此刻真正枚举出的屏幕重新创建 HWND、交换链和视频处理器。
    // 旧输出先隐藏，新的输出仍等到首帧完成才显示，因此不会用黑色窗口遮住静态壁纸。
    if (!g_decoderReleasedForIdle) {
        if (FAILED(g_player.ReleaseDecoder())) return;
        g_decoderReleasedForIdle = true;
    }
    DestroyWallpaperWindows();
    if (!CreateWallpaperWindows()) return;
    if (SUCCEEDED(g_player.RestoreDecoder(g_windows))) {
        g_decoderReleasedForIdle = false;
    } else {
        DestroyWallpaperWindows();
    }
}

void UpdateEnergyState(bool releaseImmediately = false) {
    if(g_previewParent){
        if(!IsWindow(g_previewParent)){PostQuitMessage(0);return;}
        if(!g_windows.empty()) {RECT r{};GetClientRect(g_previewParent,&r);
            SetWindowPos(g_windows.front(),nullptr,0,0,r.right,r.bottom,SWP_NOZORDER|SWP_NOACTIVATE);}
        if(g_sessionLocked||g_systemSuspended||!IsWindowVisible(g_previewParent) ||
           (g_pauseEvent && WaitForSingleObject(g_pauseEvent,0)==WAIT_OBJECT_0))g_player.Pause();
        else g_player.Resume();
        return;
    }
    const bool energyInactive = g_sessionLocked || g_systemSuspended || IsFullscreenForeground();
    const bool userPaused = g_pauseEvent && WaitForSingleObject(g_pauseEvent,0)==WAIT_OBJECT_0;
    const bool inactive = energyInactive || userPaused;
    if(auto* data=g_effectChannel.Data())InterlockedExchange(&data->energyPaused,inactive?1:0);
    // “任一屏全屏时暂停”与“所有壁纸都不可见”是两回事。只有后一种情况
    // 才能隐藏输出、释放交换链；否则仍露出的副屏会突然退回静态壁纸。
    const bool fullyInvisible=g_sessionLocked || g_systemSuspended ||
        (energyInactive && IsFullscreenForeground(1));
    const ULONGLONG now = GetTickCount64();
    if (inactive) {
        if(fullyInvisible) {if(!g_inactiveSince) g_inactiveSince=now;}
        else g_inactiveSince=0;
        g_player.Pause();
        if (!g_decoderReleasedForIdle && fullyInvisible &&
            (releaseImmediately || now - g_inactiveSince >= kDecoderReleaseDelayMs)) {
            // 释放失败时保留暂停状态，下一个计时周期会再次尝试，不影响当前桌面画面。
            if (SUCCEEDED(g_player.ReleaseDecoder())) {
                g_decoderReleasedForIdle = true;
                DestroyWallpaperWindows();
                // HWND 销毁后 DWM 才会真正放开交换链，所以此时再通知驱动裁剪驻留内存。
                g_player.TrimReleasedMemory();
            }
        }
        return;
    }

    g_inactiveSince = 0;
    if (g_decoderReleasedForIdle) {
        // CreateDecoder 采用临时 Source Reader，失败不会破坏仍保留的最后一帧；
        // 因此可安全地每秒重试，直到视频文件/显卡设备恢复可用。
        if (g_windows.empty() && !CreateWallpaperWindows()) return;
        if (SUCCEEDED(g_player.RestoreDecoder(g_windows))) {
            g_decoderReleasedForIdle = false;
        } else {
            // 创建输出失败时不留下黑色子窗口；下一秒重新创建并重试。
            DestroyWallpaperWindows();
        }
    } else {
        g_player.Resume();
    }
}

LRESULT CALLBACK WallpaperProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case kDecodeFailed:
        // 工作线程只报告错误；释放/退出在窗口线程完成，避免线程自己 join 自己。
        g_playbackFailed=true;
        RuntimeLog(L"Decode/render failed HRESULT="+DiagnosticResult(static_cast<HRESULT>(wParam)));
        if(g_controllerWindow) PostMessageW(g_controllerWindow,kWallpaperErrorMessage,
            wParam,static_cast<LPARAM>(GetCurrentProcessId()));
        PostQuitMessage(7);return 0;
    case kReadyMessage:
        g_hadReadyFrame=true;
        // 创建时窗口保持隐藏，避免用户先看到一块黑色窗口；首帧完成后才统一显示。
        for (HWND window : g_windows) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        if (g_readyEvent) SetEvent(g_readyEvent);
        for(HWND window:g_windows) RuntimeLog(L"Ready output "+WindowDiagnostic(window));
        return 0;
    case WM_TIMER:
        if (wParam == kEnergyTimer) {
            // Explorer 重建壁纸宿主后，原子窗口可能已被系统销毁；这不是接口断电。
            // 只恢复 DWD 自己的输出，避免进程还活着却永远没有可见窗口。
            if(!g_decoderReleasedForIdle && !g_displayRebuildPending &&
                std::any_of(g_windows.begin(),g_windows.end(),[](HWND window){return !IsWindow(window);})) {
                RuntimeLog(L"Desktop host changed; rebuilding DWD outputs");
                ScheduleDisplayRebuild(0);
            }
            if (g_displayRebuildPending && GetTickCount64() >= g_displayRebuildAfter &&
                !g_sessionLocked && !g_systemSuspended && !IsFullscreenForeground()) {
                g_displayRebuildPending = false;
                RebuildWallpaperOutputs();
            }
            UpdateEnergyState();
        }
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) g_sessionLocked = true;
        else if (wParam == WTS_SESSION_UNLOCK) g_sessionLocked = false;
        UpdateEnergyState();
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) {
            g_systemSuspended = true;
            // 系统挂起已经是明确的长时不可见状态，不需要再等待 30 秒。
            UpdateEnergyState(true);
        } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            g_systemSuspended = false;
            // 只恢复 DWD 自己释放的渲染资源，不修改 Windows 显示拓扑或端口供电。
            UpdateEnergyState();
        }
        return TRUE;
    case WM_DISPLAYCHANGE:
        if(g_previewParent)return 0;
        // 扩展/复制模式、分辨率、旋转或热插拔发生变化时，旧交换链尺寸和屏幕数量
        // 已不再可靠。短暂去抖后统一重建，避免一轮变化触发多次解码器重启。
        ScheduleDisplayRebuild(kDisplayRebuildAfterChangeMs);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}

int RunPlayer(HINSTANCE instance) {
    // 这个函数由同一个 EXE 在 --player 模式下进入。控制器通过命名事件管理它：
    // stopEvent 请求退出，readyEvent 表示首帧已显示，parentProcess 用于检测控制器崩溃。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc{};
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring video = ArgValue(argc, argv, L"--video");
    const std::wstring stopName = ArgValue(argc, argv, L"--stop-event");
    const std::wstring readyName = ArgValue(argc, argv, L"--ready-event");
    const std::wstring pauseName = ArgValue(argc, argv, L"--pause-event");
    const std::wstring fullscreenRule = ArgValue(argc, argv, L"--fullscreen-rule");
    g_fullscreenRule=std::clamp(_wtoi(fullscreenRule.c_str()),0,2);
    g_requestedMonitor=ArgValue(argc,argv,L"--monitor-device");
    const auto screenFocus=ArgValue(argc,argv,L"--screen-focus");
    size_t start{};
    while(start<screenFocus.size()) {
        const auto end=screenFocus.find(L';',start);
        const auto field=screenFocus.substr(start,end==std::wstring::npos?std::wstring::npos:end-start);
        const auto eq=field.find(L'='),comma=field.find(L',');
        if(eq!=std::wstring::npos && comma!=std::wstring::npos && comma>eq) {
            const auto xText=field.substr(eq+1,comma-eq-1),yText=field.substr(comma+1);
            wchar_t* endX{};wchar_t* endY{};
            const double x=wcstod(xText.c_str(),&endX),y=wcstod(yText.c_str(),&endY);
            if(endX && !*endX && endY && !*endY && x>=0 && x<=1 && y>=0 && y<=1)
                g_screenFocus[field.substr(0,eq)]={x,y};
        }
        if(end==std::wstring::npos) break;
        start=end+1;
    }
    const std::wstring parentPidText = ArgValue(argc, argv, L"--parent-pid");
    const std::wstring focusXText = ArgValue(argc, argv, L"--focus-x");
    const std::wstring focusYText = ArgValue(argc, argv, L"--focus-y");
    const std::wstring previewTextureName = ArgValue(argc, argv, L"--preview-texture");
    const std::wstring previewOwnerText = ArgValue(argc, argv, L"--preview-owner");
    const auto effectName=ArgValue(argc,argv,L"--effects-channel");
    const auto previewParent=ArgValue(argc,argv,L"--preview-window");
    if(!previewParent.empty())g_previewParent=reinterpret_cast<HWND>(_wcstoui64(previewParent.c_str(),nullptr,10));
    if(!effectName.empty() && !g_effectChannel.Open(effectName,false)){LocalFree(argv);return 8;}
    LocalFree(argv);
    if (video.empty() || stopName.empty() || readyName.empty()) return 2;

    HANDLE stopEvent = OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str());
    g_readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    if(!pauseName.empty()) g_pauseEvent=OpenEventW(SYNCHRONIZE,FALSE,pauseName.c_str());
    HANDLE parentProcess = parentPidText.empty() ? nullptr :
        OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(_wtoi(parentPidText.c_str())));
    if (!stopEvent || !g_readyEvent) return 3;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return 4;
    g_instance = instance;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WallpaperProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWallpaperClass;
    RegisterClassExW(&windowClass);

    // 协调窗口永不显示，也不承载视频。深度休眠销毁双屏壁纸 HWND 后，
    // 它仍可接收计时器、锁屏和电源消息，并负责触发恢复。
    g_coordinatorWindow = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                           kWallpaperClass, L"DWD Coordinator",
                                           WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                           instance, nullptr);
    if (!g_coordinatorWindow || !CreateWallpaperWindows()) {
        DestroyWallpaperWindows();
        if (g_coordinatorWindow) DestroyWindow(g_coordinatorWindow);
        MFShutdown();
        CoUninitialize();
        CloseHandle(stopEvent);
        CloseHandle(g_readyEvent);
        if (parentProcess) CloseHandle(parentProcess);
        return 5;
    }

    WTSRegisterSessionNotification(g_coordinatorWindow, NOTIFY_FOR_THIS_SESSION);
    SetTimer(g_coordinatorWindow, kEnergyTimer, 1000, nullptr);
    const double focusX = focusXText.empty() ? 0.5 : _wtof(focusXText.c_str());
    const double focusY = focusYText.empty() ? 0.5 : _wtof(focusYText.c_str());
    const HWND previewOwner = previewOwnerText.empty() ? nullptr :
        reinterpret_cast<HWND>(static_cast<UINT_PTR>(_wcstoui64(previewOwnerText.c_str(), nullptr, 10)));
    g_controllerWindow=previewOwner;
    const HRESULT openResult = g_player.Open(video, g_windows, focusX, focusY,
                                             previewTextureName, previewOwner,
                                             g_coordinatorWindow);
    RuntimeLog(L"Player Open HRESULT="+DiagnosticResult(openResult)+L" video="+video);
    if (FAILED(openResult)) {
        g_player.Shutdown();
        DestroyWallpaperWindows();
        KillTimer(g_coordinatorWindow, kEnergyTimer);
        WTSUnRegisterSessionNotification(g_coordinatorWindow);
        DestroyWindow(g_coordinatorWindow);
        MFShutdown();
        CoUninitialize();
        CloseHandle(stopEvent);
        CloseHandle(g_readyEvent);
        if (parentProcess) CloseHandle(parentProcess);
        return 6;
    }

    MSG message{};
    HANDLE waits[2]{stopEvent, parentProcess};
    const DWORD waitCount = parentProcess ? 2 : 1;
    bool running = true;
    bool restoreWallpaper = false;
    while (running) {
        // 同时等待“控制器要求停止”“控制器进程意外退出”和 Win32 窗口消息。
        // 不能只 WaitForSingleObject，否则锁屏、挂起、定时省电消息都无法处理。
        const DWORD result = MsgWaitForMultipleObjects(waitCount, waits, FALSE, INFINITE, QS_ALLINPUT);
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + waitCount) {
            restoreWallpaper = parentProcess && result == WAIT_OBJECT_0 + 1;
            break;
        }
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    for (HWND window : g_windows) ShowWindow(window, SW_HIDE);
    g_player.Shutdown();
    DestroyWallpaperWindows();
    KillTimer(g_coordinatorWindow, kEnergyTimer);
    WTSUnRegisterSessionNotification(g_coordinatorWindow);
    DestroyWindow(g_coordinatorWindow);
    // 正常切换壁纸时由控制器显式发 stopEvent，不恢复静态壁纸；只有父控制器意外消失
    // 才在这里兜底恢复，避免桌面永久停留在最后一帧或黑色 WorkerW。
    // 新播放器尚未显示首帧就失败时，不能干扰仍在运行的旧壁纸。
    if (!g_previewParent && (restoreWallpaper || (g_playbackFailed && g_hadReadyFrame)))
        StaticWallpaper::Restore();

    MFShutdown();
    CoUninitialize();
    CloseHandle(stopEvent);
    CloseHandle(g_readyEvent);
    if(g_pauseEvent) CloseHandle(g_pauseEvent);
    if (parentProcess) CloseHandle(parentProcess);
    RuntimeLog(g_playbackFailed?L"Player stopped after playback error":L"Player stopped normally");
    return g_playbackFailed?7:0;
}
