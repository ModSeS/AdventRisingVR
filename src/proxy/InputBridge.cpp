#define DIRECTINPUT_VERSION 0x0800
#include "InputBridge.h"
#include "SharedReader.h"
#include "GameState.h"
#include "UnrealScript.h"
#include "../shared/Locomotion.h"
#include "Log.h"
#include "../shared/MenuKeys.h"
#include "../shared/MovementKeys.h"
#include "../shared/MeleeGesture.h"
#include "../shared/PocketGrenade.h"
#include <dinput.h>
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <MinHook.h>

namespace arvr {
static HANDLE gThread=nullptr;
static volatile LONG gRunning=0;
static SRWLOCK gInputLock=SRWLOCK_INIT;

struct VirtualState {
    bool keys[256]{};
    bool mouseButtons[8]{};
    LONG mouseDX{};
    LONG mouseDY{};
    bool valid{};
    uint32_t movement{};
    ULONGLONG updated{};
};
static VirtualState gVirtual{};

static float IniFloat(const wchar_t*s,const wchar_t*k,float d){
    wchar_t b[64]{};GetPrivateProfileStringW(s,k,L"",b,64,L".\\AdventRisingVR.ini");
    return *b?(float)_wtof(b):d;
}
static int IniInt(const wchar_t*s,const wchar_t*k,int d){return GetPrivateProfileIntW(s,k,d,L".\\AdventRisingVR.ini");}

static void SetKey(VirtualState& s,int dik,bool down){ if(dik>=0 && dik<256) s.keys[dik]=down; }

static void PostMenuEvents(HWND window,const MenuKeyEvents& events){
    if(!window)return;
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);if(pid!=GetCurrentProcessId())return;
    const UINT keys[]={VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_RETURN,VK_ESCAPE,VK_ESCAPE};
    for(unsigned i=0;i<7;++i){
        const uint32_t bit=1u<<i;const UINT key=keys[i];
        LPARAM param=1|(LPARAM(MapVirtualKeyW(key,MAPVK_VK_TO_VSC))<<16);
        if(i<4)param|=LPARAM(1)<<24;
        if(events.up&bit)PostMessageW(window,WM_KEYUP,key,param|LPARAM(0xC0000000u));
        if(events.down&bit)PostMessageW(window,WM_KEYDOWN,key,param);
        if(events.repeat&bit)PostMessageW(window,WM_KEYDOWN,key,param|(LPARAM(1)<<30));
    }
}

