#include "UnrealScript.h"
#include "Log.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <MinHook.h>
#include "GameState.h"
#include "../shared/HudState.h"
#include "SharedReader.h"

namespace arvr {
struct FName { int Index{}; };
using FNameCtorFn=FName*(__thiscall*)(FName*,const char*,int);
using FindFunctionFn=void*(__thiscall*)(void*,FName,int);
using ProcessEventFn=void(__thiscall*)(void*,void*,void*,void*);
using IsFirstPersonFn=int(__thiscall*)(void*);

static FNameCtorFn gNameCtor=nullptr;
static FindFunctionFn gFindFunction=nullptr;
static ProcessEventFn gProcessEvent=nullptr;
static IsFirstPersonFn gIsFirstPerson=nullptr;
static bool gInit=false,gReady=false;
static ULONGLONG gLastForceAttempt=0;
static void* gLastPlayer=nullptr;
static bool gLoggedSuccess=false;

static bool Init(){
    if(gInit) return gReady;
    gInit=true;
    HMODULE core=GetModuleHandleW(L"Core.dll");
    HMODULE eon=GetModuleHandleW(L"EonEngine.dll");
    if(!core||!eon) return false;
    gNameCtor=(FNameCtorFn)GetProcAddress(core,"??0FName@@QAE@PBDW4EFindName@@@Z");
    gFindFunction=(FindFunctionFn)GetProcAddress(core,"?FindFunction@UObject@@QAEPAVUFunction@@VFName@@H@Z");
    gProcessEvent=(ProcessEventFn)GetProcAddress(core,"?ProcessEvent@UObject@@UAEXPAVUFunction@@PAX1@Z");
    gIsFirstPerson=(IsFirstPersonFn)GetProcAddress(eon,"?eventIsFirstPerson@AEonCameraSystem@@QAEKXZ");
    gReady=gNameCtor&&gFindFunction&&gProcessEvent&&gIsFirstPerson;
    Log("UnrealScript bridge: %s",gReady?"ready":"exports missing");
    return gReady;
}

using PawnPhysicsRotationFn=void(__thiscall*)(void*,float,FVector);
using ActorGetLevelFn=void*(__thiscall*)(void*);
using LevelMoveFn=int(__thiscall*)(void*,void*,FVector,FRotator,void*,int,int,int,int);
static PawnPhysicsRotationFn gPhysicsRotation=nullptr;
static ActorGetLevelFn gActorGetLevel=nullptr;
static LevelMoveFn gLevelMove=nullptr;
static void* gFacingPawn=nullptr;static int gFacingYaw=0;static ULONGLONG gFacingTime=0;
static bool gFacingHookTried=false,gFacingHookReady=false;
static SRWLOCK gHeadingLock=SRWLOCK_INIT;
static Pose gHeadingOrigin{};static int gHeadingBase=0,gHeadingMovementOffset=0;static float gHeadingScale=1;static ULONGLONG gHeadingTime=0;
void SetBodyTrackingFrame(const Pose& origin,int baseYaw,float yawScale,int movementYawOffset){
    AcquireSRWLockExclusive(&gHeadingLock);
    gHeadingOrigin=origin;gHeadingBase=baseYaw;gHeadingMovementOffset=movementYawOffset;gHeadingScale=yawScale;gHeadingTime=GetTickCount64();
    ReleaseSRWLockExclusive(&gHeadingLock);
}
bool GetBodyHeading(const Pose& head,int& worldYaw,float& relativeRadians){
    if(!(head.flags&PoseOrientationValid))return false;
    AcquireSRWLockShared(&gHeadingLock);
    const Pose origin=gHeadingOrigin;const int base=gHeadingBase;const int movementOffset=gHeadingMovementOffset;const float scale=gHeadingScale;const auto age=GetTickCount64()-gHeadingTime;
    ReleaseSRWLockShared(&gHeadingLock);
    if(age>=500)return false;
    const int relative=(int)(RelativePoseToUnrealRotation(origin,head,1,1,1).Yaw*scale);
    // Camera base already includes stick yaw; native movement axes do not.
    worldYaw=(base+relative)&65535;relativeRadians=(relative+movementOffset)*(6.28318530718f/65536.f);return true;
}
struct RotationHit {unsigned zero[9]{};float time=1.f;int item=-1;void* next=nullptr;};
static_assert(sizeof(RotationHit)==48);
static bool RotatePawnNative(void* pawn,int yaw){
    // Same zero-translation MoveActor call and FCheckResult layout as
    // Engine execSetRotation, but executed during pawn physics, not rendering.
    RotationHit hit{};void* level=gActorGetLevel(pawn);if(!level)return false;
    return gLevelMove(level,pawn,{}, {0,yaw&65535,0},&hit,0,0,0,0)!=0;
}

using PawnCameraRotationFn=int(__thiscall*)(void*,FRotator,FVector*);
static PawnCameraRotationFn gPawnCameraRotation=nullptr;
static bool TrackedPawnHeading(void* pawn,int& heading){
    if(pawn!=gFacingPawn||!gFacingTime||GetTickCount64()-gFacingTime>=250||!GameplayFirstPersonActive()||ControllerMenuActive())return false;
    Payload latest{};float relative=0;
    return ReadBridgePayload(latest)&&latest.sessionState==5&&GetBodyHeading(latest.head,heading,relative);
}
static int __fastcall HookPawnCameraRotation(void* pawn,void*,FRotator rotation,FVector* offset){
    int heading=0;
    if(!TrackedPawnHeading(pawn,heading))return gPawnCameraRotation(pawn,rotation,offset);
    // Eon has an additional first-person weapon/aim pose path outside pawn
    // physics. Give it the same world yaw, not the untracked desktop camera.
    rotation.Yaw=heading;
    const int result=gPawnCameraRotation(pawn,rotation,offset);
    // Preserve its return value and output offset, then align the actor through
    // native movement so attachments see the resulting rotation as well.
    const bool moved=RotatePawnNative(pawn,heading);
    static ULONGLONG report=0;
    if(GetTickCount64()>=report){Log("Weapon camera body yaw=%d moved=%d nativeResult=%d",heading,moved,result);report=GetTickCount64()+5000;}
    return result;
}
static void __fastcall HookPhysicsRotation(void* pawn,void*,float dt,FVector oldVelocity){
    int heading=0;
    if(TrackedPawnHeading(pawn,heading)){
        const bool ok=RotatePawnNative(pawn,heading);
        static ULONGLONG report=0;
        if(GetTickCount64()>=report){Log("Body physics yaw=%d moved=%d",heading,ok);report=GetTickCount64()+5000;}
        if(ok)return;
    }
    gPhysicsRotation(pawn,dt,oldVelocity);
}
bool SetPawnFacingYaw(void* pawn,int yaw){
    if(!pawn)return false;
    if(!gFacingHookTried){
        gFacingHookTried=true;
        auto engine=GetModuleHandleW(L"Engine.dll");
        gActorGetLevel=(ActorGetLevelFn)GetProcAddress(engine,"?GetLevel@AActor@@QBEPAVULevel@@XZ");
        gLevelMove=(LevelMoveFn)GetProcAddress(engine,"?MoveActor@ULevel@@UAEHPAVAActor@@VFVector@@VFRotator@@AAUFCheckResult@@HHHH@Z");
        auto target=(BYTE*)GetProcAddress(engine,"?physicsRotation@APawn@@UAEXMVFVector@@@Z");
        auto weaponTarget=(BYTE*)GetProcAddress(GetModuleHandleW(L"EonEngine.dll"),"?UpdatePawnFromCameraRotation@AEonPawn@@UAEHVFRotator@@PAVFVector@@@Z");
        if(!target||!weaponTarget||!gActorGetLevel||!gLevelMove)return false;
        if(*weaponTarget==0xE9)weaponTarget+=5+*(int32_t*)(weaponTarget+1);
        const BYTE weaponSeh[]={0x64,0xa1,0,0,0,0,0x50};
        if(weaponTarget[0]!=0x6a||weaponTarget[1]!=0xff||weaponTarget[2]!=0x68||memcmp(weaponTarget+7,weaponSeh,sizeof(weaponSeh)))return false;
        if(*target==0xE9)target+=5+*(int32_t*)(target+1);
        const BYTE prefix[]={0x83,0xEC,0x68,0x53,0x55,0x56};
        if(memcmp(target,prefix,sizeof(prefix)))return false;
        auto st=MH_Initialize();if(st!=MH_OK&&st!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,(LPVOID)&HookPhysicsRotation,(LPVOID*)&gPhysicsRotation)!=MH_OK)return false;
        if(MH_CreateHook(weaponTarget,(LPVOID)&HookPawnCameraRotation,(LPVOID*)&gPawnCameraRotation)!=MH_OK){MH_RemoveHook(target);return false;}
        if(MH_EnableHook(target)!=MH_OK||MH_EnableHook(weaponTarget)!=MH_OK){
            MH_DisableHook(target);MH_DisableHook(weaponTarget);MH_RemoveHook(target);MH_RemoveHook(weaponTarget);return false;
        }
        gFacingHookReady=true;
        Log("Weapon camera rotation hook: ready");
        Log("Body physics rotation hook: %s",gFacingHookReady?"ready":"failed");
    }
    if(!gFacingHookReady)return false;
    gFacingPawn=pawn;gFacingYaw=yaw&65535;gFacingTime=GetTickCount64();return true;
}

