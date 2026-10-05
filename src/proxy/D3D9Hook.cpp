#include <emmintrin.h>
#include "D3D9Hook.h"
#include "SceneViewHook.h"
#include "../shared/EyeCrop.h"
#include "Log.h"
#include "StereoState.h"
#include "GameState.h"
#include "SharedReader.h"
#include "../shared/SharedState.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cwchar>
#include <cmath>
#include <vector>

// Windows headers may have defined min/max before NOMINMAX became visible
// through another project header. Remove them here so C++ math calls are safe.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace arvr {
using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using CreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetTransformFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using EndSceneFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
static EndSceneFn gOrigEndScene=nullptr;
static thread_local IDirect3DDevice9* gRenderedDevice=nullptr;
static thread_local uint64_t gRenderedPair=0;
static thread_local StereoEye gRenderedEye=StereoEyeMono;

static PresentFn gOrigPresent = nullptr;
static ResetFn gOrigReset = nullptr;
static SetTransformFn gOrigSetTransform = nullptr;

static HANDLE gFrameMap = nullptr;
static FrameTransportHeader* gFrameHeader = nullptr;
static uint8_t* gFrameBase = nullptr;
static IDirect3DSurface9* gCaptureSurface = nullptr;
static IDirect3DSurface9* gResolveSurface = nullptr;
static UINT gCaptureW = 0, gCaptureH = 0;
static D3DFORMAT gCaptureFormat = D3DFMT_UNKNOWN;
static D3DMULTISAMPLE_TYPE gCaptureMSAA = D3DMULTISAMPLE_NONE;
static bool gDeviceHooked = false;
static bool gGlobalMethodHooks = false;
static bool gLoggedCopyFailure = false;
static bool gLoggedLockFailure = false;
static bool gLoggedFirstFrame = false;
static bool gLoggedViewport = false;
static bool gDumpedFirstFrame = false;
static uint64_t gPresentCalls = 0;
static ULONGLONG gPresentStatStart = 0;
static int gPresentStatReports = 0;
static bool gLoggedProjectionLeft = false;
static bool gLoggedProjectionRight = false;
static bool gLoggedProjectionNoFov = false;
static volatile LONG gLastBackbufferW=1920;
static volatile LONG gLastBackbufferH=1080;
static bool gLoggedEyeCrop[2]={false,false};
static bool gLoggedLegacyProjectionDisabled=false;
struct PendingEye {
    std::vector<uint8_t> pixels;
    uint32_t w=0,h=0;
    uint64_t pair=0,captured=0;
    Payload pose{};
};
static PendingEye gPendingLeft;
static uint32_t gSourceW=0,gSourceH=0,gCropX=0,gCropY=0,gCropW=0,gCropH=0;
static void WriteCaptureMetadata(){
    gFrameHeader->sourceWidth=gSourceW;gFrameHeader->sourceHeight=gSourceH;
    gFrameHeader->viewportX=gCropX;gFrameHeader->viewportY=gCropY;
    gFrameHeader->viewportWidth=gCropW;gFrameHeader->viewportHeight=gCropH;
}

static int IniInt(const wchar_t*s,const wchar_t*k,int d){return GetPrivateProfileIntW(s,k,d,L".\\AdventRisingVR.ini");}

float GetCurrentGameAspect(){
    LONG w=InterlockedCompareExchange(&gLastBackbufferW,0,0),h=InterlockedCompareExchange(&gLastBackbufferH,0,0);
    return (w>0&&h>0)?((float)w/(float)h):(16.0f/9.0f);
}

static const char* D3DFormatName(D3DFORMAT f){
    switch(f){
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_A8B8G8R8: return "A8B8G8R8";
        case D3DFMT_X8B8G8R8: return "X8B8G8R8";
        case D3DFMT_R5G6B5: return "R5G6B5";
        case D3DFMT_X1R5G5B5: return "X1R5G5B5";
        case D3DFMT_A1R5G5B5: return "A1R5G5B5";
        case D3DFMT_A2R10G10B10: return "A2R10G10B10";
        default: return "other";
    }
}

