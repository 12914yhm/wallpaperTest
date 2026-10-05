#include "EffectsRenderer.h"
#include <d3dcompiler.h>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace {
// 一次全屏颜色处理，再以实例化小四边形绘制粒子，避免每个像素遍历所有粒子。
constexpr char shader[]=R"(
cbuffer Params:register(b0){float4 adjustment;float4 motion;float4 area;float4 screen;};
Texture2D source:register(t0);SamplerState linearSampler:register(s0);
struct V {float4 p:SV_Position;float2 uv:TEXCOORD0;float alpha:TEXCOORD1;};
V Full(uint id:SV_VertexID){V o;float2 uv=float2((id<<1)&2,id&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.uv=uv;o.alpha=1;return o;}
float4 Color(V i):SV_Target{
 float4 c=source.Sample(linearSampler,i.uv);
 if(all(i.uv>=area.xy)&&all(i.uv<=area.zw)){
 float l=dot(c.rgb,float3(.2126,.7152,.0722));c.rgb=lerp(l.xxx,c.rgb,adjustment.y)*adjustment.x;
 c.rgb*=float3(1+adjustment.z*.15,1,1-adjustment.z*.15);}
 return float4(saturate(c.rgb),1);
}
float hash(float n){return frac(sin(n*127.1+311.7)*43758.5453);}
V Particle(uint vertex:SV_VertexID,uint id:SV_InstanceID){
 V o;float2 q=float2(vertex==1||vertex==2||vertex==4?1:-1,vertex>=2&&vertex<=4?1:-1);
 float seed=id+1;float depth=.35+.65*hash(seed+51);float t=motion.x*(.2+motion.y*1.8);
 float2 p=float2(hash(seed),hash(seed+17));float2 size;
 if(adjustment.w<1.5){p+=float2(sin(t*.24+seed)*.04,-t*.009*depth);size=float2(.0025,.0045)*depth;}
 else if(adjustment.w<2.5){p+=float2(-t*.035,t*.35)*depth;size=float2(.00055,.02)*depth;}
 else {p+=float2(sin(t*.2+seed)*.035,t*.045*depth);size=float2(.002,.0036)*depth;}
 p=frac(p);p=lerp(area.xy,area.zw,p);size*=float2(720/screen.x,720/screen.y);
 float2 uv=p+q*size; o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.uv=q;
 o.alpha=motion.z*depth*(adjustment.w<1.5?(.55+.45*sin(t+seed)):1);return o;
}
float4 ParticleColor(V i):SV_Target{
 float d=length(i.uv);float a=adjustment.w>1.5&&adjustment.w<2.5?
  (1-abs(i.uv.x))*(1-abs(i.uv.y)):1-smoothstep(.25,1,d);
 return float4(adjustment.w<1.5?float3(.65,1,.45):float3(.8,.9,1),a*i.alpha);
}
)";
struct Constants {float adjustment[4],motion[4],area[4],screen[4];};
HRESULT Compile(const char* entry,const char* profile,ID3DBlob** result){
 return D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,result,nullptr);
}
}
void EffectsRenderer::Reset(){copy_.Reset();input_.Reset();target_.Reset();vertex_.Reset();particles_.Reset();
 color_.Reset();particleColor_.Reset();constants_.Reset();sampler_.Reset();blend_.Reset();raster_.Reset();width_=height_=0;}
