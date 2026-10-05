#include "UnrealBones.h"
#include "MathVR.h"
#include "Log.h"
#include "GameState.h"
#include "SharedReader.h"
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <MinHook.h>
#include <map>
#include <string>

namespace arvr {
// Advent Rising / UE2 build facts verified from Engine.dll/Core.dll exports:
// AActor::Mesh is at +0xD4 (also visible in AActor::GetBoneCoords machine code).
// FName is one 32-bit index in this build.
struct FName { int Index{}; };
using FNameCtorFn=FName*(__thiscall*)(FName*,const char*,int);
using MeshGetInstanceFn=void*(__thiscall*)(void*,const void*);
using SetBoneDirectionFn=int(__thiscall*)(void*,FName,FRotator,FVector,float,int,int);
using SetBoneRotationFn=int(__thiscall*)(void*,FName,FRotator,int,float,int);

static FNameCtorFn gNameCtor=nullptr;
static MeshGetInstanceFn gMeshGetInstance=nullptr;
static SetBoneDirectionFn gSetBoneDirection=nullptr;
static SetBoneRotationFn gSetBoneRotation=nullptr;
using SetBoneScaleFn=int(__thiscall*)(void*,int,float,FName,int);
static SetBoneScaleFn gSetBoneScale=nullptr;
static FName gHeadBone{};
static bool gInitTried=false,gReady=false,gLogged=false,gDisabledAfterFailure=false;
static SRWLOCK gTargetLock=SRWLOCK_INIT;
static void* gTargetActor=nullptr;
static Payload gTargetPayload{};
static Pose gTargetOrigin{};
static Pose gTargetHead{};
static int gTargetBaseYaw=0;
static FVector gTargetCameraCenter{};
static bool gFrameHookTried=false,gFrameHookReady=false;
static uint64_t gFrameHandCalls=0;
using GetFrameFn=void(__thiscall*)(void*,void*,void*,FVector*,int,int&,unsigned long);
static GetFrameFn gOrigGetFrame=nullptr;
static const char* gFingerText[]={"rightindex1","rightindex2","rightindex3","rightmiddle2","rightmiddle3","rightring1","rightring2","rightring3","rightpinky1","rightpinky2","rightpinky3"};
static FName gFingers[11]{};
static float gFingerAxis[11]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
static FName gRightArm{},gRightForeArm{},gRightHand{},gLeftArm{},gLeftForeArm{},gLeftHand{};
struct BoneCoords {FVector origin,x,y,z;};
using ActorBoneCoordsFn=BoneCoords(__thiscall*)(void*,FName);
static ActorBoneCoordsFn gActorBoneCoords=nullptr;
static thread_local bool gReadingReference=false;
static void* gReferenceActor=nullptr;

static float IniFloat(const wchar_t*s,const wchar_t*k,float d){
    // Bone rendering may call this dozens of times per eye. Settings are
    // loaded once per render thread; restart the game after editing the INI.
    static thread_local std::map<std::wstring,float> cache;
    const std::wstring key=std::wstring(s)+L"/"+k;
    auto found=cache.find(key);if(found!=cache.end())return found->second;
    wchar_t b[64]{};GetPrivateProfileStringW(s,k,L"",b,64,L".\\AdventRisingVR.ini");
    float v=*b?(float)_wtof(b):d;cache.emplace(key,v);return v;
}

static bool ReadablePtr(const void* p){
    if(!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(p,&mbi,sizeof(mbi))) return false;
    if(mbi.State!=MEM_COMMIT) return false;
    const DWORD bad=PAGE_NOACCESS|PAGE_GUARD;
    return (mbi.Protect&bad)==0;
}

static bool InitNativeBones(){
    if(gInitTried) return gReady;
    gInitTried=true;
    HMODULE core=GetModuleHandleW(L"Core.dll");
    HMODULE eng=GetModuleHandleW(L"Engine.dll");
    if(!core||!eng){Log("Native bone API: Core/Engine not loaded");return false;}
    gNameCtor=(FNameCtorFn)GetProcAddress(core,"??0FName@@QAE@PBDW4EFindName@@@Z");
    gMeshGetInstance=(MeshGetInstanceFn)GetProcAddress(eng,"?MeshGetInstance@UMesh@@UAEPAVUMeshInstance@@PBVAActor@@@Z");
    gSetBoneDirection=(SetBoneDirectionFn)GetProcAddress(eng,"?SetBoneDirection@USkeletalMeshInstance@@UAEHVFName@@VFRotator@@VFVector@@MHH@Z");
    gSetBoneRotation=(SetBoneRotationFn)GetProcAddress(eng,"?SetBoneRotation@USkeletalMeshInstance@@UAEHVFName@@VFRotator@@HMH@Z");
    gActorBoneCoords=(ActorBoneCoordsFn)GetProcAddress(eng,"?GetBoneCoords@AActor@@UAE?AVFCoords@@VFName@@@Z");
    gSetBoneScale=(SetBoneScaleFn)GetProcAddress(eng,"?SetBoneScale@USkeletalMeshInstance@@UAEHHMVFName@@H@Z");
    gReady=gNameCtor&&gMeshGetInstance&&gSetBoneDirection&&gSetBoneRotation;
    if(!gReady){Log("Native bone API exports missing: name=%p mesh=%p dir=%p rot=%p",gNameCtor,gMeshGetInstance,gSetBoneDirection,gSetBoneRotation);return false;}
    gNameCtor(&gHeadBone,"head",0);
    // EFindName::FNAME_Find = 0. These names are present in humans.ukx/Engine name tables.
    gNameCtor(&gRightArm,"rightArm",0);gNameCtor(&gRightForeArm,"rightForeArm",0);gNameCtor(&gRightHand,"rightHand",0);
    gNameCtor(&gLeftArm,"leftArm",0);gNameCtor(&gLeftForeArm,"leftForeArm",0);gNameCtor(&gLeftHand,"leftHand",0);
    if(!gRightArm.Index || !gRightForeArm.Index || !gRightHand.Index || !gLeftArm.Index || !gLeftForeArm.Index || !gLeftHand.Index){
        gReady=false;Log("Hand bone names not loaded yet; retry after character loads");gInitTried=false;return false;
    }
    for(unsigned i=0;i<11;++i)gNameCtor(&gFingers[i],gFingerText[i],0);
    Log("Native USkeletalMeshInstance bone API ready: FName=4 bytes MeshOffset=0xD4");
    return true;
}

static FVector Add(FVector a,FVector b){return {a.X+b.X,a.Y+b.Y,a.Z+b.Z};}
static FVector Sub(FVector a,FVector b){return {a.X-b.X,a.Y-b.Y,a.Z-b.Z};}
static FVector MulV(FVector a,float s){return {a.X*s,a.Y*s,a.Z*s};}
static float Dot(FVector a,FVector b){return a.X*b.X+a.Y*b.Y+a.Z*b.Z;}
static FVector Cross(FVector a,FVector b){return {a.Y*b.Z-a.Z*b.Y,a.Z*b.X-a.X*b.Z,a.X*b.Y-a.Y*b.X};}
static float Len(FVector a){return std::sqrt(Dot(a,a));}
static FVector Norm(FVector a){float l=Len(a);return l>0.0001f?MulV(a,1.0f/l):FVector{1,0,0};}
static FRotator RotFromDir(FVector d){
    d=Norm(d);const float yaw=std::atan2(d.Y,d.X);const float pitch=std::atan2(d.Z,std::sqrt(d.X*d.X+d.Y*d.Y));
    const float k=65536.0f/(2.0f*3.14159265358979323846f);return {(int)std::lround(pitch*k),(int)std::lround(yaw*k),0};
}
static FVector HandRelativeToHeadBody(const Pose& originHead,const Pose& currentHead,const Pose& hand,float upm){
    FVector d{hand.px-currentHead.px,hand.py-currentHead.py,hand.pz-currentHead.pz};
    Quat oq{originHead.qx,originHead.qy,originHead.qz,originHead.qw};d=Rotate(Conjugate(oq),d);
    return MulV(FVector{-d.Z,d.X,d.Y},upm);
}

static FVector WorldToBody(FVector delta,int yaw){
    const float a=yaw*(6.28318530718f/65536.0f),c=std::cos(a),s=std::sin(a);
    return {c*delta.X+s*delta.Y,-s*delta.X+c*delta.Y,delta.Z};
}
static FVector ElbowPole(bool right,const Pose& origin,const Pose& head){
    // Hands/shoulders are expressed in the calibrated frame, while the torso
    // follows HMD yaw. Rotate its outward/backward bend preference into that frame.
    const int yaw=RelativePoseToUnrealRotation(origin,head,1,1,1).Yaw;
    return WorldToBody({IniFloat(L"Hands",L"ElbowForwardBias",-.35f),
        (right?1.f:-1.f)*IniFloat(L"Hands",L"ElbowSideBias",1.f),
        IniFloat(L"Hands",L"ElbowDownBias",-.55f)},-yaw);
}
static bool CallArm(void* inst,bool right,const Pose& originHead,const Pose& head,const Pose& hand,float upm,int baseYaw,const FVector* measuredShoulder=nullptr){
    if(!(hand.flags&PosePositionValid)||!(hand.flags&PoseOrientationValid)){
        const FName names[]={right?gRightArm:gLeftArm,right?gRightForeArm:gLeftForeArm,right?gRightHand:gLeftHand};
        for(int i=0;i<3;++i)gSetBoneDirection(inst,names[i],{}, {},0.0f,1,0);
        if(right)for(const auto& finger:gFingers)if(finger.Index)gSetBoneDirection(inst,finger,{}, {},0.f,1,0);
        return false;
    }
    const float side=right?1.0f:-1.0f;
    FVector shoulder{IniFloat(L"Hands",L"ShoulderForward",0.0f),side*IniFloat(L"Hands",L"ShoulderSide",16.0f),IniFloat(L"Hands",L"ShoulderDown",-17.0f)};
    if(measuredShoulder)shoulder=*measuredShoulder;
    FVector target=HandRelativeToHeadBody(originHead,head,hand,upm);
    target=Add(target,{IniFloat(L"Hands",L"HandForwardOffset",0.0f),side*IniFloat(L"Hands",L"HandSideOffset",0.0f),IniFloat(L"Hands",L"HandVerticalOffset",-3.0f)});
    const float l1=IniFloat(L"Hands",L"UpperArmLength",21.77f),l2=IniFloat(L"Hands",L"ForeArmLength",21.73f);
    FVector st=Sub(target,shoulder);float dist=Len(st);if(dist<0.01f)return true;
    const float maxD=(l1+l2)*0.995f,minD=std::fabs(l1-l2)+0.05f;if(dist>maxD)dist=maxD;if(dist<minD)dist=minD;
    FVector dir=Norm(st);target=Add(shoulder,MulV(dir,dist));const float along=(l1*l1-l2*l2+dist*dist)/(2.0f*dist);float h2=l1*l1-along*along;if(h2<0)h2=0;
    FVector pole=ElbowPole(right,originHead,head);FVector perp=Sub(pole,MulV(dir,Dot(pole,dir)));if(Len(perp)<0.01f)perp=Cross(dir,{0,0,1});perp=Norm(perp);
    FVector elbow=Add(Add(shoulder,MulV(dir,along)),MulV(perp,std::sqrt(h2)));
    // Recorded reference skeleton: right segments extend along local -X,
    // left segments along +X. Invert the bone axis, never the tracked target.
    const float boneAxis=right?-1.0f:1.0f;
    FRotator upper=RotFromDir(MulV(Sub(elbow,shoulder),boneAxis)),fore=RotFromDir(MulV(Sub(target,elbow),boneAxis));
    FRotator wrist=RelativePoseToUnrealRotation(originHead,hand,1,1,1);
    // Right hand's longitudinal axis is -X, like its arm bones.
    // Compose a half-turn about controller-local up; keep controller roll.
    if(right){wrist.Pitch=-wrist.Pitch;wrist.Yaw+=32768;wrist.Roll=-wrist.Roll;}
    wrist.Pitch+=(int)IniFloat(L"Hands",right?L"RightHandPitchOffset":L"LeftHandPitchOffset",0.0f);
    wrist.Yaw+=(int)IniFloat(L"Hands",right?L"RightHandYawOffset":L"LeftHandYawOffset",0.0f);
    wrist.Roll+=(int)IniFloat(L"Hands",right?L"RightHandRollOffset":L"LeftHandRollOffset",0.0f);
    upper.Yaw+=baseYaw;fore.Yaw+=baseYaw;wrist.Yaw+=baseYaw;
    FRotator noRot{};FVector zero{};
    const FName& a=right?gRightArm:gLeftArm;const FName& f=right?gRightForeArm:gLeftForeArm;const FName& h=right?gRightHand:gLeftHand;
    // Engine.dll dispatches Space 0,1,2,3,50. Values 4..7 are not
    // controller slots: they require a custom callback. Use world-space (1)
    // consistently, with the same body yaw used by the VR camera.
    const int sa=1,sf=1,sh=1;
    gSetBoneRotation(inst,a,noRot,0,0.0f,0);gSetBoneRotation(inst,f,noRot,0,0.0f,0);gSetBoneRotation(inst,h,noRot,0,0.0f,0);
    const int ra=gSetBoneDirection(inst,a,upper,zero,1.0f,sa,0);
    const int rf=gSetBoneDirection(inst,f,fore,zero,1.0f,sf,0);
    const int rh=gSetBoneDirection(inst,h,wrist,zero,1.0f,sh,0);
    if(right){
        // Straighten finger segments in the wrist basis. Absolute directions
        // override the fist animation; a zero additive rotation would not.
        for(unsigned i=0;i<11;++i)if(gFingers[i].Index){
            FRotator finger=wrist;
            if(gFingerAxis[i]>0){finger.Pitch=-finger.Pitch;finger.Yaw+=32768;finger.Roll=-finger.Roll;}
            gSetBoneRotation(inst,gFingers[i],{},0,0.f,0);
            gSetBoneDirection(inst,gFingers[i],finger,{},1.f,1,0);
        }
    }
    static ULONGLONG nextReport[2]{};
    const unsigned sideIndex=right?1:0;
    if(GetTickCount64()>=nextReport[sideIndex]){
        Log("Hand request %s: trackedFlags=0x%X target=(%.2f %.2f %.2f) shoulder=(%.2f %.2f %.2f) measured=%d boneResults=(%d %d %d) instance=%p",right?"R":"L",hand.flags,target.X,target.Y,target.Z,shoulder.X,shoulder.Y,shoulder.Z,measuredShoulder?1:0,ra,rf,rh,inst);
        nextReport[sideIndex]=GetTickCount64()+5000;
    }
    return ra!=0 && rf!=0 && rh!=0;
}

static bool ApplyToInstance(void* inst,const Payload& now,const Pose& originHead,const Pose& currentHead,int baseYaw,const FVector* rightShoulder=nullptr,const FVector* leftShoulder=nullptr){
    if(!ReadablePtr(inst)) return false;
    const float upm=IniFloat(L"Camera",L"WorldUnitsPerMeter",50.0f);
    const bool r=CallArm(inst,true,originHead,currentHead,now.right,upm,baseYaw,rightShoulder);
    const bool l=CallArm(inst,false,originHead,currentHead,now.left,upm,baseYaw,leftShoulder);
    return r&&l;
}

static void __fastcall HookGetFrame(void* self,void*,void* actor,void* sceneNode,FVector* verts,int stride,int& numVerts,unsigned long flags){
    static thread_local uint64_t appliedPair=0;
    static thread_local void* appliedInstance=nullptr;
    static thread_local void* appliedActor=nullptr;
    const auto pair=RenderSnapshotId();
    void* target=nullptr;Payload p{};Pose origin{},head{};int baseYaw=0;FVector cameraCenter{};
    AcquireSRWLockShared(&gTargetLock);
    target=gTargetActor;p=gTargetPayload;origin=gTargetOrigin;head=gTargetHead;baseYaw=gTargetBaseYaw;cameraCenter=gTargetCameraCenter;
    ReleaseSRWLockShared(&gTargetLock);
    if(actor&&actor==target&&self&&gReady&&!gDisabledAfterFailure&&!gReadingReference && (!pair||pair!=appliedPair||self!=appliedInstance||actor!=appliedActor)){
        __try {
            if(gActorBoneCoords && gReferenceActor!=actor){
                gReferenceActor=actor;gReadingReference=true;
                __try {
                    // Determine longitudinal signs from consecutive joints in
                    // the actual skeleton, even if its animation is a fist.
                    const unsigned starts[]={0,3,5,8},ends[]={2,4,7,10};
                    for(unsigned chain=0;chain<4;++chain){
                        float sign=-1.f;
                        for(unsigned j=starts[chain];j<ends[chain];++j){
                            if(gFingers[j].Index&&gFingers[j+1].Index){
                                auto a=gActorBoneCoords(actor,gFingers[j]);auto b=gActorBoneCoords(actor,gFingers[j+1]);
                                FVector d=Sub(b.origin,a.origin);float dot=Dot(Norm(d),Norm(a.x));
                                if(Len(d)>.01f&&std::fabs(dot)>.7f)sign=dot>0?1.f:-1.f;
                                gFingerAxis[j]=sign;
                            }
                        }
                        gFingerAxis[ends[chain]]=sign;
                    }
                    Log("Right finger extension initialized from skeletal axes");
                    const FName names[]={gRightArm,gRightForeArm,gRightHand,gLeftArm,gLeftForeArm,gLeftHand};
                    for(unsigned i=0;i<6;++i){
                        const BoneCoords c=gActorBoneCoords(actor,names[i]);
                        Log("Hand reference bone=%u origin=(%.3f %.3f %.3f) X=(%.3f %.3f %.3f) Y=(%.3f %.3f %.3f) Z=(%.3f %.3f %.3f)",i,c.origin.X,c.origin.Y,c.origin.Z,c.x.X,c.x.Y,c.x.Z,c.y.X,c.y.Y,c.y.Z,c.z.X,c.z.Y,c.z.Z);
                    }
                } __finally {gReadingReference=false;}
            }
            if(!p.heartbeatMs || GetTickCount64()-p.heartbeatMs>=250 || p.sessionState!=5){p.left={};p.right={};}
            FVector shoulders[2]{};bool measured=false;
            if(gActorBoneCoords){
                gReadingReference=true;
                __try {
                    shoulders[0]=WorldToBody(Sub(gActorBoneCoords(actor,gRightArm).origin,cameraCenter),baseYaw);
                    shoulders[1]=WorldToBody(Sub(gActorBoneCoords(actor,gLeftArm).origin,cameraCenter),baseYaw);
                    static ULONGLONG nextActualReport=0;
                    if(GetTickCount64()>=nextActualReport){
                        FVector wr=WorldToBody(Sub(gActorBoneCoords(actor,gRightHand).origin,cameraCenter),baseYaw);
                        FVector wl=WorldToBody(Sub(gActorBoneCoords(actor,gLeftHand).origin,cameraCenter),baseYaw);
                        Log("Hand observed before update: R=(%.2f %.2f %.2f) L=(%.2f %.2f %.2f) camera=(%.2f %.2f %.2f) yaw=%d",wr.X,wr.Y,wr.Z,wl.X,wl.Y,wl.Z,cameraCenter.X,cameraCenter.Y,cameraCenter.Z,baseYaw);
                        nextActualReport=GetTickCount64()+5000;
                    }
                    measured=std::isfinite(Len(shoulders[0]))&&std::isfinite(Len(shoulders[1]))&&Len(shoulders[0])<200&&Len(shoulders[1])<200;
                } __finally {gReadingReference=false;}
            }
            const bool ok=ApplyToInstance(self,p,origin,head,baseYaw,measured?&shoulders[0]:nullptr,measured?&shoulders[1]:nullptr);
            appliedPair=pair;appliedInstance=self;appliedActor=actor;
            ++gFrameHandCalls;
            if(ok&&!gLogged){Log("Hand bone requests accepted before GetFrame; visible movement still requires validation: instance=%p",self);gLogged=true;}
            if(!ok && gFrameHandCalls<5) Log("Pre-GetFrame hand override returned failure");
        } __except(EXCEPTION_EXECUTE_HANDLER){
            gDisabledAfterFailure=true;Log("Pre-GetFrame hand override trapped exception; disabled for this run");
        }
    }
    const bool localHead=actor==target&&actor&&!gReadingReference&&gSetBoneScale&&gHeadBone.Index;
    if(localHead){
        // Hardware skinning may consume this controller after GetFrame returns.
        // Keep it active throughout VR gameplay; restore on a non-VR view.
        const bool hide=GameplayFirstPersonActive()&&IniFloat(L"Camera",L"HideHead",1.f)!=0;
        gSetBoneScale(self,31,hide?0.f:1.f,gHeadBone,0);
    }
    if(gOrigGetFrame)gOrigGetFrame(self,actor,sceneNode,verts,stride,numVerts,flags);
}

static bool InstallGetFrameHook(){
    if(gFrameHookTried)return gFrameHookReady;gFrameHookTried=true;
    HMODULE eng=GetModuleHandleW(L"Engine.dll");if(!eng)return false;
    void* target=(void*)GetProcAddress(eng,"?GetFrame@USkeletalMeshInstance@@UAEXPAVAActor@@PAVFLevelSceneNode@@PAVFVector@@HAAHK@Z");
    if(!target){Log("USkeletalMeshInstance::GetFrame export missing");return false;}
    MH_STATUS st=MH_Initialize();
    if(st!=MH_OK&&st!=MH_ERROR_ALREADY_INITIALIZED){Log("Hand GetFrame hook: MH_Initialize failed %d",(int)st);return false;}
    st=MH_CreateHook(target,(LPVOID)&HookGetFrame,reinterpret_cast<LPVOID*>(&gOrigGetFrame));
    if(st!=MH_OK&&st!=MH_ERROR_ALREADY_CREATED){Log("Hand GetFrame hook: MH_CreateHook failed %d",(int)st);return false;}
    st=MH_EnableHook(target);
    if(st!=MH_OK&&st!=MH_ERROR_ENABLED){Log("Hand GetFrame hook: MH_EnableHook failed %d",(int)st);return false;}
    gFrameHookReady=true;Log("USkeletalMeshInstance::GetFrame hand hook installed target=%p",target);return true;
}

void UpdateExperimentalHandTarget(void* actor,const Payload& now,const Pose& originHead,const Pose& currentHead,int baseYaw,const FVector& cameraCenter){
    if(!actor||gDisabledAfterFailure||!InitNativeBones())return;
    InstallGetFrameHook();
    AcquireSRWLockExclusive(&gTargetLock);
    gTargetActor=actor;gTargetPayload=now;gTargetOrigin=originHead;gTargetHead=currentHead;gTargetBaseYaw=baseYaw;gTargetCameraCenter=cameraCenter;
    ReleaseSRWLockExclusive(&gTargetLock);
}

void ApplyExperimentalHands(void* actor,const Payload& now,const Pose& originHead,const Pose& currentHead){
    if(!actor||gDisabledAfterFailure||!InitNativeBones())return;
    __try {
        void* mesh=*(void**)((uint8_t*)actor+0xD4);
        if(!ReadablePtr(mesh)){if(!gLogged)Log("Native bone API: actor Mesh pointer invalid (%p)",mesh);return;}
        void* inst=gMeshGetInstance(mesh,actor);
        if(!ReadablePtr(inst)){if(!gLogged)Log("Native bone API: MeshGetInstance returned invalid %p",inst);return;}
        if(!ApplyToInstance(inst,now,originHead,currentHead,0)){Log("Native bone API returned failure");return;}
        if(!gLogged){Log("Quest hand -> native USkeletalMeshInstance IK active: mesh=%p instance=%p",mesh,inst);gLogged=true;}
    } __except(EXCEPTION_EXECUTE_HANDLER){Log("Native USkeletalMeshInstance hand IK trapped exception");}
}

}
