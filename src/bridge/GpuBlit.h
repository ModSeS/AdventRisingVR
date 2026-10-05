#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
namespace arvr {
class GpuBlit {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11PixelShader> psSmooth,psAlpha;
    Ptr<ID3D11SamplerState> sampler;
    Ptr<ID3D11Texture2D> source;
    Ptr<ID3D11ShaderResourceView> srv;
    uint32_t width=0,height=0;
    DXGI_FORMAT sourceViewFormat=DXGI_FORMAT_UNKNOWN;
    Ptr<ID3D11Texture2D> processed;
    Ptr<ID3D11RenderTargetView> processedView;
    D3D11_TEXTURE2D_DESC processedDesc{};
    DXGI_FORMAT processedFormat=DXGI_FORMAT_UNKNOWN;
    uint64_t uploadedStamp=0,processedStamp=0;
    bool processedSmooth=false,processedAlpha=false;
    uint64_t uploads=0,filters=0,reuses=0;
public:
    bool Init(ID3D11Device* device);
    bool Draw(ID3D11Device* device,ID3D11DeviceContext* ctx,ID3D11Texture2D* target,
        const uint8_t* bgra,uint32_t w,uint32_t h,uint32_t pitch,DXGI_FORMAT viewFormat,bool smoothEdges=false,uint64_t sourceStamp=0,bool preserveAlpha=false);
    uint64_t UploadCount()const{return uploads;}
    uint64_t DrawCount()const{return filters;}
    uint64_t ReuseCount()const{return reuses;}
};
}
