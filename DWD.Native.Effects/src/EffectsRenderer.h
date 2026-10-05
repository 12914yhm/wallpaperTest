#pragma once
#include "WallpaperEffects.h"
#include <d3d11.h>
#include <wrl/client.h>

// 桌面与预览共用此绘制器。关闭效果时不创建纹理/着色器；每帧不回读 GPU 像素。
class EffectsRenderer {
public:
    HRESULT Render(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* output,
                   WallpaperEffects settings,const RECT& content,float seconds);
    void Reset();
private:
    HRESULT Initialize(ID3D11Device* device,ID3D11Texture2D* output);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_,particles_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> color_,particleColor_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    UINT width_{},height_{};
};