static bool EnsureFrameShare(){
    if(gFrameHeader) return true;
    gFrameMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)kFrameMappingSize, kFrameMappingName);
    if(!gFrameMap){ Log("Frame mapping create failed: %lu", GetLastError()); return false; }
    gFrameHeader = (FrameTransportHeader*)MapViewOfFile(gFrameMap, FILE_MAP_ALL_ACCESS, 0, 0, kFrameMappingSize);
    if(!gFrameHeader){ Log("Frame mapping map failed: %lu", GetLastError()); CloseHandle(gFrameMap); gFrameMap=nullptr; return false; }
    std::memset(gFrameHeader, 0, kFrameMappingSize);
    gFrameHeader->magic = kFrameMagic;
    gFrameHeader->version = kVersion;
    gFrameBase = reinterpret_cast<uint8_t*>(gFrameHeader) + sizeof(FrameTransportHeader);
    Log("Frame mapping ready: %u x %u max", kFrameMaxWidth, kFrameMaxHeight);
    return true;
}
static inline uint8_t* EyePtr(uint32_t eye){ return gFrameBase + (eye==StereoEyeRight ? kFrameEyeBytes : 0); }
static inline void BGRA(uint8_t* d, uint8_t b, uint8_t g, uint8_t r, uint8_t a=255){ d[0]=b; d[1]=g; d[2]=r; d[3]=a; }

static void ConvertPixel(uint8_t* d, const uint8_t* s, D3DFORMAT fmt){
    switch(fmt){
        case D3DFMT_A8R8G8B8: BGRA(d,s[0],s[1],s[2],s[3]); break;
        case D3DFMT_X8R8G8B8: BGRA(d,s[0],s[1],s[2],255); break;
        case D3DFMT_A8B8G8R8: BGRA(d,s[2],s[1],s[0],s[3]); break;
        case D3DFMT_X8B8G8R8: BGRA(d,s[2],s[1],s[0],255); break;
        case D3DFMT_R5G6B5: {
            uint16_t v=*reinterpret_cast<const uint16_t*>(s);
            uint8_t r=(uint8_t)(((v>>11)&31)*255/31), g=(uint8_t)(((v>>5)&63)*255/63), b=(uint8_t)((v&31)*255/31);
            BGRA(d,b,g,r,255); break;
        }
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5: {
            uint16_t v=*reinterpret_cast<const uint16_t*>(s);
            uint8_t r=(uint8_t)(((v>>10)&31)*255/31), g=(uint8_t)(((v>>5)&31)*255/31), b=(uint8_t)((v&31)*255/31);
            uint8_t a=(fmt==D3DFMT_A1R5G5B5 && !(v&0x8000))?0:255; BGRA(d,b,g,r,a); break;
        }
        case D3DFMT_A2R10G10B10: {
            uint32_t v=*reinterpret_cast<const uint32_t*>(s);
            uint8_t b=(uint8_t)((v&1023)*255/1023), g=(uint8_t)(((v>>10)&1023)*255/1023), r=(uint8_t)(((v>>20)&1023)*255/1023), a=(uint8_t)(((v>>30)&3)*255/3);
            BGRA(d,b,g,r,a); break;
        }
        default: BGRA(d,255,0,255,255); break;
    }
}

static uint32_t BytesPerPixel(D3DFORMAT fmt){
    switch(fmt){
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5: return 2;
        default: return 4;
    }
}

static bool SupportedCaptureFormat(D3DFORMAT fmt){
    switch(fmt){
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5: case D3DFMT_A2R10G10B10: return true;
        default: return false;
    }
}

static void ConvertFrame(uint8_t* dst, uint32_t dstW, uint32_t dstH,
                         const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t srcPitch, D3DFORMAT fmt){
    const uint32_t bpp=BytesPerPixel(fmt);
    for(uint32_t y=0;y<dstH;++y){
        const uint32_t sy=(dstH==srcH)?y:uint32_t((uint64_t)y*srcH/dstH);
        const uint8_t* srow=src+size_t(sy)*srcPitch;
        uint8_t* drow=dst+size_t(y)*dstW*4;
        if(dstW==srcW && (fmt==D3DFMT_A8R8G8B8 || fmt==D3DFMT_X8R8G8B8)){
            if(fmt==D3DFMT_A8R8G8B8) std::memcpy(drow,srow,size_t(dstW)*4);
            else {
                // Same BGR bytes and opaque alpha, four pixels per SSE2 operation.
                const auto opaque=_mm_set1_epi32((int)0xff000000u);uint32_t x=0;
                for(;x+4<=dstW;x+=4)_mm_storeu_si128((__m128i*)(drow+x*4),_mm_or_si128(_mm_loadu_si128((const __m128i*)(srow+x*4)),opaque));
                for(;x<dstW;++x){const uint8_t*s=srow+x*4;BGRA(drow+x*4,s[0],s[1],s[2],255);}
            }
        } else {
            for(uint32_t x=0;x<dstW;++x){
                const uint32_t sx=(dstW==srcW)?x:uint32_t((uint64_t)x*srcW/dstW);
                ConvertPixel(drow+x*4,srow+size_t(sx)*bpp,fmt);
            }
        }
    }
}

