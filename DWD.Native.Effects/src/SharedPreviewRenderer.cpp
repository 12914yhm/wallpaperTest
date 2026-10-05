#include "SharedPreviewRenderer.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <iterator>
#include "EffectsRenderer.h"

using Microsoft::WRL::ComPtr;

struct SharedPreviewRenderer::State {
    std::wstring textureName;
    HWND window{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sharedTexture;
    ComPtr<IDXGIKeyedMutex> keyedMutex;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11Texture2D> backBuffer;
    EffectsRenderer effects;
};

SharedPreviewRenderer::SharedPreviewRenderer() = default;
SharedPreviewRenderer::~SharedPreviewRenderer() { Stop(); }

bool SharedPreviewRenderer::Open(const std::wstring& sharedTextureName, HWND targetWindow) {
    if (sharedTextureName.empty() || !targetWindow) return false;
    if (state_ && state_->textureName == sharedTextureName && state_->window == targetWindow &&
        SUCCEEDED(state_->device->GetDeviceRemovedReason())) return Render();

    auto next = std::make_unique<State>();
    if (state_ && SUCCEEDED(state_->device->GetDeviceRemovedReason())) {
        next->device = state_->device; next->context = state_->context;
    }
    Stop(); // 先释放同一 HWND 的旧交换链，但有效设备可保留复用。
    next->textureName = sharedTextureName; next->window = targetWindow;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = next->device ? S_OK : D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                   &next->device, &selected, &next->context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                               &next->device, &selected, &next->context);
    }

    ComPtr<ID3D11Device1> device1;
    if (SUCCEEDED(hr)) hr = next->device.As(&device1);
    if (SUCCEEDED(hr)) {
        hr = device1->OpenSharedResourceByName(sharedTextureName.c_str(), DXGI_SHARED_RESOURCE_READ,
                                               IID_PPV_ARGS(&next->sharedTexture));
    }
    if (SUCCEEDED(hr)) hr = next->sharedTexture.As(&next->keyedMutex);

    D3D11_TEXTURE2D_DESC sharedDesc{};
    if (SUCCEEDED(hr)) next->sharedTexture->GetDesc(&sharedDesc);
    if (SUCCEEDED(hr) && (!sharedDesc.Width || !sharedDesc.Height ||
                          sharedDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)) {
        hr = E_UNEXPECTED;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (SUCCEEDED(hr)) hr = next->device.As(&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = sharedDesc.Width;
    swapDesc.Height = sharedDesc.Height;
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.Scaling = DXGI_SCALING_STRETCH;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateSwapChainForHwnd(next->device.Get(), targetWindow, &swapDesc,
                                              nullptr, nullptr, &next->swapChain);
    }
    if (SUCCEEDED(hr)) hr = next->swapChain->GetBuffer(0, IID_PPV_ARGS(&next->backBuffer));
    if (FAILED(hr)) return false;

    if (ComPtr<IDXGIDevice1> latencyDevice; SUCCEEDED(next->device.As(&latencyDevice))) {
        latencyDevice->SetMaximumFrameLatency(1);
    }
    state_ = std::move(next);
    return Render();
}

bool SharedPreviewRenderer::Render() {
    if (!state_) return false;
    // 生产者使用 key 0，写完后交给 key 1；这里只做一次 GPU 内复制，不回读到系统内存。
    HRESULT acquire = state_->keyedMutex->AcquireSync(1, 0);
    // 暂停/静态图片也能试调效果；key 0 空闲时读上一帧，读完仍归还 key 0。
    if(acquire==WAIT_TIMEOUT)acquire=state_->keyedMutex->AcquireSync(0,0);
    if (acquire == WAIT_TIMEOUT) return true;
    if (acquire != S_OK) return false;
    state_->context->CopyResource(state_->backBuffer.Get(), state_->sharedTexture.Get());
    state_->keyedMutex->ReleaseSync(0);
    D3D11_TEXTURE2D_DESC desc{};state_->backBuffer->GetDesc(&desc);
    RECT content{0,0,LONG(desc.Width),LONG(desc.Height)};
    if(source_.cx>0 && source_.cy>0) {
        const double scale=(std::min)(double(desc.Width)/source_.cx,double(desc.Height)/source_.cy);
        const LONG w=LONG(source_.cx*scale),h=LONG(source_.cy*scale);
        content={LONG(desc.Width-w)/2,LONG(desc.Height-h)/2,LONG(desc.Width+w)/2,LONG(desc.Height+h)/2};
    }
    if(FAILED(state_->effects.Render(state_->device.Get(),state_->context.Get(),state_->backBuffer.Get(),
        effects_,content,float(GetTickCount64()%3600000)/1000.f)))return false;
    // 不让 GPU 队列等待占住 UI 线程；忙时丢一帧，下一次通知自然更新。
    const HRESULT present = state_->swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    return SUCCEEDED(present) || present == DXGI_ERROR_WAS_STILL_DRAWING;
}

void SharedPreviewRenderer::Stop() {
    if (!state_) return;
    state_->context->ClearState();
    state_->context->Flush();
    state_.reset();
}

bool SharedPreviewRenderer::IsOpen() const { return state_ != nullptr; }
void SharedPreviewRenderer::SetEffects(WallpaperEffects value,SIZE source){effects_=value;source_=source;}