void* GetPlayerPawn(void* playerController){
    if(!playerController) return nullptr;
    __try { return *(void**)((uint8_t*)playerController+0x254); }
    __except(EXCEPTION_EXECUTE_HANDLER){ return nullptr; }
}

static void* GetCameraSystem(void* playerController){
    if(!playerController) return nullptr;
    __try { return *(void**)((uint8_t*)playerController+0x6E4); }
    __except(EXCEPTION_EXECUTE_HANDLER){ return nullptr; }
}

bool CallNoParamScript(void* object,const char* functionName){
    if(!object||!functionName||!Init()) return false;
    __try {
        FName name{}; gNameCtor(&name,functionName,0);
        void* fn=gFindFunction(object,name,1);
        if(!fn) fn=gFindFunction(object,name,0);
        if(!fn){ Log("Script function not found: %s",functionName); return false; }
        gProcessEvent(object,fn,nullptr,nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){
        Log("Script call trapped exception: %s",functionName);
        return false;
    }
}

bool CameraSystemIsFirstPerson(void* playerController){
    if(!Init()) return false;
    void* camera=GetCameraSystem(playerController);
    if(!camera) return false;
    __try { return gIsFirstPerson(camera)!=0; }
    __except(EXCEPTION_EXECUTE_HANDLER){ return false; }
}


// The shipped EonPlayerController.SetCurrentTarget script returns immediately
// when bFirstPerson is true. KeyboardFlick ultimately calls this native event.
// ForceSetCurrentTarget has the same bookkeeping plus bDontPointCamera, and
// TargetObtained updates the native reticle state. Keep the bypass scoped to
// keyboard flick dispatch: other actors and scripted targeting restrictions
// continue through the original event.
using SetTargetEventFn=void(__thiscall*)(void*,void*,unsigned);
using KeyboardFlickFn=void(__thiscall*)(void*,void*,void*);
static SetTargetEventFn gSetTargetEvent=nullptr;
static KeyboardFlickFn gKeyboardFlick=nullptr;
static void* gFocusPlayer=nullptr;
static void* gFocusCamera=nullptr;
static ULONGLONG gFocusSeen=0;
static bool gFocusActive=false,gFocusHookTried=false,gFocusHookReady=false;
static thread_local void* gFlickPlayer=nullptr;
static bool RunFocusScript(void* player,const char* name,void* params){
    if(!player||!Init())return false;
    __try {
        FName n{};gNameCtor(&n,name,0);
        void* fn=gFindFunction(player,n,1);
        if(!fn)return false;
        gProcessEvent(player,fn,params,nullptr);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){Log("VR focus script exception: %s",name);return false;}
}
static bool FocusLive(void* player){
    return player && player==gFocusPlayer && gFocusActive &&
        GetTickCount64()-gFocusSeen<250 && GameplayFirstPersonActive() && !ControllerMenuActive();
}
static void __fastcall HookSetTargetEvent(void* player,void*,void* target,unsigned dontPointCamera){
    if(player==gFlickPlayer && FocusLive(player)){
        struct Params {void* target;unsigned dontPointCamera;} params{target,1};
        if(RunFocusScript(player,"ForceSetCurrentTarget",&params)){
            Log("VR focus: native candidate=%p assigned without camera turn",target);return;
        }
    }
    gSetTargetEvent(player,target,dontPointCamera);
}
static void __fastcall HookKeyboardFlick(void* view,void*,void* frame,void* result){
    void* player=nullptr;
    // UTargetView.Controller, verified in execKeyboardFlick/HandleReticleFlick.
    __try {if(view)player=*(void**)((BYTE*)view+0x18c);}
    __except(EXCEPTION_EXECUTE_HANDLER){player=nullptr;}
    void* previous=gFlickPlayer;
    gFlickPlayer=FocusLive(player)?player:nullptr;
    __try {gKeyboardFlick(view,frame,result);}
    __finally {gFlickPlayer=previous;}
}
using CrosshairLocationFn=void(__thiscall*)(void*,void*,void*);
using AimLocationFn=FVector(__thiscall*)(void*,unsigned);
static CrosshairLocationFn gCrosshairLocation=nullptr;
static AimLocationFn gAimLocation=nullptr;
static bool gAimHookTried=false,gAimHookReady=false;
static void __fastcall HookCrosshairLocation(void* player,void*,void* frame,void* result){
    // Let the native VM handler consume its parameters and provide fallback.
    gCrosshairLocation(player,frame,result);
    if(!result||!FocusLive(player))return;
    __try {
        void* target=*(void**)((BYTE*)player+0x830);
        if(!target||(*(BYTE*)((BYTE*)target+0x34)&0x80))return;
        const FVector aim=gAimLocation(target,0);
        if(!std::isfinite(aim.X)||!std::isfinite(aim.Y)||!std::isfinite(aim.Z))return;
        *(FVector*)result=aim;
        static bool logged=false;if(!logged){Log("VR focus: first-person weapon aim follows selected target");logged=true;}
    } __except(EXCEPTION_EXECUTE_HANDLER){Log("VR focus: aim lookup failed; native aim preserved");}
}
static bool InstallAimHook(){
    if(gAimHookTried)return gAimHookReady;gAimHookTried=true;
    auto target=(BYTE*)GetProcAddress(GetModuleHandleW(L"EonEngine.dll"),"?execGetCrosshairTargetLocation@AEonPlayerController@@QAEXAAUFFrame@@QAX@Z");
    gAimLocation=(AimLocationFn)GetProcAddress(GetModuleHandleW(L"Engine.dll"),"?eventGetAimLocation@AActor@@QAE?AVFVector@@K@Z");
    if(!target||!gAimLocation)return false;
    if(*target==0xe9)target+=5+*(int32_t*)(target+1);
    const BYTE prefix[]={0x8b,0x44,0x24,0x04,0x83,0xec,0x60,0x56,0x57};
    if(memcmp(target,prefix,sizeof(prefix)))return false;
    auto st=MH_Initialize();if(st!=MH_OK&&st!=MH_ERROR_ALREADY_INITIALIZED)return false;
    if(MH_CreateHook(target,(LPVOID)&HookCrosshairLocation,(LPVOID*)&gCrosshairLocation)!=MH_OK)return false;
    if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);return false;}
    gAimHookReady=true;Log("VR focus: first-person aim hook ready");return true;
}
using DamageEventFn=void(__thiscall*)(void*,int,void*,FVector,FVector,void*,void*);
using DamageHealthFn=float(__thiscall*)(void*,unsigned);
static DamageEventFn gDamageEvent=nullptr;static DamageHealthFn gDamageHealth=nullptr;
static CrosshairLocationFn gFireEvent=nullptr;
static bool gDamageHooksTried=false;
static thread_local unsigned gShotId=0;static unsigned gShotSequence=0;
static float DiagnosticHealth(void* actor){
    __try {return gDamageHealth?gDamageHealth(actor,1):-1.f;}
    __except(EXCEPTION_EXECUTE_HANDLER){return -1.f;}
}
static bool DiagnosticLocal(void* instigator){
    return FocusLive(gFocusPlayer)&&instigator&&instigator==GetPlayerPawn(gFocusPlayer);
}
static void __fastcall HookDamageEvent(void* target,void*,int damage,void* instigator,FVector hit,FVector momentum,void* type,void* weapon){
    const bool local=DiagnosticLocal(instigator);
    const float before=local?DiagnosticHealth(target):-1.f;
    gDamageEvent(target,damage,instigator,hit,momentum,type,weapon);
    if(local)Log("VR damage diagnostic: shot=%u hitActor=%p focus=%p amount=%d health=%.4f->%.4f hit=(%.1f %.1f %.1f)",
        gShotId,target,*(void**)((BYTE*)gFocusPlayer+0x830),damage,before,DiagnosticHealth(target),hit.X,hit.Y,hit.Z);
}
static void __fastcall HookFireEvent(void* weapon,void*,void* frame,void* result){
    void* instigator=nullptr;__try{instigator=*(void**)((BYTE*)weapon+0xac);}__except(EXCEPTION_EXECUTE_HANDLER){}
    const bool local=DiagnosticLocal(instigator);const unsigned previous=gShotId;
    if(local){gShotId=++gShotSequence;void* target=*(void**)((BYTE*)gFocusPlayer+0x830);
        Log("VR shot diagnostic: shot=%u weapon=%p focus=%p focusHealth=%.4f",gShotId,weapon,target,target?DiagnosticHealth(target):-1.f);
        if(target){__try{
            const FVector aim=gAimLocation(target,0),head=gAimLocation(target,1),pos=*(FVector*)((BYTE*)target+0x130);
            Log("VR shot geometry: shot=%u aim=(%.1f %.1f %.1f) head=(%.1f %.1f %.1f) actor=(%.1f %.1f %.1f)",gShotId,aim.X,aim.Y,aim.Z,head.X,head.Y,head.Z,pos.X,pos.Y,pos.Z);
        }__except(EXCEPTION_EXECUTE_HANDLER){Log("VR shot geometry unavailable");}}
    }
    __try{gFireEvent(weapon,frame,result);}__finally{gShotId=previous;}
}
static void InstallDamageDiagnostics(){
    if(gDamageHooksTried)return;gDamageHooksTried=true;
    auto engine=GetModuleHandleW(L"Engine.dll"),eon=GetModuleHandleW(L"EonEngine.dll");
    auto damage=(BYTE*)GetProcAddress(engine,"?eventProcessTakeDamage@AActor@@QAEXHPAVAPawn@@VFVector@@1PAVUClass@@PAV1@@Z");
    auto fire=(BYTE*)GetProcAddress(eon,"?execHandleFire@AAdventWeapon@@QAEXAAUFFrame@@QAX@Z");
    gDamageHealth=(DamageHealthFn)GetProcAddress(engine,"?eventGetHealthPercentage@AActor@@QAEMK@Z");
    if(!damage||!fire||!gDamageHealth)return;
    if(*damage==0xe9)damage+=5+*(int32_t*)(damage+1);if(*fire==0xe9)fire+=5+*(int32_t*)(fire+1);
    const BYTE dp[]={0x83,0xec,0x28,0x8b,0x54,0x24,0x34,0x8b,0x44,0x24,0x2c};
    const BYTE fp[]={0x81,0xec,0x98,0,0,0,0x53,0x55,0x56};
    if(memcmp(damage,dp,sizeof(dp))||memcmp(fire,fp,sizeof(fp)))return;
    auto st=MH_Initialize();if(st!=MH_OK&&st!=MH_ERROR_ALREADY_INITIALIZED)return;
    if(MH_CreateHook(damage,(LPVOID)&HookDamageEvent,(LPVOID*)&gDamageEvent)!=MH_OK)return;
    if(MH_CreateHook(fire,(LPVOID)&HookFireEvent,(LPVOID*)&gFireEvent)!=MH_OK){MH_RemoveHook(damage);return;}
    if(MH_EnableHook(damage)!=MH_OK||MH_EnableHook(fire)!=MH_OK){MH_DisableHook(damage);MH_DisableHook(fire);MH_RemoveHook(damage);MH_RemoveHook(fire);return;}
    Log("VR ProcessTakeDamage diagnostic hooks ready; damage and collision behavior unchanged");
}
// The shooting-range bot calls GetCurrentTarget from UnrealScript, not the
// native event wrapper. Intercept the VM dispatch so that exact query sees
// the selected VR target even though the desktop camera never locks onto it.
static CrosshairLocationFn gVirtualFunction=nullptr;
static FName gCurrentTargetQuery{};
static bool gQueryHookTried=false,gQueryHookReady=false;
static void __fastcall HookVirtualFunction(void* object,void*,void* frame,void* result){
    bool query=false;
    if(object==gFocusPlayer&&frame&&result&&gCurrentTargetQuery.Index){
        __try {const BYTE* code=*(const BYTE**)((BYTE*)frame+0xc);query=code&&*(const int*)code==gCurrentTargetQuery.Index;}
        __except(EXCEPTION_EXECUTE_HANDLER){}
    }
    // Always let the VM consume arguments and execute the original function.
    gVirtualFunction(object,frame,result);
    if(!query||!FocusLive(object))return;
    __try {
        void* selected=*(void**)((BYTE*)object+0x830);
        if(selected&&!(*(BYTE*)((BYTE*)selected+0x34)&0x80)){
            *(void**)result=selected;
            static bool logged=false;if(!logged){Log("VR focus: script GetCurrentTarget returns selected actor without camera lock");logged=true;}
        }
    }__except(EXCEPTION_EXECUTE_HANDLER){Log("VR focus: target query override failed; original result retained");}
}
static bool InstallTargetQueryHook(){
    if(gQueryHookTried)return gQueryHookReady;gQueryHookTried=true;
    auto entry=(BYTE*)GetProcAddress(GetModuleHandleW(L"Core.dll"),"?execVirtualFunction@UObject@@QAEXAAUFFrame@@QAX@Z");
    if(!entry)return false;if(*entry==0xe9)entry+=5+*(int32_t*)(entry+1);
    const BYTE prefix[]={0x53,0x56,0x57,0x8b,0x7c,0x24,0x10,0x8b,0x47,0x0c,0x8b,0xf1,0x8b,0x08};
    if(memcmp(entry,prefix,sizeof(prefix)))return false;
    gNameCtor(&gCurrentTargetQuery,"GetCurrentTarget",0);if(!gCurrentTargetQuery.Index)return false;
    auto st=MH_Initialize();if(st!=MH_OK&&st!=MH_ERROR_ALREADY_INITIALIZED)return false;
    if(MH_CreateHook(entry,(LPVOID)&HookVirtualFunction,(LPVOID*)&gVirtualFunction)!=MH_OK)return false;
    if(MH_EnableHook(entry)!=MH_OK){MH_RemoveHook(entry);return false;}
    gQueryHookReady=true;Log("VR focus: scoped script target query hook ready");return true;
}
using RenderReticleFn=void(__thiscall*)(void*,void*);
using DrawReticleFn=void(__thiscall*)(void*,void*,void*);
static RenderReticleFn gRenderReticle=nullptr;
static DrawReticleFn gDrawReticle=nullptr;
static bool gReticleHookTried=false,gReticleHookReady=false;
static void __fastcall HookRenderReticle(void* player,void*,void* canvas){
    // Steam EonEngine RenderTargetingReticle skips DrawReticle in first person.
    // Bypass only that rendering gate for the live local VR player. Keep all
    // gameplay/camera flags intact and use the engine's current FTargetInfo.
    if(canvas&&FocusLive(player)){
        void* view=*(void**)((BYTE*)player+0x888);
        void* info=(BYTE*)player+0x830;
        if(view&&*(void**)info){
            gDrawReticle(view,canvas,info);
            static bool logged=false;if(!logged){Log("VR focus: native selected-target marker rendered");logged=true;}
            return;
        }
    }
    gRenderReticle(player,canvas);
}
static bool InstallReticleHook(){
    if(gReticleHookTried)return gReticleHookReady;
    gReticleHookTried=true;
    auto eon=GetModuleHandleW(L"EonEngine.dll");
    auto render=(BYTE*)GetProcAddress(eon,"?RenderTargetingReticle@AEonPlayerController@@UAEXPAVUCanvas@@@Z");
    auto draw=(BYTE*)GetProcAddress(eon,"?DrawReticle@UTargetView@@UAEXPAVUCanvas@@PAUFTargetInfo@@@Z");
    if(!render||!draw)return false;
    if(*render==0xe9)render+=5+*(int32_t*)(render+1);
    if(*draw==0xe9)draw+=5+*(int32_t*)(draw+1);
    // Validate both the first-person gate and object/FTargetInfo offsets.
    const BYTE prefix[]={0x8b,0x81,0x70,0x07,0,0,0xf6,0xc4,1,0x75,0x1c,
        0x8b,0x81,0x88,0x08,0,0,0x8b,0x10,0x81,0xc1,0x30,0x08,0,0};
    if(memcmp(render,prefix,sizeof(prefix))||draw[0]!=0xa1||draw[5]!=0x81||draw[6]!=0xec)return false;
    auto st=MH_Initialize();if(st!=MH_OK&&st!=MH_ERROR_ALREADY_INITIALIZED)return false;
    gDrawReticle=(DrawReticleFn)draw;
    if(MH_CreateHook(render,(LPVOID)&HookRenderReticle,(LPVOID*)&gRenderReticle)!=MH_OK)return false;
    if(MH_EnableHook(render)!=MH_OK){MH_RemoveHook(render);return false;}
    gReticleHookReady=true;Log("VR focus: native selected-target marker hook ready");return true;
}
static bool InstallFocusHooks(){
    if(gFocusHookTried)return gFocusHookReady;
    gFocusHookTried=true;
    auto engine=GetModuleHandleW(L"Engine.dll"),eon=GetModuleHandleW(L"EonEngine.dll");
    auto event=(BYTE*)GetProcAddress(engine,"?eventSetCurrentTarget@AController@@QAEXPAVAActor@@K@Z");
    auto flick=(BYTE*)GetProcAddress(eon,"?execKeyboardFlick@UTargetView@@QAEXAAUFFrame@@QAX@Z");
    if(!event||!flick)return false;
    if(*event==0xe9)event+=5+*(int32_t*)(event+1);
    if(*flick==0xe9)flick+=5+*(int32_t*)(flick+1);
    const BYTE eventPrefix[]={0x83,0xec,0x08,0x8b,0x44,0x24,0x0c,0x56};
    const BYTE flickPrefix[]={0x51,0x56,0x8b,0x74,0x24,0x0c,0x8b,0x46,0x0c};
    if(memcmp(event,eventPrefix,sizeof(eventPrefix))||memcmp(flick,flickPrefix,sizeof(flickPrefix))){
        Log("VR focus: unsupported native prologue; hooks disabled");return false;
    }
    auto st=MH_Initialize();if(st!=MH_OK&&st!=MH_ERROR_ALREADY_INITIALIZED)return false;
    if(MH_CreateHook(event,(LPVOID)&HookSetTargetEvent,(LPVOID*)&gSetTargetEvent)!=MH_OK)return false;
    if(MH_CreateHook(flick,(LPVOID)&HookKeyboardFlick,(LPVOID*)&gKeyboardFlick)!=MH_OK){MH_RemoveHook(event);return false;}
    if(MH_EnableHook(event)!=MH_OK||MH_EnableHook(flick)!=MH_OK){
        MH_DisableHook(event);MH_DisableHook(flick);MH_RemoveHook(event);MH_RemoveHook(flick);return false;
    }
    gFocusHookReady=true;Log("VR focus: scoped keyboard-flick targeting hooks ready");return true;
}
static void RefreshFocusAim(void* player);
void UpdateVRTargeting(void* player,bool active){
    if(!active||!player){gFocusActive=false;gFocusPlayer=nullptr;gFocusCamera=nullptr;return;}
    if(!Init()||!InstallFocusHooks())return;
    InstallReticleHook();
    InstallAimHook();
    InstallDamageDiagnostics();
    InstallTargetQueryHook();
    void* camera=GetCameraSystem(player);if(!camera){gFocusActive=false;return;}
    const bool enable=!gFocusActive||gFocusPlayer!=player||gFocusCamera!=camera;
    gFocusPlayer=player;gFocusCamera=camera;gFocusSeen=GetTickCount64();
    if(enable){
        // CameraMover.InitController disables both in first-person. Restore
        // through game functions on entry/resume, without changing camera mode.
        unsigned yes=1;
        const bool targeting=RunFocusScript(player,"EnableTargeting",&yes);
        const bool reticle=RunFocusScript(player,"EnableTargetReticle",&yes);
        const bool flick=RunFocusScript(player,"EnableTargetReticleFlick",&yes);
        gFocusActive=targeting&&reticle&&flick;
        Log("VR focus: targeting=%d reticle=%d flick=%d",targeting,reticle,flick);
    }
    RefreshFocusAim(player);
}