static void DumpBmp(const uint8_t* bgra,uint32_t w,uint32_t h,uint32_t pitch){
    if(gDumpedFirstFrame || !IniInt(L"Debug",L"DumpFirstFrame",1)) return;
    gDumpedFirstFrame=true;
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr,path,MAX_PATH);
    wchar_t* slash=wcsrchr(path,L'\\'); if(slash) *(slash+1)=0;
    wcscat_s(path,_countof(path),L"AdventRisingVR_capture.bmp");
    HANDLE f=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE){Log("Capture BMP create failed: %lu",GetLastError());return;}
    BITMAPFILEHEADER bfh{}; BITMAPINFOHEADER bih{};
    bih.biSize=sizeof(bih); bih.biWidth=(LONG)w; bih.biHeight=-(LONG)h; bih.biPlanes=1; bih.biBitCount=32; bih.biCompression=BI_RGB; bih.biSizeImage=w*h*4;
    bfh.bfType=0x4D42; bfh.bfOffBits=sizeof(bfh)+sizeof(bih); bfh.bfSize=bfh.bfOffBits+bih.biSizeImage;
    DWORD wr{}; WriteFile(f,&bfh,sizeof(bfh),&wr,nullptr); WriteFile(f,&bih,sizeof(bih),&wr,nullptr);
    for(uint32_t y=0;y<h;++y) WriteFile(f,bgra+size_t(y)*pitch,w*4,&wr,nullptr);
    CloseHandle(f); Log("First capture dumped: AdventRisingVR_capture.bmp (%ux%u)",w,h);
}

static void FitCaptureSize(uint32_t srcW,uint32_t srcH,uint32_t&dstW,uint32_t&dstH){
    if(!srcW||!srcH){dstW=dstH=0;return;}
    double scale=1.0;
    if(srcW>kFrameMaxWidth) scale=(double)kFrameMaxWidth/srcW;
    if(srcH*scale>kFrameMaxHeight) scale=(double)kFrameMaxHeight/srcH;
    dstW=(uint32_t)(srcW*scale+0.5);dstH=(uint32_t)(srcH*scale+0.5);
    if(!dstW)dstW=1;if(!dstH)dstH=1;
}

static void WriteFrameOne(uint32_t eye,const uint8_t*src,uint32_t srcW,uint32_t srcH,uint32_t srcPitch,D3DFORMAT fmt){
    if(!EnsureFrameShare()||!src||!srcW||!srcH||!SupportedCaptureFormat(fmt))return;
    uint32_t dstW=0,dstH=0;FitCaptureSize(srcW,srcH,dstW,dstH);
    const auto pair=RenderSnapshotId();
    if(StereoMode()==StereoModeDoubleDraw){
        Payload pose{};
        if(!pair || !ReadBridgePayload(pose))return;
        if(eye==StereoEyeLeft){
            gPendingLeft.pair=0;
            gPendingLeft.pixels.resize(size_t(dstW)*dstH*4);
            ConvertFrame(gPendingLeft.pixels.data(),dstW,dstH,src,srcW,srcH,srcPitch,fmt);
            gPendingLeft.w=dstW;gPendingLeft.h=dstH;gPendingLeft.pose=pose;
            gPendingLeft.captured=GetTickCount64();gPendingLeft.pair=pair;
            return;
        }
        if(gPendingLeft.pair!=pair || pose.predictedDisplayTime!=gPendingLeft.pose.predictedDisplayTime)return;
        // Publish metadata and both pixel arrays in ONE seqlock transaction.
        InterlockedIncrement(&gFrameHeader->sequence);MemoryBarrier();
        WriteCaptureMetadata();
        const uint32_t id=std::max(gFrameHeader->frameId[0],gFrameHeader->frameId[1])+1;
        gFrameHeader->sceneMode=SceneGameplayVR;
        for(uint32_t side=0;side<2;++side){
            const uint32_t w=side?dstW:gPendingLeft.w,h=side?dstH:gPendingLeft.h;
            gFrameHeader->width[side]=w;gFrameHeader->height[side]=h;gFrameHeader->pitch[side]=w*4;
            gFrameHeader->frameId[side]=id;
            gFrameHeader->capturedMs[side]=side?GetTickCount64():gPendingLeft.captured;
            gFrameHeader->renderPose[side]=pose.eyes[side];gFrameHeader->renderFov[side]=pose.eyeFov[side];
            gFrameHeader->poseTime[side]=pose.predictedDisplayTime;
        }
        std::memcpy(EyePtr(0),gPendingLeft.pixels.data(),gPendingLeft.pixels.size());
        ConvertFrame(EyePtr(1),dstW,dstH,src,srcW,srcH,srcPitch,fmt);
        MemoryBarrier();InterlockedIncrement(&gFrameHeader->sequence);
        gPendingLeft.pair=0;
        return;
    }
    uint8_t*dst=EyePtr(eye);
    InterlockedIncrement(&gFrameHeader->sequence);MemoryBarrier();
    WriteCaptureMetadata();
    gFrameHeader->sceneMode=SceneGameplayVR;
    gFrameHeader->width[eye]=dstW;gFrameHeader->height[eye]=dstH;gFrameHeader->pitch[eye]=dstW*4;
    ConvertFrame(dst,dstW,dstH,src,srcW,srcH,srcPitch,fmt);
    Payload pose{};
    if(ReadBridgePayload(pose)){
        gFrameHeader->renderPose[eye]=pose.eyes[eye];gFrameHeader->renderFov[eye]=pose.eyeFov[eye];
        gFrameHeader->poseTime[eye]=pose.predictedDisplayTime;
    } else {gFrameHeader->renderPose[eye]={};gFrameHeader->renderFov[eye]={};gFrameHeader->poseTime[eye]=0;}
    gFrameHeader->capturedMs[eye]=GetTickCount64();
    ++gFrameHeader->frameId[eye];MemoryBarrier();InterlockedIncrement(&gFrameHeader->sequence);
    DumpBmp(dst,dstW,dstH,dstW*4);
    if(!gLoggedFirstFrame){Log("First game frame published: eye=%u id=%u %ux%u format=%s",eye,gFrameHeader->frameId[eye],dstW,dstH,D3DFormatName(fmt));gLoggedFirstFrame=true;}
}

