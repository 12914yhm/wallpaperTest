#include "EffectsRenderer.h"
#include "LibraryPreferences.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Check(HRESULT hr,const char* message){if(FAILED(hr)){std::cerr<<message<<" HRESULT="<<std::hex<<hr<<"\n";throw std::runtime_error(message);}}
int main(){try{
 WallpaperEffects e;e.kind=3;e.brightness=72;e.warmth=-18;e.opacity=63;
 Require(WallpaperEffects::Decode(e.Encode())==e,"effect roundtrip");
 auto invalid=WallpaperEffects::Decode(L"9,-4,200,400,0,-1,300");Require(invalid.kind==3&&invalid.density==0&&invalid.brightness==30&&invalid.warmth==50,"clamp input");
 LibraryPreference p;p.favorite=true;p.lastUsed=134060023456700000ULL;p.playlists=7;p.effects=e;
 auto back=LibraryPreference::Decode(p.Encode());Require(back.favorite&&back.lastUsed==p.lastUsed&&back.playlists==7&&back.effects==e,"preference roundtrip");
 Require(CurrentPlaylist(true,0,6)==2&&CurrentPlaylist(true,0,7)==1&&CurrentPlaylist(true,0,19)==2,"day boundaries");
 Require(NextPlaylistIndex({1,3,5},3,false,0)==5&&NextPlaylistIndex({1,3,5},5,false,0)==1,"sequential rotation");
 for(int i=0;i<100;++i)Require(NextPlaylistIndex({1,3,5},3,true,i)!=3,"random repeat");
 Require(NextPlaylistIndex({3},3,false,0)==SIZE_MAX,"single item no restart");
 const auto name=L"Local\\DWD_EffectTest_"+std::to_wstring(GetCurrentProcessId());EffectChannel producer,consumer;
 Require(producer.Open(name,true)&&consumer.Open(name,false),"open channel");Require(producer.Write(e)&&consumer.Read({})==e,"read settings");
 InterlockedExchange(&producer.Data()->seekMs,3200);Require(InterlockedExchange(&consumer.Data()->seekMs,-1)==3200,"seek command");
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
 Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"WARP device");
 constexpr UINT width=320,height=180;D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
 desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;desc.Usage=D3D11_USAGE_DEFAULT;
 ComPtr<ID3D11Texture2D> output,staging;Check(device->CreateTexture2D(&desc,nullptr,&output),"target");
 desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 Check(device->CreateTexture2D(&desc,nullptr,&staging),"readback");
 std::vector<unsigned> pixels(width*height,0xff808080);EffectsRenderer renderer;RECT rect{0,0,width,height};
 auto render=[&](WallpaperEffects settings,float t){context->UpdateSubresource(output.Get(),0,nullptr,pixels.data(),width*4,0);
  Check(renderer.Render(device.Get(),context.Get(),output.Get(),settings,rect,t),"render shader");
  context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE map{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"map");
  std::vector<unsigned> result(width*height);for(UINT y=0;y<height;++y)memcpy(result.data()+y*width,static_cast<BYTE*>(map.pData)+y*map.RowPitch,width*4);
  context->Unmap(staging.Get(),0);return result;};
 Require(render({},0)==pixels,"off path preserves pixels");WallpaperEffects dim;dim.brightness=50;const auto darker=render(dim,0);
 Require((darker[width*height/2]&255)<80,"brightness visible");
 for(int kind=1;kind<=3;++kind){WallpaperEffects fx;fx.kind=kind;fx.density=100;fx.opacity=100;
  const auto a=render(fx,1),b=render(fx,3);Require(a!=pixels&&a!=b,"particle visible and moving");}
 Require(render({},0)==pixels,"disable after enable restores original");
 std::cout<<"PASS effects serialization, IPC, playlists, shader compile, color, three particles, disable\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