using HealthEventFn=float(__thiscall*)(void*,unsigned);
using AmmoEventFn=float(__thiscall*)(void*,int,unsigned,unsigned);
struct EngineString {wchar_t* data;int count,capacity;};
using AimStringFn=void(__thiscall*)(EngineString*,const wchar_t*);
using AimStructFn=int(__thiscall*)(void*,EngineString,FVector**);
static AimStringFn gAimString=nullptr;static AimStructFn gAimStruct=nullptr;
static void* gAimOwner=nullptr;static bool gAimOverridden=false;static FVector* gAimSlot=nullptr;
static void RefreshFocusAim(void* player){
    if(!gAimHookReady||!FocusLive(player))return;
    __try {
        if(gAimOwner!=player){gAimOwner=player;gAimOverridden=false;gAimSlot=nullptr;}
        void* selected=*(void**)((BYTE*)player+0x830);
        if(!selected&&!gAimOverridden)return;
        if(!gAimString||!gAimStruct){
            auto core=GetModuleHandleW(L"Core.dll");
            gAimString=(AimStringFn)GetProcAddress(core,"??0FString@@QAE@PBG@Z");
            gAimStruct=(AimStructFn)GetProcAddress(core,"?FindStructProperty@UObject@@QAEHVFString@@PAPAVUStruct@@@Z");
        }
        if(!gAimString||!gAimStruct)return;
        EngineString name{};
        // Core FindStructProperty returns the instance's struct storage (verified
        // at 0x1016A477), and consumes the by-value FString, like FindObjectProperty.
        if(!gAimSlot){gAimString(&name,L"CurrentAimLocation");if(!gAimStruct(player,name,&gAimSlot)||!gAimSlot)return;}
        FVector location{};
        if(selected&&!(*(BYTE*)((BYTE*)selected+0x34)&0x80))location=gAimLocation(selected,0);
        else if(!RunFocusScript(player,"GetCrosshairTargetLocation",&location))return;
        if(!std::isfinite(location.X)||!std::isfinite(location.Y)||!std::isfinite(location.Z))return;
        *gAimSlot=location;gAimOverridden=selected!=nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER){Log("VR focus: current aim refresh failed");}
}