static void WriteFrameBoth(const uint8_t*src,uint32_t srcW,uint32_t srcH,uint32_t srcPitch,D3DFORMAT fmt){
    if(!EnsureFrameShare()||!src||!srcW||!srcH||!SupportedCaptureFormat(fmt))return;
    uint32_t dstW=0,dstH=0;FitCaptureSize(srcW,srcH,dstW,dstH);
    uint8_t*left=EyePtr(StereoEyeLeft); uint8_t*right=EyePtr(StereoEyeRight);
    InterlockedIncrement(&gFrameHeader->sequence);MemoryBarrier();
    gFrameHeader->sceneMode=SceneFlatScreen;
    WriteCaptureMetadata();
    gPendingLeft.pair=0;
    for(uint32_t eye=0;eye<2;++eye){gFrameHeader->width[eye]=dstW;gFrameHeader->height[eye]=dstH;gFrameHeader->pitch[eye]=dstW*4;}
    ConvertFrame(left,dstW,dstH,src,srcW,srcH,srcPitch,fmt);
    std::memcpy(right,left,size_t(dstW)*dstH*4);
    uint32_t id=((gFrameHeader->frameId[0]>gFrameHeader->frameId[1])?gFrameHeader->frameId[0]:gFrameHeader->frameId[1])+1;
    gFrameHeader->frameId[0]=id;gFrameHeader->frameId[1]=id;
    for(int eye=0;eye<2;eye++){gFrameHeader->capturedMs[eye]=GetTickCount64();gFrameHeader->poseTime[eye]=0;gFrameHeader->renderFov[eye]={};gFrameHeader->renderPose[eye]={};}
    MemoryBarrier();InterlockedIncrement(&gFrameHeader->sequence);
    DumpBmp(left,dstW,dstH,dstW*4);
    if(!gLoggedFirstFrame){Log("First game frame published to BOTH eyes: id=%u %ux%u format=%s",id,dstW,dstH,D3DFormatName(fmt));gLoggedFirstFrame=true;}
}

static void ReleaseCaptureSurfaces(){
    if(gCaptureSurface){gCaptureSurface->Release();gCaptureSurface=nullptr;}
    if(gResolveSurface){gResolveSurface->Release();gResolveSurface=nullptr;}
    gCaptureW=gCaptureH=0;gCaptureFormat=D3DFMT_UNKNOWN;gCaptureMSAA=D3DMULTISAMPLE_NONE;
}

static bool EnsureCaptureSurfaces(IDirect3DDevice9*dev,const D3DSURFACE_DESC&desc){
    if(gCaptureSurface&&gCaptureW==desc.Width&&gCaptureH==desc.Height&&gCaptureFormat==desc.Format&&gCaptureMSAA==desc.MultiSampleType)return true;
    ReleaseCaptureSurfaces();
    HRESULT hr=dev->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&gCaptureSurface,nullptr);
    if(FAILED(hr)){Log("CreateOffscreenPlainSurface %ux%u format=%s(%u) failed: 0x%08X",desc.Width,desc.Height,D3DFormatName(desc.Format),(unsigned)desc.Format,(unsigned)hr);return false;}
    if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE){
        hr=dev->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&gResolveSurface,nullptr);
        if(FAILED(hr)){Log("CreateRenderTarget resolve surface failed: 0x%08X",(unsigned)hr);ReleaseCaptureSurfaces();return false;}
    }
    gCaptureW=desc.Width;gCaptureH=desc.Height;gCaptureFormat=desc.Format;gCaptureMSAA=desc.MultiSampleType;
    Log("Created D3D9 capture surface %ux%u format=%s(%u) msaa=%u",gCaptureW,gCaptureH,D3DFormatName(desc.Format),(unsigned)desc.Format,(unsigned)desc.MultiSampleType);
    return true;
}