using ReadInputFn=void(__thiscall*)(void*,float,void*);
using PreProcessFn=int(__thiscall*)(void*,int,int,float);
using ProcessFn=int(__thiscall*)(void*,void*,int,int,float);
using KeyDownFn=unsigned char(__thiscall*)(void*,int);
static ReadInputFn gReadInput=nullptr;
static PreProcessFn gPreProcess=nullptr;
static ProcessFn gProcess=nullptr;
static KeyDownFn gKeyDown=nullptr;
using FindKeyFn=int(__thiscall*)(const void*,const wchar_t*,int&);
static FindKeyFn gFindKey=nullptr;
static void* gButtonOwner=nullptr;
static bool gButtonsDown[11]{};
static int gButtonKeys[11]{};
static volatile LONG gPocketGripMask=0;
static volatile LONG gButtonPresses=0;
static uint32_t NativeButtonMask(const InputState& input){
    const bool down[]={input.leftTrigger>.35f,input.rightTrigger>.35f,input.leftGrip>.45f,input.rightGrip>.45f,
        (input.buttons&ButtonA)!=0,(input.buttons&ButtonX)!=0,(input.buttons&ButtonY)!=0,(input.buttons&ButtonLStick)!=0,
        input.rightStickY>.65f,input.rightStickY<-.65f,false};
    uint32_t mask=0;for(unsigned i=0;i<11;++i)if(down[i])mask|=1u<<i;return mask;
}
static void ApplyNativeButtons(void* self,void* output,const InputState& input,bool active,float dt,uint32_t presses=0){
    if(self!=gButtonOwner){
        gButtonOwner=self;std::memset(gButtonsDown,0,sizeof(gButtonsDown));
        const wchar_t* names[]={L"LeftMouse",L"RightMouse",L"Q",L"E",L"Space",L"R",L"MouseWheelUp",L"MiddleMouse",L"3",L"MouseWheelDown",L"G"};
        for(unsigned i=0;i<11;++i){gButtonKeys[i]=-1;if(gFindKey&&!gFindKey(self,names[i],gButtonKeys[i]))gButtonKeys[i]=-1;}
    }
    auto desired=NativeButtonMask(input)|presses;
    const auto pocket=(unsigned)InterlockedCompareExchange(&gPocketGripMask,0,0);
    desired&=~((pocket&3)<<2);
    for(unsigned i=0;i<11;++i){
        const int key=gButtonKeys[i];if(key<0||key>255)continue;
        const bool want=active&&(desired&(1u<<i)),physical=gKeyDown(self,key)!=0;
        if(!physical){
            if(want&&!gButtonsDown[i])gProcess(self,output,key,1,0.f);
            if(want&&i<6&&dt>0&&std::isfinite(dt))gProcess(self,output,key,2,dt);
            if(!want&&gButtonsDown[i])gProcess(self,output,key,3,0.f);
        }
        gButtonsDown[i]=want&&!physical;
    }
}
static volatile LONG gMovementPolls=0,gMovementHolds=0;
alignas(8) static volatile LONG64 gMeleeRequest=0;
static bool gMeleeDown=false;static void* gMeleeInput=nullptr;
static ULONGLONG gMeleeReleaseAt=0;
static void ApplyMeleePulse(void* self,void* output,bool active,ULONGLONG now,const Payload* tracking=nullptr){
    if(gMeleeDown){
        if(active&&self==gMeleeInput&&now<gMeleeReleaseAt)return;
        if(self==gMeleeInput)gProcess(self,output,'F',3,0.f);gMeleeDown=false;
    }
    const auto packed=(ULONGLONG)InterlockedExchange64(&gMeleeRequest,0);
    const auto request=packed>>2;
    if(packed&3){
        if(!tracking)active=false;
        else {const auto flags=(packed&3)==1?tracking->left.flags:tracking->right.flags;
            active=active&&(flags&7)==7&&(tracking->head.flags&7)==7&&MeleeGesture::TriggerHeld(*tracking,(unsigned)(packed&3));}
    }
    if(active&&request&&now>=request&&now-request<250&&!gKeyDown(self,'F')&&!(GetAsyncKeyState('F')&0x8000)){
        gProcess(self,output,'F',1,0.f);gMeleeDown=true;gMeleeInput=self;
        gMeleeReleaseAt=now+100;
        Log("Melee: native F press, hold at least 100 ms, source=%s",(packed&3)?"gesture":"button B");
    }
}
static void ApplyNativeAxes(void* self,void* output,MoveAxes axes,uint32_t physicalMask,float dt){
    if(!std::isfinite(dt)||dt<=0.f)return;
    const int keys[]={'W','S','D','A'};
    const float values[]={std::fmax(axes.forward,0.f),std::fmax(-axes.forward,0.f),std::fmax(axes.side,0.f),std::fmax(-axes.side,0.f)};
    for(unsigned i=0;i<4;++i)if(values[i]>.0001f&&!(physicalMask&(1u<<i))&&!gKeyDown(self,keys[i])){
        gProcess(self,output,keys[i],2,dt*values[i]);InterlockedIncrement(&gMovementHolds);
    }
}
static void __fastcall HookReadInput(void* self,void*,float dt,void* output){
    uint32_t physical=0;MoveAxes axes{};
    DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    Payload latest{};int heading=0;float relative=0;bool live=false;
    if(pid==GetCurrentProcessId()&&GameplayFirstPersonActive()&&!ControllerMenuActive()&&(live=ReadBridgePayload(latest))&&latest.sessionState==5&&GetBodyHeading(latest.head,heading,relative)){
        axes=HeadRelativeAxes(latest.input.leftStickX,latest.input.leftStickY,relative,IniFloat(L"Input",L"StickDeadzone",.25f));
    }
    const int keys[]={'W','S','D','A'};
    for(unsigned i=0;i<4;++i)if(GetAsyncKeyState(keys[i])&0x8000)physical|=1u<<i;
    ApplyNativeAxes(self,output,axes,physical,dt);
    const bool active=live&&latest.sessionState==5;
    ApplyNativeButtons(self,output,latest.input,active,dt,(uint32_t)InterlockedExchange(&gButtonPresses,0));
    ApplyMeleePulse(self,output,live&&latest.sessionState==5,GetTickCount64(),&latest);
    InterlockedIncrement(&gMovementPolls);
    // Native ReadInput dispatches action 2 (hold) with dt to the bindings.
    gReadInput(self,dt,output);
}
static bool InstallMovementPoll(){
    auto engine=GetModuleHandleW(L"Engine.dll");if(!engine)return false;
    gPreProcess=(PreProcessFn)GetProcAddress(engine,"?PreProcess@UInput@@UAEHW4EInputKey@@W4EInputAction@@M@Z");
    gProcess=(ProcessFn)GetProcAddress(engine,"?Process@UInput@@UAEHAAVFOutputDevice@@W4EInputKey@@W4EInputAction@@M@Z");
    gKeyDown=(KeyDownFn)GetProcAddress(engine,"?KeyDown@UInput@@QAEEH@Z");
    gFindKey=(FindKeyFn)GetProcAddress(engine,"?FindKeyName@UInput@@UBEHPBGAAW4EInputKey@@@Z");
    auto target=(BYTE*)GetProcAddress(engine,"?ReadInput@UInput@@UAEXMAAVFOutputDevice@@@Z");
    if(!gPreProcess||!gProcess||!gKeyDown||!gFindKey||!target)return false;
    if(*target==0xE9)target+=5+*(int32_t*)(target+1);
    // Verified prologue of this Steam build, preceding a relocatable absolute load.
    if(target[0]!=0x51||target[1]!=0xA1||target[6]!=0x56||target[7]!=0x57)return false;
    auto st=MH_Initialize();if(st!=MH_OK&&st!=MH_ERROR_ALREADY_INITIALIZED)return false;
    if(MH_CreateHook(target,(LPVOID)&HookReadInput,(LPVOID*)&gReadInput)!=MH_OK)return false;
    if(MH_EnableHook(target)!=MH_OK)return false;
    Log("UInput::ReadInput movement hook installed: %p; continuous native hold=2; no synthetic key edges",target);return true;
}