using StringCtorFn=EngineString*(__thiscall*)(EngineString*,const wchar_t*);
using ObjectPropertyFn=int(__thiscall*)(void*,EngineString,void***);
static HealthEventFn gHudHealth=nullptr;
static AmmoEventFn gHudAmmo=nullptr;
static StringCtorFn gHudString=nullptr;
static ObjectPropertyFn gHudProperty=nullptr;
static bool gHudTried=false,gHudReady=false;
static HudChannel gHudChannel;
static ULONGLONG gHudNext=0;
static int AmmoCount(float value){return std::isfinite(value)&&value>=0&&value<=99999?(int)std::lround(value):-1;}
static void* HudWeapon(void* pawn,const wchar_t* name){
 EngineString text{};gHudString(&text,name);
 void** slot=nullptr;
 // FindObjectProperty takes FString by value and destroys its argument.
 // POD transfers this allocation once; output is a pointer to the property slot.
 if(!gHudProperty(pawn,text,&slot)||!slot)return nullptr;
 return *slot;
}
static HudData ReadHudNative(void* pawn){
 HudData d{};
 __try {
  const float h=gHudHealth(pawn,1);
  if(std::isfinite(h)&&h>=0&&h<=1)d.health=h;
  if(void* left=HudWeapon(pawn,L"LeftWeapon")){
   d.leftClip=AmmoCount(gHudAmmo(left,0,0,0));d.leftReserve=AmmoCount(gHudAmmo(left,0,1,1));
  }
  if(void* right=HudWeapon(pawn,L"RightWeapon")){
   d.rightClip=AmmoCount(gHudAmmo(right,0,0,0));d.rightReserve=AmmoCount(gHudAmmo(right,0,1,1));
  }
  d.active=1;
 } __except(EXCEPTION_EXECUTE_HANDLER){Log("VR HUD: native data read failed");d=HudData{};}
 return d;
}
void UpdateVRHud(void* pawn,bool active){
 if(!active||!pawn){if(gHudNext){HudData empty{};gHudChannel.Write(empty);gHudNext=0;}return;}
 const auto now=GetTickCount64();if(now<gHudNext)return;gHudNext=now+100;
 if(!gHudTried){
  gHudTried=true;auto engine=GetModuleHandleW(L"Engine.dll"),core=GetModuleHandleW(L"Core.dll");
  gHudHealth=(HealthEventFn)GetProcAddress(engine,"?eventGetHealthPercentage@AActor@@QAEMK@Z");
  gHudAmmo=(AmmoEventFn)GetProcAddress(engine,"?eventGetAmmoInClip@AWeaponBase@@QAEMHKK@Z");
  gHudString=(StringCtorFn)GetProcAddress(core,"??0FString@@QAE@PBG@Z");
  gHudProperty=(ObjectPropertyFn)GetProcAddress(core,"?FindObjectProperty@UObject@@QAEHVFString@@PAPAV1@@Z");
  gHudReady=gHudHealth&&gHudAmmo&&gHudString&&gHudProperty;
  Log("VR HUD data exports: %s",gHudReady?"ready":"missing");
 }
 if(!gHudReady)return;
 HudData d=ReadHudNative(pawn);d.timestamp=now;gHudChannel.Write(d);
}