static bool ComputeOpenXREyeCrop(uint32_t backW,uint32_t backH,StereoEye eye,uint32_t& sx,uint32_t& sy,uint32_t& sw,uint32_t& sh){
    Payload p{}; if(!ReadBridgePayload(p)) return false;
    uint32_t idx=(eye==StereoEyeRight)?1u:0u;
    if(IniInt(L"Stereo",L"SwapEyes",0)) idx=1u-idx;
    const EyeFov& f=p.eyeFov[idx]; if(!f.valid) return false;
    float mx=0,my=0,ox=0,oy=0;EyeCrop crop{};
    if(!GetGameProjection(mx,my,ox,oy))return false;
    if(!ProjectedEyeCrop(backW,backH,f,mx,my,ox,oy,crop)){
        static ULONGLONG next=0;const auto now=GetTickCount64();
        if(now>=next){Log("Eye crop rejected: eye=%u matrix=(%.6f %.6f %.6f %.6f) FOV=(%.6f %.6f %.6f %.6f) source=%ux%u",idx,mx,my,ox,oy,f.angleLeft,f.angleRight,f.angleUp,f.angleDown,backW,backH);next=now+5000;}
        return false;
    }
    sx=crop.x;sy=crop.y;sw=crop.w;sh=crop.h;
    if(!gLoggedEyeCrop[idx]){Log("OpenXR eye crop: eye=%s rect=%u,%u %ux%u from %ux%u",idx?"R":"L",sx,sy,sw,sh,backW,backH);gLoggedEyeCrop[idx]=true;}
    return true;
}

