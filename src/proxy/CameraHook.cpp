#include "CameraHook.h"
#include "MathVR.h"
#include "Roomscale.h"
#include "../shared/StickTurn.h"
#include "SharedReader.h"
#include "Log.h"
#include "UnrealBones.h"
#include "StereoState.h"
#include "UnrealScript.h"
#include "GameState.h"
#include "D3D9Hook.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <MinHook.h>
#include "SharedReader.h"
#include <cmath>

namespace arvr {
using OriginalFn=void(__thiscall*)(void*,void*&,FVector&,FRotator&);
static OriginalFn gOriginal=nullptr; static BYTE* gTarget=nullptr; static BYTE gSaved[6]{};
static Pose gOrigin{},gOriginLeft{},gOriginRight{}; static bool gOriginSet=false; static bool gEnabled=true;
static int gReferenceBasePitch=0,gReferenceBaseRoll=0;
static uint64_t gCameraCalls=0; static ULONGLONG gCameraStatStart=0; static int gCameraStatReports=0;
static bool gTrackingWasValid=false;
static bool gWasFirstPerson=false;
using MoveSmoothFn=int(__thiscall*)(void*,FVector);
static MoveSmoothFn gMoveSmooth=nullptr;
static FVector gLastRoomscaleBodyOffset{};
static bool gBodyOffsetValid=false;
static bool gLoggedBodyFollow=false;
static bool gArmControllersPrimed=false;
static FVector gConsumedBodyOffset{};
static RoomscaleTracker gRoomscale;
static StickTurn gStickTurn;static void* gTurnPawn=nullptr;
static bool gLoggedBodyMoveFailure=false;
static ULONGLONG gLastFovSetMs=0;

static int IniInt(const wchar_t*s,const wchar_t*k,int d){return GetPrivateProfileIntW(s,k,d,L".\\AdventRisingVR.ini");}
static float IniFloat(const wchar_t*s,const wchar_t*k,float d){wchar_t b[64]{};GetPrivateProfileStringW(s,k,L"",b,64,L".\\AdventRisingVR.ini");return *b?(float)_wtof(b):d;}


static bool MovePawnRoomscale(void* pawn,const FVector& delta,FVector& actual){
    actual={};
    if(!pawn) return false;
    if(!gMoveSmooth){
        HMODULE e=GetModuleHandleW(L"Engine.dll");
        if(e) gMoveSmooth=(MoveSmoothFn)GetProcAddress(e,"?moveSmooth@AActor@@QAEHVFVector@@@Z");
        if(!gMoveSmooth){Log("Roomscale body follow: AActor::moveSmooth export missing");return false;}
    }
    __try {
        // AActor Location precedes Rotation (0x13c) at 0x130 in this build.
        const auto before=*(const FVector*)((const uint8_t*)pawn+0x130);
        gMoveSmooth(pawn,delta);
        const auto after=*(const FVector*)((const uint8_t*)pawn+0x130);
        actual={after.X-before.X,after.Y-before.Y,after.Z-before.Z};
        if(!std::isfinite(actual.X)||!std::isfinite(actual.Y)||!std::isfinite(actual.Z)){actual={};return false;}
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER){ Log("Roomscale body follow: moveSmooth exception trapped"); return false; }
}

static void __fastcall Hook(void* self,void*,void*& viewActor,FVector& loc,FRotator& rot){
    gOriginal(self,viewActor,loc,rot);
    // Both eyes must share the same unmodified game camera. Calling the
    // engine twice can otherwise advance camera interpolation between eyes.
    static thread_local uint64_t cameraPair=0;
    static thread_local void* cameraOwner=nullptr;
    static thread_local void* pairActor=nullptr;
    static thread_local FVector pairLoc{};
    static thread_local FRotator pairRot{};
    const uint64_t pair=RenderSnapshotId();
    if(pair){
        if(pair!=cameraPair||self!=cameraOwner){cameraPair=pair;cameraOwner=self;pairActor=viewActor;pairLoc=loc;pairRot=rot;}
        else {viewActor=pairActor;loc=pairLoc;rot=pairRot;}
    }
    ++gCameraCalls;
    const ULONGLONG nowMs=GetTickCount64();
    if(!gCameraStatStart) gCameraStatStart=nowMs;
    if(gCameraStatReports<6 && nowMs-gCameraStatStart>=5000){
        const double seconds=(nowMs-gCameraStatStart)/1000.0;
        Log("PlayerCalcView cadence: %llu calls in %.2f s (%.1f calls/s), stereoMode=%d",(unsigned long long)gCameraCalls,seconds,gCameraCalls/seconds,StereoMode());
        gCameraCalls=0;gCameraStatStart=nowMs;++gCameraStatReports;
    }

    if(GetAsyncKeyState(VK_F9)&1){gEnabled=!gEnabled;Log("VR camera %s",gEnabled?"enabled":"disabled");}
    if(GetAsyncKeyState(VK_F8)&1){gOriginSet=false;gBodyOffsetValid=false;Log("Recenter requested");}

    void* pawn=GetPlayerPawn(self);
    bool firstPerson=false;
    if(IniInt(L"Camera",L"ForceFirstPerson",1)) firstPerson=EnsureFirstPerson(self,viewActor);
    else firstPerson=CameraSystemIsFirstPerson(self);
    const bool gameplayTarget=(pawn && (!viewActor || viewActor==pawn));
    if(firstPerson && gameplayTarget && !gWasFirstPerson){gOriginSet=false;gBodyOffsetValid=false;gArmControllersPrimed=false;gConsumedBodyOffset={};ClearFlatOverlay();Log("Entered first-person gameplay; HMD origin will recenter; immersive scene latched");}
    gWasFirstPerson=firstPerson && gameplayTarget;
    MarkCameraGameplay(gWasFirstPerson);
    if(!gameplayTarget || !firstPerson) ForceFlatOverlayFor(250);

    if(!gEnabled || !gWasFirstPerson){gStickTurn.time=0;UpdateVRTargeting(nullptr,false);UpdateVRHud(nullptr,false);return;}
    Payload p{}; if(!ReadBridgePayload(p)){gStickTurn.time=0;UpdateVRTargeting(nullptr,false);UpdateVRHud(nullptr,false);return;}
    UpdateVRHud(pawn,p.sessionState==5 && !ControllerMenuActive() && IniInt(L"HUD",L"Enable",1)!=0);
    UpdateVRTargeting(self,p.sessionState==5 && !ControllerMenuActive() && IniInt(L"Input",L"VRTargetFocus",1)!=0);
    if(StereoMode()==StereoModeDoubleDraw && p.eyeFov[0].valid && p.eyeFov[1].valid){
        const ULONGLONG fovNow=GetTickCount64();
        if(fovNow-gLastFovSetMs>=250){
            float maxV=0.0f;
            for(int i=0;i<2;i++){
                const float vu=std::fabs(std::tan(p.eyeFov[i].angleUp));
                const float vd=std::fabs(std::tan(p.eyeFov[i].angleDown));
                if(vu>maxV)maxV=vu;if(vd>maxV)maxV=vd;
            }
            const float aspect=GetCurrentGameAspect();
            if(aspect>0.1f) for(int i=0;i<2;i++){
                maxV=std::fmax(maxV,std::fabs(std::tan(p.eyeFov[i].angleLeft))/aspect);
                maxV=std::fmax(maxV,std::fabs(std::tan(p.eyeFov[i].angleRight))/aspect);
            }
            if(maxV>0.01f && aspect>0.1f){
                const float hfov=2.0f*std::atan(maxV*aspect)*(180.0f/3.14159265358979323846f);
                SetPlayerFOVNative(self,hfov);
            }
            gLastFovSetMs=fovNow;
        }
    }
    const bool trackingValid=(p.head.flags&PoseOrientationValid)!=0;
    if(!trackingValid){gStickTurn.time=0;gTrackingWasValid=false;gRoomscale.valid=false;return;}
    if(!gTrackingWasValid && IniInt(L"Camera",L"RecenterOnTrackingResume",0)) gOriginSet=false;
    gTrackingWasValid=true;

    AdvanceStereoEye();
    if(!gOriginSet){
        gOrigin=YawOnlyOrigin(p.head);gOriginLeft=p.left;gOriginRight=p.right;gOriginSet=true;
        gRoomscale.Reset(gOrigin,p.head);
        gLastRoomscaleBodyOffset={};gConsumedBodyOffset={};gBodyOffsetValid=true;
        gReferenceBasePitch=rot.Pitch;gReferenceBaseRoll=rot.Roll;
        Log("HMD/controller origin captured; basePitch=%d baseRoll=%d",gReferenceBasePitch,gReferenceBaseRoll);
    }

    if(gTurnPawn!=pawn){gTurnPawn=pawn;gStickTurn={};}
    const int stickYaw=gStickTurn.Update(p,p.sessionState==5&&!ControllerMenuActive(),IniFloat(L"Input",L"TurnDegreesPerSecond",90.f),IniFloat(L"Input",L"StickDeadzone",.25f));
    rot.Yaw+=stickYaw;
    const int baseYaw=rot.Yaw;
    if(IniInt(L"Camera",L"StabilizeGamePitchRoll",1)){
        rot.Pitch=0;
        rot.Roll=0;
    }
    if(IniInt(L"Camera",L"RotationTracking",1)){
        auto rr=RelativePoseToUnrealRotation(gOrigin,p.head,
            IniFloat(L"Camera",L"PitchSign",1),IniFloat(L"Camera",L"YawSign",1),IniFloat(L"Camera",L"RollSign",1));
        const float scale=IniFloat(L"Camera",L"RotationScale",1.0f);
        rot.Pitch+=(int)(rr.Pitch*scale);rot.Yaw+=(int)(rr.Yaw*scale);rot.Roll+=(int)(rr.Roll*scale);
    }
    SetBodyTrackingFrame(gOrigin,baseYaw,IniInt(L"Camera",L"RotationTracking",1)?IniFloat(L"Camera",L"YawSign",1)*IniFloat(L"Camera",L"RotationScale",1):0.f,stickYaw);
    if(IniInt(L"Camera",L"BodyFollowsHeadYaw",1)&&!ControllerMenuActive()&&(p.head.flags&PoseOrientationValid)){
        // Final camera yaw already contains HMD yaw. Queue it for pawn physics;
        // never feed the offset back into PlayerController rotation.
        static uint64_t appliedPair=0;static void* appliedPawn=nullptr;
        if(!pair||pair!=appliedPair||pawn!=appliedPawn){
            const bool ok=SetPawnFacingYaw(pawn,rot.Yaw);
            appliedPair=pair;appliedPawn=pawn;
            static ULONGLONG nextBodyReport=0;
            if(nowMs>=nextBodyReport){Log("Body facing target: yaw=%d queued=%d",rot.Yaw&65535,ok);nextBodyReport=nowMs+5000;}
        }
    }
    const float upm=IniFloat(L"Camera",L"WorldUnitsPerMeter",50.0f);
    if(IniInt(L"Camera",L"PositionTracking",1)&&(p.head.flags&PosePositionValid)){
        auto dp=RelativePoseToUnrealPosition(gOrigin,p.head,upm,baseYaw);
        if(IniInt(L"Camera",L"BodyFollowHMD",1) && pawn){
            if(!gRoomscale.valid || ControllerMenuActive())gRoomscale.Reset(gOrigin,p.head);
            FVector wanted=gRoomscale.Delta(p.head,upm,baseYaw);
            const float jumpLimit=IniFloat(L"Camera",L"BodyFollowMaxStepUnits",40.0f);
            const float minMove=IniFloat(L"Camera",L"BodyFollowMinMoveUnits",0.50f);
            const float d2=wanted.X*wanted.X+wanted.Y*wanted.Y;
            if(d2>jumpLimit*jumpLimit){
                gRoomscale.Consume(p.head);wanted={};
            } else if(!ControllerMenuActive() && (pair || StereoMode()!=StereoModeDoubleDraw) && d2>=minMove*minMove){
                FVector actual{};
                if(MovePawnRoomscale(pawn,wanted,actual)){
                    loc.X+=actual.X;loc.Y+=actual.Y;loc.Z+=actual.Z;
                    // The second eye restores pairLoc, so update that shared
                    // camera base by the ACTUAL collision-limited displacement.
                    if(pair){pairLoc.X+=actual.X;pairLoc.Y+=actual.Y;pairLoc.Z+=actual.Z;}
                    if(!gLoggedBodyFollow){Log("Roomscale body follow active: collision-limited movement, shared stereo camera");gLoggedBodyFollow=true;}
                } else if(!gLoggedBodyMoveFailure){
                    Log("Roomscale body follow native movement unavailable");gLoggedBodyMoveFailure=true;
                }
                // Consume blocked physical steps too: do not accumulate a
                // delayed teleport when the player moves away from a wall.
                gRoomscale.Consume(p.head);wanted={};
            }
            const float maxLean=IniFloat(L"Camera",L"BodyFollowMaxLeanUnits",6.0f);
            const float lean=std::sqrt(wanted.X*wanted.X+wanted.Y*wanted.Y);
            if(lean>maxLean && lean>0.0001f){const float q=maxLean/lean;wanted.X*=q;wanted.Y*=q;}
            loc.X+=wanted.X;loc.Y+=wanted.Y;loc.Z+=dp.Z;
        } else {
            loc.X+=dp.X;loc.Y+=dp.Y;loc.Z+=dp.Z;
        }
    }
    if(!(p.head.flags&PosePositionValid))gRoomscale.valid=false;
    const FVector cameraCenter=loc;
    ApplyStereoEyeOffset(loc,gOrigin,p.head,upm,baseYaw,rot.Yaw);

    if(IniInt(L"Hands",L"Enable",1) && pawn && firstPerson && gameplayTarget){
        // v0.6 uses the exported native UMeshInstance API directly. ProcessEvent/ikTest is not
        // used here because it could report success while animation still overwrote the controllers.
        UpdateExperimentalHandTarget(pawn,p,gOrigin,p.head,baseYaw,cameraCenter);
    }
}

static BYTE* FollowExportThunk(BYTE* p){if(p&&p[0]==0xE9){int32_t rel=*(int32_t*)(p+1);return p+5+rel;}return p;}

bool InstallCameraHook(){
    HMODULE m=nullptr;for(int i=0;i<300 && !(m=GetModuleHandleW(L"EonEngine.dll"));i++)Sleep(100);
    if(!m){Log("EonEngine.dll not loaded");return false;}
    const char* n="?PlayerCalcView@AEonPlayerController@@UAEXAAPAVAActor@@AAVFVector@@AAVFRotator@@@Z";
    BYTE* exp=(BYTE*)GetProcAddress(m,n);if(!exp){Log("PlayerCalcView export not found");return false;}gTarget=FollowExportThunk(exp);
    const BYTE expected[6]={0x64,0xA1,0,0,0,0};if(memcmp(gTarget,expected,6)!=0){Log("Unexpected PlayerCalcView prologue at %p",gTarget);return false;}
    MH_STATUS st=MH_Initialize();
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_INITIALIZED)return false;
    st=MH_CreateHook(gTarget,(LPVOID)&Hook,reinterpret_cast<LPVOID*>(&gOriginal));
    if(st!=MH_OK){Log("Camera/draw hook create failed: %d",(int)st);return false;}
    st=MH_EnableHook(gTarget);
    if(st!=MH_OK){MH_RemoveHook(gTarget);gOriginal=nullptr;return false;}
    Log("PlayerCalcView hook installed with MinHook");return true;
}
}