bool EnsureFirstPerson(void* playerController,void* viewActor){
    if(!playerController||!Init()) return false;
    void* pawn=GetPlayerPawn(playerController);
    if(!pawn) return false;
    // During cinematics ViewTarget is normally not the player's Pawn. Do not force
    // first-person there: cutscenes are intentionally handled as a flat VR screen.
    if(viewActor && viewActor!=pawn) return CameraSystemIsFirstPerson(playerController);
    if(CameraSystemIsFirstPerson(playerController)) return true;
    const ULONGLONG now=GetTickCount64();
    if(playerController!=gLastPlayer){ gLastPlayer=playerController; gLastForceAttempt=0; }
    if(now-gLastForceAttempt<1000) return false;
    gLastForceAttempt=now;
    if(CallNoParamScript(playerController,"FirstPerson")){
        if(!gLoggedSuccess){ Log("Requested native Advent Rising first-person camera"); gLoggedSuccess=true; }
    }
    return CameraSystemIsFirstPerson(playerController);
}
static void* gSetBoneDirectionFunction=nullptr;
static void* gSetBoneRotationFunction=nullptr;
static void* gIkTestFunction=nullptr;
static bool gBoneNamesReady=false;
static FName gBoneRightArm{},gBoneRightForeArm{},gBoneRightHand{},gBoneLeftArm{},gBoneLeftForeArm{},gBoneLeftHand{};

