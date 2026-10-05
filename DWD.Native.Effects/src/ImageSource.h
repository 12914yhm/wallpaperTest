#pragma once
#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <cwctype>
inline bool IsWallpaperImage(const std::wstring& path){
 auto extension=std::filesystem::path(path).extension().wstring();
 std::transform(extension.begin(),extension.end(),extension.begin(),towlower);
 return extension==L".jpg"||extension==L".jpeg"||extension==L".png"||extension==L".bmp";
}
inline HRESULT LoadImageTexture(ID3D11Device* device,const std::wstring& path,
 ID3D11Texture2D** texture,UINT& width,UINT& height){
 using Microsoft::WRL::ComPtr;
 ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
 ComPtr<IWICFormatConverter> converter;
 HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
 if(SUCCEEDED(hr))hr=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder);
 if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
 if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);
 if(FAILED(hr))return hr;
 if(!width||!height||width>16384||height>16384)return E_INVALIDARG;
 // 图片无需保留超大解码表面；限制长边 4096，用 WIC 按需缩放而不是先展开全图。
 ComPtr<IWICBitmapScaler> scaler;ComPtr<IWICBitmapSource> source=frame;
 if(width>4096||height>4096){
  const double ratio=4096.0/(std::max)(width,height);width=(std::max)(2U,UINT(width*ratio));height=(std::max)(2U,UINT(height*ratio));
  hr=factory->CreateBitmapScaler(&scaler);
  if(SUCCEEDED(hr))hr=scaler->Initialize(frame.Get(),width,height,WICBitmapInterpolationModeFant);
  if(FAILED(hr))return hr;source=scaler;
 }
 hr=factory->CreateFormatConverter(&converter);
 if(SUCCEEDED(hr))hr=converter->Initialize(source.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
 std::vector<BYTE> pixels(size_t(width)*height*4);
 if(SUCCEEDED(hr))hr=converter->CopyPixels(nullptr,width*4,static_cast<UINT>(pixels.size()),pixels.data());
 D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
 desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
 desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
 D3D11_SUBRESOURCE_DATA data{pixels.data(),width*4,0};
 if(SUCCEEDED(hr))hr=device->CreateTexture2D(&desc,&data,texture);
 return hr;
}