static void CaptureFrame(IDirect3DDevice9*dev){
    if(!IniInt(L"Stereo",L"CaptureEnabled",1))return;
    Payload consumer{};
    if(!IniInt(L"Debug",L"CaptureWithoutRuntime",0) && (!ReadBridgePayload(consumer)||consumer.sessionState<2))return;
    IDirect3DSurface9*back=nullptr;HRESULT backHr=dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);
    if(FAILED(backHr)||!back){if(!gLoggedCopyFailure){Log("GetBackBuffer failed: 0x%08X",(unsigned)backHr);gLoggedCopyFailure=true;}return;}
    D3DSURFACE_DESC desc{};back->GetDesc(&desc);
    InterlockedExchange(&gLastBackbufferW,(LONG)desc.Width);InterlockedExchange(&gLastBackbufferH,(LONG)desc.Height);
    if(!SupportedCaptureFormat(desc.Format)){if(!gLoggedCopyFailure){Log("Unsupported D3D9 backbuffer format=%u (%s)",(unsigned)desc.Format,D3DFormatName(desc.Format));gLoggedCopyFailure=true;}back->Release();return;}
    if(!EnsureCaptureSurfaces(dev,desc)){back->Release();return;}

    IDirect3DSurface9*source=back;HRESULT copyHr=S_OK;
    if(gResolveSurface){copyHr=dev->StretchRect(back,nullptr,gResolveSurface,nullptr,D3DTEXF_NONE);if(SUCCEEDED(copyHr))source=gResolveSurface;}
    if(SUCCEEDED(copyHr))copyHr=dev->GetRenderTargetData(source,gCaptureSurface);
    if(FAILED(copyHr)){if(!gLoggedCopyFailure){Log("D3D9 frame copy failed: hr=0x%08X backbuffer=%ux%u format=%s(%u) msaa=%u",(unsigned)copyHr,desc.Width,desc.Height,D3DFormatName(desc.Format),(unsigned)desc.Format,(unsigned)desc.MultiSampleType);gLoggedCopyFailure=true;}back->Release();return;}

    D3DLOCKED_RECT lr{};HRESULT lockHr=gCaptureSurface->LockRect(&lr,nullptr,D3DLOCK_READONLY);
    if(SUCCEEDED(lockHr)){
        const uint32_t bpp=BytesPerPixel(desc.Format);
        uint32_t sx=0,sy=0,sw=desc.Width,sh=desc.Height;
        SceneMode scene=CurrentSceneMode();
        D3DVIEWPORT9 vp{};
        if(SUCCEEDED(dev->GetViewport(&vp))){
            if(!gLoggedViewport){Log("D3D9 viewport: x=%u y=%u w=%u h=%u within backbuffer=%ux%u",vp.X,vp.Y,vp.Width,vp.Height,desc.Width,desc.Height);gLoggedViewport=true;}
            // Menus are often rendered with a temporary 640x480 viewport inside a larger
            // backbuffer. Cropping to that viewport is exactly what produced the user's
            // top-left menu fragment. Flat UI therefore always captures the whole backbuffer.
            if(scene==SceneGameplayVR && IniInt(L"Stereo",L"CaptureViewportOnly",0) && vp.Width>0 && vp.Height>0 && vp.X+vp.Width<=desc.Width && vp.Y+vp.Height<=desc.Height){sx=vp.X;sy=vp.Y;sw=vp.Width;sh=vp.Height;}
        }
        if(scene==SceneGameplayVR && StereoMode()==StereoModeDoubleDraw && IniInt(L"Stereo",L"OpenXRCrop",1)){
            StereoEye eye=GetForcedStereoEye();uint32_t cx=sx,cy=sy,cw=sw,ch=sh;
            if((eye==StereoEyeLeft||eye==StereoEyeRight)&&ComputeOpenXREyeCrop(desc.Width,desc.Height,eye,cx,cy,cw,ch)){sx=cx;sy=cy;sw=cw;sh=ch;}
            else {
                // A failed eye is not a menu. Keep the previous complete pair;
                // never replace both eyes with an uncropped flat gameplay frame.
                gPendingLeft.pair=0;gCaptureSurface->UnlockRect();back->Release();return;
            }
        }
        gSourceW=desc.Width;gSourceH=desc.Height;gCropX=sx;gCropY=sy;gCropW=sw;gCropH=sh;
        const uint8_t*src=(const uint8_t*)lr.pBits+size_t(sy)*(uint32_t)lr.Pitch+size_t(sx)*bpp;
        const int mode=StereoMode();
        if(scene==SceneFlatScreen){WriteFrameBoth(src,sw,sh,(uint32_t)lr.Pitch,desc.Format);}
        else if(mode==StereoModeAlternate){
            StereoEye eye=GetActiveStereoEye();uint32_t dstEye=(eye==StereoEyeRight)?StereoEyeRight:StereoEyeLeft;
            WriteFrameOne(dstEye,src,sw,sh,(uint32_t)lr.Pitch,desc.Format);
        } else if(mode==StereoModeDoubleDraw){
            StereoEye eye=GetForcedStereoEye();
            if(eye==StereoEyeLeft || eye==StereoEyeRight) WriteFrameOne((uint32_t)eye,src,sw,sh,(uint32_t)lr.Pitch,desc.Format);
            else WriteFrameBoth(src,sw,sh,(uint32_t)lr.Pitch,desc.Format);
        } else WriteFrameBoth(src,sw,sh,(uint32_t)lr.Pitch,desc.Format);
        gCaptureSurface->UnlockRect();
    } else if(!gLoggedLockFailure){Log("D3D9 capture LockRect failed: 0x%08X",(unsigned)lockHr);gLoggedLockFailure=true;}
    back->Release();
}