HRESULT EffectsRenderer::Initialize(ID3D11Device* device,ID3D11Texture2D* output){
 D3D11_TEXTURE2D_DESC desc{};output->GetDesc(&desc);
 if(copy_ && width_==desc.Width && height_==desc.Height)return S_OK;
 Reset();width_=desc.Width;height_=desc.Height;
 desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=0;desc.CPUAccessFlags=0;desc.Usage=D3D11_USAGE_DEFAULT;
 HRESULT hr=device->CreateTexture2D(&desc,nullptr,&copy_);
 if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(copy_.Get(),nullptr,&input_);
 if(SUCCEEDED(hr))hr=device->CreateRenderTargetView(output,nullptr,&target_);
 ComPtr<ID3DBlob> blob;
 if(SUCCEEDED(hr))hr=Compile("Full","vs_4_0",&blob);
 if(SUCCEEDED(hr))hr=device->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&vertex_);
 blob.Reset();if(SUCCEEDED(hr))hr=Compile("Particle","vs_4_0",&blob);
 if(SUCCEEDED(hr))hr=device->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&particles_);
 blob.Reset();if(SUCCEEDED(hr))hr=Compile("Color","ps_4_0",&blob);
 if(SUCCEEDED(hr))hr=device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&color_);
 blob.Reset();if(SUCCEEDED(hr))hr=Compile("ParticleColor","ps_4_0",&blob);
 if(SUCCEEDED(hr))hr=device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&particleColor_);
 D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(Constants);buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
 if(SUCCEEDED(hr))hr=device->CreateBuffer(&buffer,nullptr,&constants_);
 D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
 sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
 if(SUCCEEDED(hr))hr=device->CreateSamplerState(&sampler,&sampler_);
 D3D11_BLEND_DESC blend{};auto& b=blend.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_SRC_ALPHA;
 b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;b.SrcBlendAlpha=D3D11_BLEND_ONE;
 b.DestBlendAlpha=D3D11_BLEND_ZERO;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
 if(SUCCEEDED(hr))hr=device->CreateBlendState(&blend,&blend_);
 D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;raster.ScissorEnable=TRUE;
 if(SUCCEEDED(hr))hr=device->CreateRasterizerState(&raster,&raster_);
 if(FAILED(hr))Reset();return hr;
}
HRESULT EffectsRenderer::Render(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* output,
 WallpaperEffects s,const RECT& content,float seconds){
 s.Clamp();if(!s.Enabled()){if(copy_){context->ClearState();Reset();}return S_OK;}
 HRESULT hr=Initialize(device,output);if(FAILED(hr))return hr;
 Constants c{{s.brightness/100.f,s.saturation/100.f,s.warmth/50.f,float(s.kind)},
  {seconds,s.speed/100.f,s.opacity/100.f,0},
  {float(content.left)/width_,float(content.top)/height_,float(content.right)/width_,float(content.bottom)/height_},
  {float(width_),float(height_),0,0}};
 context->CopyResource(copy_.Get(),output);context->UpdateSubresource(constants_.Get(),0,nullptr,&c,0,0);
 auto* cb=constants_.Get();auto* srv=input_.Get();auto* rt=target_.Get();auto* sampler=sampler_.Get();
 context->OMSetRenderTargets(1,&rt,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);
 context->OMSetDepthStencilState(nullptr,0);context->RSSetState(raster_.Get());
 D3D11_VIEWPORT viewport{0,0,float(width_),float(height_),0,1};context->RSSetViewports(1,&viewport);
 RECT full{0,0,LONG(width_),LONG(height_)};context->RSSetScissorRects(1,&full);
 context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
 context->PSSetShaderResources(0,1,&srv);context->PSSetSamplers(0,1,&sampler);
 context->VSSetShader(vertex_.Get(),nullptr,0);context->PSSetShader(color_.Get(),nullptr,0);context->Draw(3,0);
 if(s.Particles()){
  context->RSSetScissorRects(1,&content);context->OMSetBlendState(blend_.Get(),nullptr,0xffffffff);
  context->VSSetShader(particles_.Get(),nullptr,0);context->PSSetShader(particleColor_.Get(),nullptr,0);
  context->DrawInstanced(6,UINT(s.density*2+8),0,0);
 }
 srv=nullptr;context->PSSetShaderResources(0,1,&srv);context->OMSetRenderTargets(0,nullptr,nullptr);
 return S_OK;
}
