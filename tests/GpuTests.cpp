#include "bridge/GpuBlit.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace arvr {void BridgeLog(const char*,...){};}
static void check(bool v,const char* msg){if(!v){std::printf("FAIL: %s\n",msg);std::exit(1);}}
int main(){
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c)),"WARP device");
    arvr::GpuBlit blit;check(blit.Init(d.Get()),"shader compile");
    const uint8_t pixels[16]={0,0,255,255,0,255,0,255,255,0,0,255,255,255,255,255};
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}){
      for(bool typeless:{false,true}){
        const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.ArraySize=td.MipLevels=1;td.SampleDesc.Count=1;
        td.Format=typeless?(bgra?DXGI_FORMAT_B8G8R8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_TYPELESS):format;td.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> target,staging;
        check(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&target)),"target");
        check(blit.Draw(d.Get(),c.Get(),target.Get(),pixels,2,2,8,format),"GPU blit");
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&staging)),"staging");c->CopyResource(staging.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};check(SUCCEEDED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"readback");
        auto p=(uint8_t*)m.pData;const unsigned red=bgra?2:0,blue=bgra?0:2;
        check(p[red]>250&&p[1]<3&&p[blue]<3,"red upper left");
        check(p[12+red]<3&&p[13]>250,"green upper right");check(p[3*m.RowPitch+blue]>250,"blue lower left");
        check(p[m.RowPitch+4+1]>10 && p[m.RowPitch+4+1]<245,"linear interpolation");std::vector<uint8_t> reference(64);
        for(int y=0;y<4;++y)std::memcpy(reference.data()+y*16,p+y*m.RowPitch,16);
        c->Unmap(staging.Get(),0);
        const auto beforeUpload=blit.UploadCount(),beforeDraw=blit.DrawCount(),beforeReuse=blit.ReuseCount();
        for(int frame=0;frame<10;++frame)check(blit.Draw(d.Get(),c.Get(),target.Get(),pixels,2,2,8,format,false,42),"cached full-resolution draw");
        check(blit.UploadCount()==beforeUpload+1&&blit.DrawCount()==beforeDraw+1&&blit.ReuseCount()==beforeReuse+9,"duplicate frame avoids upload and shader draw");
        c->CopyResource(staging.Get(),target.Get());check(SUCCEEDED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"cache readback");
        for(int y=0;y<4;++y)check(!std::memcmp(reference.data()+y*16,(uint8_t*)m.pData+y*m.RowPitch,16),"cache is pixel-exact");c->Unmap(staging.Get(),0);
        const uint8_t gray[]={128,128,128,255};
        check(blit.Draw(d.Get(),c.Get(),target.Get(),gray,1,1,4,format),"gray blit");
        c->CopyResource(staging.Get(),target.Get());check(SUCCEEDED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"gray readback");
        p=(uint8_t*)m.pData;check(p[0]>=127&&p[0]<=129,"sRGB conversion exactly once");c->Unmap(staging.Get(),0);
        check(blit.Draw(d.Get(),c.Get(),target.Get(),nullptr,0,0,0,format),"black background");
        c->CopyResource(staging.Get(),target.Get());check(SUCCEEDED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"black readback");
        p=(uint8_t*)m.pData;check(p[0]==0&&p[1]==0&&p[2]==0&&p[3]==255,"opaque black");c->Unmap(staging.Get(),0);
      }
    }
    D3D11_TEXTURE2D_DESC edgeDesc{};edgeDesc.Width=edgeDesc.Height=32;edgeDesc.ArraySize=edgeDesc.MipLevels=1;edgeDesc.SampleDesc.Count=1;
    edgeDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;edgeDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> edgeTarget,edgeRead;
    check(SUCCEEDED(d->CreateTexture2D(&edgeDesc,nullptr,&edgeTarget)),"edge target");
    edgeDesc.BindFlags=0;edgeDesc.Usage=D3D11_USAGE_STAGING;edgeDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(SUCCEEDED(d->CreateTexture2D(&edgeDesc,nullptr,&edgeRead)),"edge staging");
    std::vector<uint8_t> diagonal(32*32*4);
    for(int y=0;y<32;++y)for(int x=0;x<32;++x){auto p=&diagonal[(y*32+x)*4];p[0]=p[1]=p[2]=x>y?255:0;p[3]=255;}
    unsigned intermediate[2]{};
    for(int enabled=0;enabled<2;++enabled){
        check(blit.Draw(d.Get(),c.Get(),edgeTarget.Get(),diagonal.data(),32,32,128,DXGI_FORMAT_R8G8B8A8_UNORM,enabled!=0),"edge filter draw");
        c->CopyResource(edgeRead.Get(),edgeTarget.Get());D3D11_MAPPED_SUBRESOURCE m{};
        check(SUCCEEDED(c->Map(edgeRead.Get(),0,D3D11_MAP_READ,0,&m)),"edge readback");
        for(int y=2;y<30;++y)for(int x=2;x<30;++x){auto p=(uint8_t*)m.pData+y*m.RowPitch+x*4;
            if(p[0]>2&&p[0]<253)++intermediate[enabled];
            check(p[3]==255,"filter keeps alpha opaque");
            if(x>y+5)check(p[0]>252,"white interior unchanged");
            if(y>x+5)check(p[0]<3,"black interior unchanged");
        }c->Unmap(edgeRead.Get(),0);
    }


    // Cache the filtered image, then compare byte-for-byte with direct output.
    check(blit.Draw(d.Get(),c.Get(),edgeTarget.Get(),diagonal.data(),32,32,128,DXGI_FORMAT_R8G8B8A8_UNORM,true),"reference smoothed image");
    c->CopyResource(edgeRead.Get(),edgeTarget.Get());D3D11_MAPPED_SUBRESOURCE exact{};
    check(SUCCEEDED(c->Map(edgeRead.Get(),0,D3D11_MAP_READ,0,&exact)),"reference map");std::vector<uint8_t> expected(4096);
    for(int y=0;y<32;++y)std::memcpy(expected.data()+y*128,(uint8_t*)exact.pData+y*exact.RowPitch,128);c->Unmap(edgeRead.Get(),0);
    for(int i=0;i<12;++i)check(blit.Draw(d.Get(),c.Get(),edgeTarget.Get(),diagonal.data(),32,32,128,DXGI_FORMAT_R8G8B8A8_UNORM,true,200),"cached filter");
    c->CopyResource(edgeRead.Get(),edgeTarget.Get());check(SUCCEEDED(c->Map(edgeRead.Get(),0,D3D11_MAP_READ,0,&exact)),"cached filter map");
    for(int y=0;y<32;++y)check(!std::memcmp(expected.data()+y*128,(uint8_t*)exact.pData+y*exact.RowPitch,128),"cached smoothing pixel equality");c->Unmap(edgeRead.Get(),0);
    std::fill(diagonal.begin(),diagonal.end(),255);
    check(blit.Draw(d.Get(),c.Get(),edgeTarget.Get(),diagonal.data(),32,32,128,DXGI_FORMAT_R8G8B8A8_UNORM,true,201),"new frame invalidates cache");
    c->CopyResource(edgeRead.Get(),edgeTarget.Get());check(SUCCEEDED(c->Map(edgeRead.Get(),0,D3D11_MAP_READ,0,&exact)),"new frame map");
    check(*(uint32_t*)exact.pData==0xffffffffu,"new frame visible immediately");c->Unmap(edgeRead.Get(),0);
    const uint8_t translucent[]={255,255,255,96};
    check(blit.Draw(d.Get(),c.Get(),edgeTarget.Get(),translucent,1,1,4,DXGI_FORMAT_R8G8B8A8_UNORM,false,100,true),"HUD alpha shader");
    c->CopyResource(edgeRead.Get(),edgeTarget.Get());D3D11_MAPPED_SUBRESOURCE alpha{};
    check(SUCCEEDED(c->Map(edgeRead.Get(),0,D3D11_MAP_READ,0,&alpha)),"alpha readback");
    check(((uint8_t*)alpha.pData)[3]==96,"HUD straight alpha preserved");c->Unmap(edgeRead.Get(),0);
    check(intermediate[1]>intermediate[0],"diagonal edge receives subpixel coverage");
    std::puts("PASS: typed/TYPELESS RGBA/BGRA colors and gamma; GPU edge filter softens diagonal and preserves flat regions");
}
