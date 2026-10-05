#include "GpuBlit.h"
#include "BridgeLog.h"
#include <d3dcompiler.h>
#include <cstring>
namespace arvr {
bool GpuBlit::Init(ID3D11Device* d){
    // Source pixels are display-encoded. Decode while sampling; an sRGB RTV
    // encodes the filtered linear value exactly once for the compositor.
    const char* shader=R"(
        Texture2D source:register(t0);SamplerState linearClamp:register(s0);
        struct Vertex{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};
        Vertex VS(uint id:SV_VertexID){
            Vertex v;v.uv=float2((id<<1)&2,id&2);
            v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;
        }
        float4 PS(Vertex v):SV_TARGET{return float4(source.Sample(linearClamp,v.uv).rgb,1);}
        float4 PSAlpha(Vertex v):SV_TARGET{return source.Sample(linearClamp,v.uv);}
        float luminance(float3 c){return dot(c,float3(.299,.587,.114));}
        float4 PSSmooth(Vertex v):SV_TARGET{
            uint w,h;source.GetDimensions(w,h);float2 texel=1.0/float2(w,h);
            float3 center=source.SampleLevel(linearClamp,v.uv,0).rgb;
            float nw=luminance(source.SampleLevel(linearClamp,v.uv+texel*float2(-1,-1),0).rgb);
            float ne=luminance(source.SampleLevel(linearClamp,v.uv+texel*float2(1,-1),0).rgb);
            float sw=luminance(source.SampleLevel(linearClamp,v.uv+texel*float2(-1,1),0).rgb);
            float se=luminance(source.SampleLevel(linearClamp,v.uv+texel*float2(1,1),0).rgb);
            float mid=luminance(center),lo=min(mid,min(min(nw,ne),min(sw,se))),hi=max(mid,max(max(nw,ne),max(sw,se)));
            if(hi-lo<max(.025,hi*.125))return float4(center,1);
            float2 direction=float2(-(nw+ne-sw-se),nw+sw-ne-se);
            float reduce=max((nw+ne+sw+se)*.03125,.0078125);
            direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduce),-4,4)*texel;
            float3 inner=.5*(source.SampleLevel(linearClamp,v.uv-direction/6,0).rgb+source.SampleLevel(linearClamp,v.uv+direction/6,0).rgb);
            float3 outer=inner*.5+.25*(source.SampleLevel(linearClamp,v.uv-direction*.5,0).rgb+source.SampleLevel(linearClamp,v.uv+direction*.5,0).rgb);
            float l=luminance(outer);return float4(l<lo||l>hi?inner:outer,1);
        }
    )";
    Ptr<ID3DBlob> v,p,err;
    if(FAILED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"VS","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&v,&err)) ||
       FAILED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"PS","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&p,&err))){
        BridgeLog("GPU shader compile failed: %s",err?(const char*)err->GetBufferPointer():"unknown");return false;
    }
    if(FAILED(d->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs)) ||
       FAILED(d->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps)))return false;
    Ptr<ID3DBlob> smooth;
    if(FAILED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"PSSmooth","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&smooth,&err)) ||
       FAILED(d->CreatePixelShader(smooth->GetBufferPointer(),smooth->GetBufferSize(),nullptr,&psSmooth)))return false;
    Ptr<ID3DBlob> alpha;
    if(FAILED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"PSAlpha","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&alpha,&err)) ||
       FAILED(d->CreatePixelShader(alpha->GetBufferPointer(),alpha->GetBufferSize(),nullptr,&psAlpha)))return false;
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    return SUCCEEDED(d->CreateSamplerState(&sd,&sampler));
}

bool GpuBlit::Draw(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Texture2D* target,
    const uint8_t* bgra,uint32_t w,uint32_t h,uint32_t pitch,DXGI_FORMAT viewFormat,bool smoothEdges,uint64_t sourceStamp,bool preserveAlpha){
    if(!target||!vs)return false;
    D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
    if(bgra&&(!w||!h||pitch<w*4))return false;
    // Nonzero identity opts into a private, full-resolution processed image.
    // Caller must create the OpenXR target with TRANSFER_DST usage.
    bool cache=bgra&&sourceStamp&&td.ArraySize==1&&td.MipLevels==1&&td.SampleDesc.Count==1;
    if(cache){
        if(!processed||processedDesc.Width!=td.Width||processedDesc.Height!=td.Height||processedDesc.Format!=td.Format||processedFormat!=viewFormat){
            processed.Reset();processedView.Reset();processedStamp=0;
            auto desc=td;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;desc.CPUAccessFlags=0;desc.MiscFlags=0;
            D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=viewFormat;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
            if(FAILED(d->CreateTexture2D(&desc,nullptr,&processed))||FAILED(d->CreateRenderTargetView(processed.Get(),&rd,&processedView))){
                processed.Reset();processedView.Reset();cache=false;
            } else {processedDesc=td;processedFormat=viewFormat;}
        }
        if(cache&&processedStamp==sourceStamp&&processedSmooth==smoothEdges&&processedAlpha==preserveAlpha&&width==w&&height==h){
            c->CopyResource(target,processed.Get());++reuses;return true;
        }
    }
    Ptr<ID3D11RenderTargetView> rtv;
    if(cache)rtv=processedView;
    else {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=viewFormat;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        if(FAILED(d->CreateRenderTargetView(target,&rd,&rtv)))return false;
        processedStamp=0;
    }
    if(!bgra){const float black[4]={0,0,0,1};c->ClearRenderTargetView(rtv.Get(),black);return true;}
    if(w!=width||h!=height||!source){
        source.Reset();srv.Reset();width=height=0;uploadedStamp=0;
        D3D11_TEXTURE2D_DESC sd{};sd.Width=w;sd.Height=h;sd.MipLevels=sd.ArraySize=1;
        sd.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS;sd.SampleDesc.Count=1;sd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(d->CreateTexture2D(&sd,nullptr,&source)))return false;
        width=w;height=h;
    }
    const bool srgb=viewFormat==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || viewFormat==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const auto desired=srgb?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM;
    if(!srv||sourceViewFormat!=desired){
        srv.Reset();D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=desired;
        sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
        if(FAILED(d->CreateShaderResourceView(source.Get(),&sv,&srv)))return false;sourceViewFormat=desired;
    }
    if(!sourceStamp||uploadedStamp!=sourceStamp){c->UpdateSubresource(source.Get(),0,nullptr,bgra,pitch,0);uploadedStamp=sourceStamp;++uploads;}
    auto rt=rtv.Get();c->OMSetRenderTargets(1,&rt,nullptr);
    D3D11_VIEWPORT vp{0,0,float(td.Width),float(td.Height),0,1};c->RSSetViewports(1,&vp);
    c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(preserveAlpha?psAlpha.Get():(smoothEdges?psSmooth.Get():ps.Get()),nullptr,0);
    auto sm=sampler.Get();auto view=srv.Get();c->PSSetSamplers(0,1,&sm);c->PSSetShaderResources(0,1,&view);
    c->Draw(3,0);++filters;
    ID3D11ShaderResourceView* empty=nullptr;c->PSSetShaderResources(0,1,&empty);c->OMSetRenderTargets(0,nullptr,nullptr);
    if(cache){
        processedStamp=sourceStamp;processedSmooth=smoothEdges;processedAlpha=preserveAlpha;
        c->CopyResource(target,processed.Get());
    }
    return true;
}
}