static HRESULT STDMETHODCALLTYPE HookSetTransform(IDirect3DDevice9*self,D3DTRANSFORMSTATETYPE state,const D3DMATRIX*matrix){
    if(!gOrigSetTransform) return D3DERR_INVALIDCALL;
    if(state!=D3DTS_PROJECTION || !matrix || StereoMode()!=StereoModeDoubleDraw || CurrentSceneMode()!=SceneGameplayVR){
        return gOrigSetTransform(self,state,matrix);
    }
    if(!IniInt(L"Stereo",L"LegacyProjectionHook",0)){
        if(!gLoggedLegacyProjectionDisabled){Log("D3DTS_PROJECTION replacement disabled: UE2 fixed-function matrix is treated as UI/legacy path");gLoggedLegacyProjectionDisabled=true;}
        return gOrigSetTransform(self,state,matrix);
    }

    StereoEye eye=GetForcedStereoEye();
    if(eye!=StereoEyeLeft && eye!=StereoEyeRight) return gOrigSetTransform(self,state,matrix);

    Payload p{};
    if(!ReadBridgePayload(p)) return gOrigSetTransform(self,state,matrix);
    uint32_t idx=(eye==StereoEyeRight)?1u:0u;
    if(GetPrivateProfileIntW(L"Stereo",L"SwapEyes",0,L".\\AdventRisingVR.ini")) idx=1u-idx;
    const EyeFov& f=p.eyeFov[idx];
    if(!f.valid){
        if(!gLoggedProjectionNoFov){Log("Projection hook active but OpenXR FOV is not valid yet");gLoggedProjectionNoFov=true;}
        return gOrigSetTransform(self,state,matrix);
    }

    const float tanL=std::tan(f.angleLeft);
    const float tanR=std::tan(f.angleRight);
    const float tanU=std::tan(f.angleUp);
    const float tanD=std::tan(f.angleDown);
    const float dx=tanR-tanL;
    const float dy=tanU-tanD;
    if(std::fabs(dx)<0.0001f || std::fabs(dy)<0.0001f) return gOrigSetTransform(self,state,matrix);

    D3DMATRIX m=*matrix;
    // Keep Advent Rising's original near/far Z mapping and replace only the
    // perspective scale/centre terms with the exact asymmetric OpenXR eye FOV.
    m._11=2.0f/dx;
    m._22=2.0f/dy;
    m._31=(tanL+tanR)/(tanL-tanR);
    m._32=(tanU+tanD)/(tanD-tanU);

    bool& logged=(eye==StereoEyeLeft)?gLoggedProjectionLeft:gLoggedProjectionRight;
    if(!logged){
        Log("OpenXR projection applied: eye=%s m11=%.4f m22=%.4f m31=%.4f m32=%.4f original=(%.4f %.4f %.4f %.4f)",
            eye==StereoEyeLeft?"L":"R",m._11,m._22,m._31,m._32,matrix->_11,matrix->_22,matrix->_31,matrix->_32);
        logged=true;
    }
    return gOrigSetTransform(self,state,&m);
}

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* self){
    const HRESULT hr=gOrigEndScene?gOrigEndScene(self):D3DERR_INVALIDCALL;
    if(SUCCEEDED(hr) && RenderSnapshotId()){
        gRenderedDevice=self;gRenderedPair=RenderSnapshotId();gRenderedEye=GetForcedStereoEye();
    }
    return hr;
}
void CaptureRenderedStereoEye(){
    if(gRenderedDevice && gRenderedPair==RenderSnapshotId() && gRenderedEye==GetForcedStereoEye()){
        __try {CaptureFrame(gRenderedDevice);}
        __except(EXCEPTION_EXECUTE_HANDLER){Log("Stereo eye capture exception trapped");gPendingLeft.pair=0;}
    }else{
        static ULONGLONG next=0;const auto now=GetTickCount64();
        if(now>=next){Log("Stereo capture missing EndScene: pair=%llu eye=%u",RenderSnapshotId(),(unsigned)GetForcedStereoEye());next=now+5000;}
    }
    gRenderedDevice=nullptr;gRenderedPair=0;
}
static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9*self,const RECT*a,const RECT*b,HWND c,const RGNDATA*d){
    ++gPresentCalls;const ULONGLONG now=GetTickCount64();if(!gPresentStatStart)gPresentStatStart=now;
    if(gPresentStatReports<6&&now-gPresentStatStart>=5000){double sec=(now-gPresentStatStart)/1000.0;Log("Present cadence: %llu calls in %.2f s (%.1f calls/s)",(unsigned long long)gPresentCalls,sec,gPresentCalls/sec);gPresentCalls=0;gPresentStatStart=now;++gPresentStatReports;}
    // Engine.dll Draw ends with Unlock and marks viewport+0x190 for a later
    // desktop blit. At this Present the stereo snapshot is already gone.
    if(StereoMode()!=StereoModeDoubleDraw || CurrentSceneMode()!=SceneGameplayVR){
        __try{CaptureFrame(self);}__except(EXCEPTION_EXECUTE_HANDLER){Log("CaptureFrame exception trapped");}
    }
    // In true sequential stereo the left pass is captured for OpenXR but is not
    // presented to the desktop. The right pass is the one normal desktop Present.
    if(StereoMode()==StereoModeDoubleDraw && GetForcedStereoEye()==StereoEyeLeft) return D3D_OK;
    return gOrigPresent?gOrigPresent(self,a,b,c,d):D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9*self,D3DPRESENT_PARAMETERS*pp){
    gRenderedDevice=nullptr;gRenderedPair=0;gPendingLeft.pair=0;
    ReleaseCaptureSurfaces();
    if(EnsureFrameShare()){InterlockedIncrement(&gFrameHeader->sequence);++gFrameHeader->resolutionGeneration;MemoryBarrier();InterlockedIncrement(&gFrameHeader->sequence);}
    Log("D3D9 Reset intercepted: requested backbuffer=%ux%u format=%u windowed=%d generation=%u",pp?pp->BackBufferWidth:0,pp?pp->BackBufferHeight:0,pp?(unsigned)pp->BackBufferFormat:0,pp?pp->Windowed:0,gFrameHeader?gFrameHeader->resolutionGeneration:0);
    return gOrigReset?gOrigReset(self,pp):D3DERR_INVALIDCALL;
}

