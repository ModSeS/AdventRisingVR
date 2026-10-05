#include "GameDrawHook.h"
#include "StereoState.h"
#include "GameState.h"
#include "Log.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <MinHook.h>
#include "SharedReader.h"
#include "SceneViewHook.h"
#include "D3D9Hook.h"

namespace arvr {
using OriginalDrawFn=void(__thiscall*)(void*,void*,int,unsigned char*,int*);
static OriginalDrawFn gOriginal=nullptr;
using IsPausedFn=int(__thiscall*)(void*);
static IsPausedFn gIsPaused=nullptr;
static BYTE* gTarget=nullptr;
static thread_local bool gInside=false;
static uint64_t gDoubleDraws=0;
static ULONGLONG gStatStart=0;
static int gStatReports=0;
static double gDrawMs=0,gCaptureMs=0;
static double ClockMs(){LARGE_INTEGER t{},f{};QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return double(t.QuadPart)*1000/double(f.QuadPart);}

static void __fastcall Hook(void*self,void*,void*viewport,int blit,unsigned char*hitData,int*hitSize){
    if(!gOriginal) return;
    if(gIsPaused&&!gInside){
        const bool paused=gIsPaused(self)!=0;
        if(paused!=ControllerMenuActive())Log("Native game pause: %s",paused?"on":"off");
        SetControllerMenu(paused);
    }
    if(gInside){gOriginal(self,viewport,blit,hitData,hitSize);return;}
    if( StereoMode()!=StereoModeDoubleDraw || CurrentSceneMode()!=SceneGameplayVR || blit==0 || hitData!=nullptr){
        SetForcedStereoEye(StereoEyeMono);
        gOriginal(self,viewport,blit,hitData,hitSize);
        return;
    }

    gInside=true;
    BeginRenderSnapshot();
    __try {
        double begin=ClockMs();
        SetForcedStereoEye(StereoEyeLeft);gOriginal(self,viewport,blit,hitData,hitSize);
        double rendered=ClockMs();gDrawMs+=rendered-begin;
        CaptureRenderedStereoEye();
        begin=ClockMs();gCaptureMs+=begin-rendered;
        SetForcedStereoEye(StereoEyeRight);gOriginal(self,viewport,blit,hitData,hitSize);
        rendered=ClockMs();gDrawMs+=rendered-begin;
        CaptureRenderedStereoEye();
        gCaptureMs+=ClockMs()-rendered;
    } __finally {
        SetForcedStereoEye(StereoEyeMono);EndRenderSnapshot();gInside=false;
    }

    ++gDoubleDraws;
    ULONGLONG now=GetTickCount64(); if(!gStatStart) gStatStart=now;
    if(now-gStatStart>=5000){
        double sec=(now-gStatStart)/1000.0;
        Log("UGameEngine::Draw stereo cadence: %llu game draws in %.2f s (%.1f/s)",(unsigned long long)gDoubleDraws,sec,gDoubleDraws/sec);
        Log("Stereo cost per pair: nativeDraw=%.2f ms captureAndPublish=%.2f ms",gDrawMs/gDoubleDraws,gCaptureMs/gDoubleDraws);
        gDrawMs=gCaptureMs=0;
        gDoubleDraws=0;gStatStart=now;++gStatReports;
    }
}

static BYTE* FollowExportThunk(BYTE*p){
    if(p&&p[0]==0xE9){int32_t rel=*(int32_t*)(p+1);return p+5+rel;}
    return p;
}

bool InstallGameDrawHook(){
    if(StereoMode()!=StereoModeDoubleDraw){Log("UGameEngine::Draw stereo hook skipped: Stereo/Mode=%d",StereoMode());return true;}
    HMODULE m=nullptr;for(int i=0;i<300&&!(m=GetModuleHandleW(L"Engine.dll"));++i)Sleep(100);
    if(!m){Log("Engine.dll not loaded for UGameEngine::Draw hook");return false;}
    gIsPaused=(IsPausedFn)GetProcAddress(m,"?IsPaused@UGameEngine@@UAEHXZ");
    if(!gIsPaused){Log("Native pause query unavailable");return false;}
    const char*name="?Draw@UGameEngine@@UAEXPAVUViewport@@HPAEPAH@Z";
    BYTE*exp=(BYTE*)GetProcAddress(m,name);if(!exp){Log("UGameEngine::Draw export not found");return false;}
    gTarget=FollowExportThunk(exp);
    if(!gTarget || gTarget[0]!=0x6A || gTarget[1]!=0xFF || gTarget[2]!=0x68){
        Log("Unexpected UGameEngine::Draw prologue at %p: %02X %02X %02X",gTarget,gTarget?gTarget[0]:0,gTarget?gTarget[1]:0,gTarget?gTarget[2]:0);
        return false;
    }

    MH_STATUS st=MH_Initialize();
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_INITIALIZED)return false;
    st=MH_CreateHook(gTarget,(LPVOID)&Hook,reinterpret_cast<LPVOID*>(&gOriginal));
    if(st!=MH_OK){Log("Camera/draw hook create failed: %d",(int)st);return false;}
    st=MH_EnableHook(gTarget);
    if(st!=MH_OK){MH_RemoveHook(gTarget);gOriginal=nullptr;return false;}
    if(!InstallSceneViewHook()){MH_DisableHook(gTarget);MH_RemoveHook(gTarget);gOriginal=nullptr;Log("Stereo disabled: scene projection hook unavailable");return false;}
    Log("UGameEngine::Draw hook installed with MinHook");return true;
}
}