static DWORD WINAPI ThreadProc(void*){
    bool logged=false;
    ULONGLONG last=GetTickCount64();
    bool prevMenu=false;
    bool prevCombatB=false;uint32_t previousButtons=0;
    MenuKeys menuKeys;HWND menuWindow=nullptr;
    MeleeGesture melee;
    PocketGrenade grenade;
    const bool gestureEnabled=IniInt(L"Input",L"GestureMelee",1)!=0;
    const float meleeSpeed=std::clamp(IniFloat(L"Input",L"MeleeSpeed",.9f),.6f,3.f);
    const float meleeDistance=std::clamp(IniFloat(L"Input",L"MeleeDistance",.06f),.05f,.30f);
    while(InterlockedCompareExchange(&gRunning,0,0)){
        DWORD foregroundPid=0;HWND foreground=GetForegroundWindow();GetWindowThreadProcessId(foreground,&foregroundPid);
        Payload p{}; if(foregroundPid!=GetCurrentProcessId() || !ReadBridgePayload(p) || p.sessionState!=5){
            melee.Reset();InterlockedExchange64(&gMeleeRequest,0);
            previousButtons=0;prevCombatB=false;InterlockedExchange(&gButtonPresses,0);
            grenade.Reset();InterlockedExchange(&gPocketGripMask,0);
            PostMenuEvents(menuWindow,menuKeys.Update(0,GetTickCount64()));
            menuWindow=nullptr;
            AcquireSRWLockExclusive(&gInputLock);gVirtual={};ReleaseSRWLockExclusive(&gInputLock);
            last=GetTickCount64();prevMenu=false;Sleep(10);continue;
        }
        if(!logged){Log("Quest controller DirectInput injection state active");logged=true;}
        const ULONGLONG now=GetTickCount64();
        float dt=(now-last)/1000.0f; if(dt<0)dt=0; if(dt>0.1f)dt=0.1f; last=now;
        const float dead=IniFloat(L"Input",L"StickDeadzone",0.25f);
        if(menuWindow!=foreground){PostMenuEvents(menuWindow,menuKeys.Update(0,now));menuWindow=foreground;}
        // Left stick click never opens pause or gates locomotion.
        const bool menuNow=(p.input.buttons&ButtonRStick)!=0;
        // Escape only requests pause; GameDraw reads the actual engine state.
        if(menuNow&&!prevMenu)Log("Controller pause button pressed");
        prevMenu=menuNow;
        // Rendering fallback must not turn movement into menu navigation.
        const bool gameplay=GameplayFirstPersonActive()&&!ControllerMenuActive();
        const bool throwGrenade=grenade.Update(p,gameplay&&IniInt(L"Input",L"PocketGrenade",1));
        InterlockedExchange(&gPocketGripMask,(LONG)grenade.suppress);
        if(throwGrenade){InterlockedOr(&gButtonPresses,1u<<10);Log("Pocket gesture: request standard grenade throw");}
        const auto currentButtons=gameplay?NativeButtonMask(p.input):0;
        InterlockedOr(&gButtonPresses,(LONG)(currentButtons&~previousButtons));previousButtons=currentButtons;
        const bool combatB=gameplay&&(p.input.buttons&ButtonB);
        const auto gestureHand=melee.Update(p,gestureEnabled&&gameplay&&!combatB&&!grenade.suppress&&!throwGrenade,meleeSpeed,meleeDistance);
        if(combatB&&!prevCombatB)InterlockedExchange64(&gMeleeRequest,(LONG64)(now<<2));
        else if(gestureHand)InterlockedExchange64(&gMeleeRequest,(LONG64)((now<<2)|gestureHand));
        prevCombatB=combatB;
        if(!gameplay)InterlockedExchange64(&gMeleeRequest,0);
        uint32_t menuMask=menuNow?(1u<<6):0;

        VirtualState next{}; next.valid=true;next.updated=now;
        next.movement=MovementMask(p.input.leftStickX,p.input.leftStickY,dead,gameplay);
        static ULONGLONG nextReport=0;
        if(now>=nextReport){
            Log("Movement input: stick=(%.3f %.3f) gameplay=%d menu=%d mask=%X nativeReads=%ld holds=%ld",p.input.leftStickX,p.input.leftStickY,gameplay,ControllerMenuActive(),next.movement,InterlockedExchange(&gMovementPolls,0),InterlockedExchange(&gMovementHolds,0));nextReport=now+5000;
        }
        const uint32_t b=p.input.buttons;
        if(gameplay){
            // Buttons and triggers are dispatched once through native UInput;
            // do not also inject them through the DirectInput polling path.

            const float ry=(std::fabs(p.input.rightStickY)>dead)?p.input.rightStickY:0.0f;
            const float speed=IniFloat(L"Input",L"TurnMousePixelsPerSecond",900.0f);
            next.mouseDX=0; // Horizontal turn is integrated once per pose by the VR camera.
            if(IniInt(L"Input",L"RightStickVertical",0)) next.mouseDY=(LONG)std::lround(-ry*speed*dt);
        }else{
            // WinDrv menus consume Windows key events even when a DirectInput
            // keyboard object exists. Target only the foreground game window.
            float sx=(std::fabs(p.input.leftStickX)>dead)?p.input.leftStickX:p.input.rightStickX;
            float sy=(std::fabs(p.input.leftStickY)>dead)?p.input.leftStickY:p.input.rightStickY;
            if(sy>0.55f)menuMask|=1;
            if(sy<-0.55f)menuMask|=2;
            if(sx<-0.55f)menuMask|=4;
            if(sx>0.55f)menuMask|=8;
            if(b&(ButtonA|ButtonX))menuMask|=16;
            if(b&(ButtonB|ButtonY))menuMask|=32;
        }

        PostMenuEvents(menuWindow,menuKeys.Update(menuMask,now));

        AcquireSRWLockExclusive(&gInputLock);
        // Mouse motion is an accumulated relative delta. Preserve any amount not yet consumed.
        next.mouseDX += gVirtual.mouseDX;
        next.mouseDY += gVirtual.mouseDY;
        if(next.mouseDX>200)next.mouseDX=200;if(next.mouseDX<-200)next.mouseDX=-200;
        if(next.mouseDY>200)next.mouseDY=200;if(next.mouseDY<-200)next.mouseDY=-200;
        gVirtual=next;
        ReleaseSRWLockExclusive(&gInputLock);
        Sleep(5);
    }
    PostMenuEvents(menuWindow,menuKeys.Update(0,GetTickCount64()));
    return 0;
}