static HWND CreateDummyWindow(){
    return CreateWindowExW(0,L"STATIC",L"AdventRisingVR_D3D9Probe",WS_POPUP,0,0,8,8,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
}

static Direct3DCreate9Fn SystemDirect3DCreate9(){
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path,MAX_PATH);
    wcscat_s(path,_countof(path),L"\\d3d9.dll");
    HMODULE m=GetModuleHandleW(path);
    if(!m) m=LoadLibraryW(path);
    if(!m) m=GetModuleHandleW(L"d3d9.dll");
    return m?(Direct3DCreate9Fn)GetProcAddress(m,"Direct3DCreate9"):nullptr;
}

static bool InstallGlobalDeviceMethodHooks(){
    if(gGlobalMethodHooks) return true;
    Direct3DCreate9Fn create9=SystemDirect3DCreate9();
    if(!create9){Log("D3D9 fallback: system Direct3DCreate9 not found");return false;}
    IDirect3D9* d3d=create9(D3D_SDK_VERSION);
    if(!d3d){Log("D3D9 fallback: Direct3DCreate9 failed");return false;}
    HWND hwnd=CreateDummyWindow();
    if(!hwnd){d3d->Release();Log("D3D9 fallback: dummy window creation failed: %lu",GetLastError());return false;}
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=hwnd;pp.BackBufferFormat=D3DFMT_UNKNOWN;
    IDirect3DDevice9* dev=nullptr;
    HRESULT hr=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hwnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&dev);
    if(FAILED(hr)) hr=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_REF,hwnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&dev);
    if(FAILED(hr)||!dev){DestroyWindow(hwnd);d3d->Release();Log("D3D9 fallback: dummy CreateDevice failed: 0x%08X",(unsigned)hr);return false;}
    void** vt=*reinterpret_cast<void***>(dev);
    void* resetTarget=vt[16];
    void* presentTarget=vt[17];
    void* transformTarget=vt[44];
    void* endSceneTarget=vt[42];
    if(presentTarget==(void*)&HookPresent || resetTarget==(void*)&HookReset || transformTarget==(void*)&HookSetTransform){
        Log("Refusing recursive D3D9 hook target");dev->Release();DestroyWindow(hwnd);d3d->Release();return false;
    }
    MH_STATUS st=MH_Initialize();
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_INITIALIZED){dev->Release();DestroyWindow(hwnd);d3d->Release();return false;}
    void* targets[]={presentTarget,resetTarget,transformTarget,endSceneTarget};
    void* detours[]={(void*)&HookPresent,(void*)&HookReset,(void*)&HookSetTransform,(void*)&HookEndScene};
    void** originals[]={reinterpret_cast<void**>(&gOrigPresent),reinterpret_cast<void**>(&gOrigReset),reinterpret_cast<void**>(&gOrigSetTransform),reinterpret_cast<void**>(&gOrigEndScene)};
    unsigned created=0;bool ok=true;
    for(unsigned i=0;i<4;i++){
        st=MH_CreateHook(targets[i],detours[i],originals[i]);
        if(st!=MH_OK){Log("D3D9 hook create failed index=%u status=%d",i,(int)st);ok=false;break;}
        ++created;
    }
    if(ok){
        for(unsigned i=0;i<4;i++){
            st=MH_QueueEnableHook(targets[i]);if(st!=MH_OK){ok=false;break;}
        }
        if(ok)ok=MH_ApplyQueued()==MH_OK;
    }
    if(!ok){
        for(unsigned i=0;i<created;i++){MH_DisableHook(targets[i]);MH_RemoveHook(targets[i]);}
        gOrigPresent=nullptr;gOrigReset=nullptr;gOrigSetTransform=nullptr;gOrigEndScene=nullptr;
    }
    dev->Release();DestroyWindow(hwnd);d3d->Release();
    if(ok){
        gGlobalMethodHooks=true;gDeviceHooked=true;EnsureFrameShare();
        Log("D3D9 fallback global hooks installed: Present=%p Reset=%p SetTransform=%p",presentTarget,resetTarget,transformTarget);
    }
    return ok;
}

bool InstallD3D9Hooks(){
    if(!IniInt(L"Stereo",L"InstallCaptureHook",0))return true;
    // Exactly one interception layer. Mixing vtable and inline hooks made the
    // probe read our own HookPresent/Reset addresses and hook them recursively.
    return InstallGlobalDeviceMethodHooks();
}
}
