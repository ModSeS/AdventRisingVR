#include "SceneViewHook.h"
#include "StereoState.h"
#include "SharedReader.h"
#include "D3D9Hook.h"
#include "GameState.h"
#include "MathVR.h"
#include "Log.h"
#include "../shared/ProjectionCalibration.h"
#include <MinHook.h>
#include <cmath>
#include <cstring>
namespace arvr {
using ConstructFn=void*(__thiscall*)(void*,void*,void*,void*,FVector,FRotator,float);
static ConstructFn original=nullptr;
static thread_local float projection[4]{};
static thread_local int projectionEye=-1;
static thread_local int64_t projectionTime=0;
static thread_local uint64_t projectionPair=0;
static thread_local ProjectionCalibration calibration;
bool GetGameProjection(float& x,float& y,float& ox,float& oy){
    Payload p{};
    if(projectionEye!=(int)GetForcedStereoEye() || !ReadBridgePayload(p) || p.predictedDisplayTime!=projectionTime || !projectionPair || projectionPair!=RenderSnapshotId()){
        static ULONGLONG next=0;const auto now=GetTickCount64();
        if(now>=next){Log("Projection unavailable: storedEye=%d captureEye=%d storedPair=%llu capturePair=%llu storedTime=%lld captureTime=%lld",projectionEye,(int)GetForcedStereoEye(),projectionPair,RenderSnapshotId(),projectionTime,p.predictedDisplayTime);next=now+5000;}
        return false;
    }
    x=projection[0];y=projection[1];ox=projection[2];oy=projection[3];
    return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(ox)&&std::isfinite(oy)&&x>0.01f&&y>0.01f;
}
static void* __fastcall Construct(void* self,void*,void* viewport,void* target,void* actor,FVector loc,FRotator rot,float fov){
    Payload p{};const auto eye=GetForcedStereoEye();
    bool vr=CurrentSceneMode()==SceneGameplayVR && (eye==StereoEyeLeft||eye==StereoEyeRight) && ReadBridgePayload(p);
    const float aspect=GetCurrentGameAspect();const float before=fov;
    if(vr && p.eyeFov[0].valid && p.eyeFov[1].valid && aspect>0.1f){
        float v=0,h=0;
        for(const auto& f:p.eyeFov){
            v=std::fmax(v,std::fabs(std::tan(f.angleUp)));v=std::fmax(v,std::fabs(std::tan(f.angleDown)));
            h=std::fmax(h,std::fabs(std::tan(f.angleLeft)));h=std::fmax(h,std::fabs(std::tan(f.angleRight)));
        }
        fov=calibration.RequiredFov(h*1.001f,v*1.001f,aspect);
    }
    // UpdateMatrices multiplies the horizontal/vertical half angles by
    // UViewport +0x98/+0x9c (verified at Engine 104FD034/104FD03E).
    // Its unequal scales waste almost half the horizontal eye resolution.
    // Use the vertical scale on both axes for this scene construction only;
    // the engine still builds culling, inverse and combined matrices itself.
    float* viewportScales=vr&&viewport?reinterpret_cast<float*>(static_cast<unsigned char*>(viewport)+0x98):nullptr;
    float savedX=0;bool normalize=false;
    if(viewportScales){
        savedX=viewportScales[0];
        normalize=std::isfinite(savedX)&&std::isfinite(viewportScales[1])&&savedX>=.25f&&savedX<=2.f&&viewportScales[1]>=.25f&&viewportScales[1]<=2.f;
    }
    void* result=nullptr;
    __try {
        if(normalize)viewportScales[0]=viewportScales[1];
        result=original(self,viewport,target,actor,loc,rot,fov);
    } __finally {
        if(normalize)viewportScales[0]=savedX;
    }
    if(vr){
        // Verified Engine.dll FCameraSceneNode::UpdateMatrices copies the
        // camera projection to +0x98 before deriving inverse/combined matrices.
        const float* m=reinterpret_cast<const float*>(static_cast<const unsigned char*>(self)+0x98);
        projection[0]=m[0];projection[1]=m[5];projection[2]=m[8];projection[3]=m[9];
        projectionEye=eye;projectionTime=p.predictedDisplayTime;projectionPair=RenderSnapshotId();
        calibration.Observe(fov,aspect,m[0],m[5]);
        static unsigned reports=0;
        if(reports++<8)Log("Scene projection eye=%d incomingFOV=%.3f renderFOV=%.3f matrix=(%.6f %.6f %.6f %.6f)",(int)eye,before,fov,m[0],m[5],m[8],m[9]);
    }
    return result;
}
bool InstallSceneViewHook(){
    auto module=GetModuleHandleW(L"Engine.dll");
    auto p=(unsigned char*)GetProcAddress(module,"??0FPlayerSceneNode@@QAE@PAVUViewport@@PAVFRenderTarget@@PAVAActor@@VFVector@@VFRotator@@M@Z");
    if(!p)return false;
    if(p[0]==0xe9){int32_t displacement;std::memcpy(&displacement,p+1,4);p+=5+displacement;}
    const unsigned char expected[]={0x8b,0x44,0x24,0x28,0x8b,0x54,0x24,0x1c};
    if(std::memcmp(p,expected,sizeof(expected))){Log("Scene constructor signature mismatch");return false;}
    if(MH_CreateHook(p,(void*)&Construct,(void**)&original)!=MH_OK)return false;
    if(MH_EnableHook(p)!=MH_OK){MH_RemoveHook(p);original=nullptr;return false;}
    Log("Scene projection hook installed; FOV applied at scene construction");return true;
}
}
