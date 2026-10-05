#include "StereoState.h"
#include "SharedReader.h"
#include <Windows.h>
#include <cmath>

namespace arvr {
static volatile LONG gFrameCounter = 0;
static volatile LONG gActiveEye = StereoEyeMono;
static volatile LONG gForcedEye = StereoEyeMono;

static int IniInt(const wchar_t*s,const wchar_t*k,int d){
    return GetPrivateProfileIntW(s,k,d,L".\\AdventRisingVR.ini");
}
static float IniFloat(const wchar_t*s,const wchar_t*k,float d){
    wchar_t b[64]{};
    GetPrivateProfileStringW(s,k,L"",b,64,L".\\AdventRisingVR.ini");
    return *b ? (float)_wtof(b) : d;
}

int StereoMode(){ return IniInt(L"Stereo", L"Mode", StereoModeMono); }
bool StereoEnabled(){ return StereoMode()!=StereoModeMono; }

void AdvanceStereoEye(){
    if(StereoMode()!=StereoModeAlternate){
        InterlockedExchange(&gActiveEye,StereoEyeMono);
        return;
    }
    LONG f=InterlockedIncrement(&gFrameCounter);
    InterlockedExchange(&gActiveEye,(f&1)?StereoEyeLeft:StereoEyeRight);
}

StereoEye GetActiveStereoEye(){ return (StereoEye)InterlockedCompareExchange(&gActiveEye,0,0); }
void SetForcedStereoEye(StereoEye eye){ InterlockedExchange(&gForcedEye,(LONG)eye); }
StereoEye GetForcedStereoEye(){ return (StereoEye)InterlockedCompareExchange(&gForcedEye,0,0); }
StereoEye GetCameraStereoEye(){
    if(StereoMode()==StereoModeDoubleDraw) return GetForcedStereoEye();
    if(StereoMode()==StereoModeAlternate) return GetActiveStereoEye();
    return StereoEyeMono;
}

void ApplyStereoEyeOffset(FVector& loc,const Pose& originHead,const Pose& currentHead,
                          float unitsPerMeter,int baseYaw,int /*finalYaw*/){
    const int mode=StereoMode();
    if(mode!=StereoModeAlternate && mode!=StereoModeDoubleDraw) return;
    const StereoEye eye=GetCameraStereoEye();
    if(eye==StereoEyeMono) return;

    const float ipd=IniFloat(L"Stereo",L"IPDMeters",0.064f);
    float sign=(eye==StereoEyeLeft)?-0.5f:0.5f;
    if(IniInt(L"Stereo",L"SwapEyes",0)) sign=-sign;

    const float ang=baseYaw*(2.0f*3.14159265358979323846f/65536.0f);
    const float cs=std::cos(ang),sn=std::sin(ang);
    Quat rel=Mul(Conjugate(Quat{originHead.qx,originHead.qy,originHead.qz,originHead.qw}),
                 Quat{currentHead.qx,currentHead.qy,currentHead.qz,currentHead.qw});
    FVector local=Rotate(rel,{sign*ipd,0.0f,0.0f});
    Payload p{};uint32_t idx=eye==StereoEyeRight?1:0;
    if(IniInt(L"Stereo",L"SwapEyes",0))idx=1-idx;
    if(ReadBridgePayload(p) && (p.eyes[idx].flags&PosePositionValid) && (currentHead.flags&PosePositionValid)){
        local=Rotate(Conjugate(Quat{originHead.qx,originHead.qy,originHead.qz,originHead.qw}),
            {p.eyes[idx].px-currentHead.px,p.eyes[idx].py-currentHead.py,p.eyes[idx].pz-currentHead.pz});
    }
    FVector mapped{-local.Z,local.X,local.Y};
    loc.X+=(mapped.X*cs-mapped.Y*sn)*unitsPerMeter;
    loc.Y+=(mapped.X*sn+mapped.Y*cs)*unitsPerMeter;
    loc.Z+=mapped.Z*unitsPerMeter;
}
}