void ApplyQuestKeyboardState(unsigned char* keys,size_t count){
    if(!keys||count<256) return;
    AcquireSRWLockShared(&gInputLock);
    if(gVirtual.valid){
        for(size_t i=0;i<256;i++) if(gVirtual.keys[i]) keys[i]|=0x80;
    }
    ReleaseSRWLockShared(&gInputLock);
}

void ApplyQuestMouseState(void* state,DWORD bytes){
    if(!state||bytes<sizeof(DIMOUSESTATE)) return;
    AcquireSRWLockExclusive(&gInputLock);
    if(gVirtual.valid){
        if(bytes>=sizeof(DIMOUSESTATE2)){
            DIMOUSESTATE2* m=(DIMOUSESTATE2*)state;
            m->lX+=gVirtual.mouseDX; m->lY+=gVirtual.mouseDY;
            for(int i=0;i<8;i++) if(gVirtual.mouseButtons[i]) m->rgbButtons[i]|=0x80;
        }else{
            DIMOUSESTATE* m=(DIMOUSESTATE*)state;
            m->lX+=gVirtual.mouseDX; m->lY+=gVirtual.mouseDY;
            for(int i=0;i<4;i++) if(gVirtual.mouseButtons[i]) m->rgbButtons[i]|=0x80;
        }
        gVirtual.mouseDX=0;gVirtual.mouseDY=0;
    }
    ReleaseSRWLockExclusive(&gInputLock);
}

void StartControllerInputThread(){
    if(!IniInt(L"Input",L"EnableQuestControllers",1)){Log("Quest controller input disabled by ini");return;}
    if(InterlockedExchange(&gRunning,1)) return;
    if(!InstallMovementPoll())Log("ERROR: UInput movement hook installation failed");
    gThread=CreateThread(nullptr,0,ThreadProc,nullptr,0,nullptr);
    if(gThread) CloseHandle(gThread);
}
}