static FName* CachedBoneName(const char* name){
    if(!gBoneNamesReady && gNameCtor){
        gNameCtor(&gBoneRightArm,"rightArm",0);
        gNameCtor(&gBoneRightForeArm,"rightForeArm",0);
        gNameCtor(&gBoneRightHand,"righthand",0);
        gNameCtor(&gBoneLeftArm,"leftArm",0);
        gNameCtor(&gBoneLeftForeArm,"leftForeArm",0);
        gNameCtor(&gBoneLeftHand,"lefthand",0);
        gBoneNamesReady=true;
    }
    if(!name) return nullptr;
    if(_stricmp(name,"rightArm")==0) return &gBoneRightArm;
    if(_stricmp(name,"rightForeArm")==0) return &gBoneRightForeArm;
    if(_stricmp(name,"righthand")==0 || _stricmp(name,"rightHand")==0) return &gBoneRightHand;
    if(_stricmp(name,"leftArm")==0) return &gBoneLeftArm;
    if(_stricmp(name,"leftForeArm")==0) return &gBoneLeftForeArm;
    if(_stricmp(name,"lefthand")==0 || _stricmp(name,"leftHand")==0) return &gBoneLeftHand;
    return nullptr;
}

bool SetActorBoneDirection(void* actor,const char* boneName,const FRotator& turn,const FVector& trans,float alpha,int space,int preCalculatedBone){
    if(!actor || !boneName || !Init()) return false;
    __try {
        if(!gSetBoneDirectionFunction){
            FName fnName{}; gNameCtor(&fnName,"SetBoneDirection",0);
            gSetBoneDirectionFunction=gFindFunction(actor,fnName,1);
            if(!gSetBoneDirectionFunction) gSetBoneDirectionFunction=gFindFunction(actor,fnName,0);
            if(!gSetBoneDirectionFunction){ Log("Actor.SetBoneDirection UFunction not found"); return false; }
            Log("Actor.SetBoneDirection ProcessEvent bridge ready");
        }
        FName* bn=CachedBoneName(boneName);
        if(!bn) return false;
        struct Params {
            FName BoneName;
            FRotator BoneTurn;
            FVector BoneTrans;
            float Alpha;
            int Space;
            int preCalculatedBone;
        } params{};
        params.BoneName=*bn;
        params.BoneTurn=turn;
        params.BoneTrans=trans;
        params.Alpha=alpha;
        params.Space=space;
        params.preCalculatedBone=preCalculatedBone;
        gProcessEvent(actor,gSetBoneDirectionFunction,&params,nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){
        Log("Actor.SetBoneDirection ProcessEvent trapped exception for %s",boneName);
        return false;
    }
}


bool SetActorBoneRotation(void* actor,const char* boneName,const FRotator& turn,int space,float alpha,int preCalculatedBone){
    if(!actor || !boneName || !Init()) return false;
    __try {
        if(!gSetBoneRotationFunction){
            FName fnName{}; gNameCtor(&fnName,"SetBoneRotation",0);
            gSetBoneRotationFunction=gFindFunction(actor,fnName,1);
            if(!gSetBoneRotationFunction) gSetBoneRotationFunction=gFindFunction(actor,fnName,0);
            if(!gSetBoneRotationFunction){Log("Actor.SetBoneRotation UFunction not found");return false;}
            Log("Actor.SetBoneRotation ProcessEvent bridge ready");
        }
        FName* bn=CachedBoneName(boneName);
        if(!bn) return false;
        struct Params {
            FName BoneName;
            FRotator BoneTurn;
            int Space;
            float Alpha;
            int preCalculatedBone;
        } params{};
        params.BoneName=*bn;
        params.BoneTurn=turn;
        params.Space=space;
        params.Alpha=alpha;
        params.preCalculatedBone=preCalculatedBone;
        gProcessEvent(actor,gSetBoneRotationFunction,&params,nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){
        Log("Actor.SetBoneRotation ProcessEvent trapped exception for %s",boneName);
        return false;
    }
}

bool PrimeArmIK(void* playerController){
    if(!playerController || !Init()) return false;
    __try {
        if(!gIkTestFunction){
            FName fnName{}; gNameCtor(&fnName,"ikTest",0);
            gIkTestFunction=gFindFunction(playerController,fnName,1);
            if(!gIkTestFunction) gIkTestFunction=gFindFunction(playerController,fnName,0);
            if(!gIkTestFunction){Log("PlayerController.ikTest UFunction not found");return false;}
        }
        struct Params { FName BoneName; } params{};
        gNameCtor(&params.BoneName,"rightArm",0);
        gProcessEvent(playerController,gIkTestFunction,&params,nullptr);
        gNameCtor(&params.BoneName,"leftArm",0);
        gProcessEvent(playerController,gIkTestFunction,&params,nullptr);
        Log("Native Advent Rising ikTest primed arm bone controllers");
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){
        Log("PlayerController.ikTest ProcessEvent trapped exception");
        return false;
    }
}

using EventSetFOVFn=void(__thiscall*)(void*,float,unsigned long,float,float);
static EventSetFOVFn gEventSetFOV=nullptr;
static float gLastFov=-1.0f;

bool SetPlayerFOVNative(void* playerController,float horizontalFovDegrees){
    if(!playerController) return false;
    if(horizontalFovDegrees<60.0f) horizontalFovDegrees=60.0f;
    if(horizontalFovDegrees>155.0f) horizontalFovDegrees=155.0f;
    if(!gEventSetFOV){
        HMODULE eng=GetModuleHandleW(L"Engine.dll");
        if(eng) gEventSetFOV=(EventSetFOVFn)GetProcAddress(eng,"?eventSetFOV@APlayerController@@QAEXMKMM@Z");
        if(!gEventSetFOV){Log("APlayerController::eventSetFOV export missing");return false;}
    }
    __try {
        gEventSetFOV(playerController,horizontalFovDegrees,1,0.0f,0.0f);
        if(std::fabs(gLastFov-horizontalFovDegrees)>0.25f){Log("VR camera FOV set through APlayerController::eventSetFOV: %.2f deg",horizontalFovDegrees);gLastFov=horizontalFovDegrees;}
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){
        Log("APlayerController::eventSetFOV trapped exception");return false;
    }
}

}
