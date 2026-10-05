#define DIRECTINPUT_VERSION 0x0800
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cwchar>
#include <initguid.h>
#include <dinput.h>
#include <cstdint>
#include "Log.h"
#include "InputBridge.h"

namespace {
HMODULE gReal=nullptr;
using DirectInput8CreateFn=HRESULT (WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN);
using CreateDeviceFn=HRESULT (STDMETHODCALLTYPE*)(void*,REFGUID,void**,LPUNKNOWN);
using GetDeviceStateFn=HRESULT (STDMETHODCALLTYPE*)(void*,DWORD,LPVOID);
CreateDeviceFn gOrigCreateDevice=nullptr;
SRWLOCK gLock=SRWLOCK_INIT;

enum DevKind:int { DevUnknown=0, DevKeyboard=1, DevMouse=2 };
struct DevEntry { void* dev{}; DevKind kind{}; };
struct VtblEntry { void** vt{}; GetDeviceStateFn orig{}; };
DevEntry gDevices[16]{};
VtblEntry gVtables[8]{};
LONG gDeviceCount=0,gVtableCount=0;
bool gLoggedKeyboard=false,gLoggedMouse=false,gLoggedDI=false;
bool gLoggedKeyboardPoll=false,gLoggedMousePoll=false;

static bool PatchSlot(void** slot,void* hook,void** original){
    if(!slot||!hook)return false;
    DWORD old{}; if(!VirtualProtect(slot,sizeof(void*),PAGE_EXECUTE_READWRITE,&old)) return false;
    if(original)*original=*slot;
    *slot=hook;
    DWORD tmp{};VirtualProtect(slot,sizeof(void*),old,&tmp);FlushInstructionCache(GetCurrentProcess(),slot,sizeof(void*));
    return true;
}

static DevKind LookupKind(void* dev){
    DevKind k=DevUnknown;AcquireSRWLockShared(&gLock);
    for(int i=0;i<gDeviceCount && i<16;i++)if(gDevices[i].dev==dev){k=gDevices[i].kind;break;}
    ReleaseSRWLockShared(&gLock);return k;
}
static GetDeviceStateFn LookupOrig(void* dev){
    if(!dev)return nullptr;void**vt=*reinterpret_cast<void***>(dev);GetDeviceStateFn f=nullptr;
    AcquireSRWLockShared(&gLock);for(int i=0;i<gVtableCount&&i<8;i++)if(gVtables[i].vt==vt){f=gVtables[i].orig;break;}ReleaseSRWLockShared(&gLock);return f;
}

static HRESULT STDMETHODCALLTYPE HookGetDeviceState(void*self,DWORD cbData,LPVOID data){
    GetDeviceStateFn orig=LookupOrig(self); if(!orig)return DIERR_GENERIC;
    HRESULT hr=orig(self,cbData,data); if(FAILED(hr)||!data)return hr;
    DevKind k=LookupKind(self);
    if(k==DevKeyboard && cbData>=256){ if(!gLoggedKeyboardPoll){arvr::Log("Game is polling DirectInput keyboard state");gLoggedKeyboardPoll=true;} arvr::ApplyQuestKeyboardState((unsigned char*)data,cbData); }
    else if(k==DevMouse){ if(!gLoggedMousePoll){arvr::Log("Game is polling DirectInput mouse state");gLoggedMousePoll=true;} arvr::ApplyQuestMouseState(data,cbData); }
    return hr;
}

static void RegisterDevice(void* dev,DevKind kind){
    if(!dev||kind==DevUnknown)return;
    void**vt=*reinterpret_cast<void***>(dev);if(!vt)return;
    AcquireSRWLockExclusive(&gLock);
    bool haveDev=false;for(int i=0;i<gDeviceCount&&i<16;i++)if(gDevices[i].dev==dev){gDevices[i].kind=kind;haveDev=true;break;}
    if(!haveDev && gDeviceCount<16){gDevices[gDeviceCount++]={dev,kind};}
    bool haveVt=false;for(int i=0;i<gVtableCount&&i<8;i++)if(gVtables[i].vt==vt){haveVt=true;break;}
    if(!haveVt && gVtableCount<8){
        void* original=nullptr;
        if(vt[9]==reinterpret_cast<void*>(&HookGetDeviceState)){
            // A previously registered device may share the same vtable.
            haveVt=true;
        }else if(PatchSlot(&vt[9],reinterpret_cast<void*>(&HookGetDeviceState),&original)){
            gVtables[gVtableCount++]={vt,reinterpret_cast<GetDeviceStateFn>(original)};
            haveVt=true;
        }
    }
    ReleaseSRWLockExclusive(&gLock);
    if(kind==DevKeyboard&&!gLoggedKeyboard){arvr::Log("DirectInput keyboard GetDeviceState injection installed");gLoggedKeyboard=true;}
    if(kind==DevMouse&&!gLoggedMouse){arvr::Log("DirectInput mouse GetDeviceState injection installed");gLoggedMouse=true;}
}

static HRESULT STDMETHODCALLTYPE HookCreateDevice(void*self,REFGUID guid,void**outDev,LPUNKNOWN outer){
    HRESULT hr=gOrigCreateDevice?gOrigCreateDevice(self,guid,outDev,outer):DIERR_GENERIC;
    if(SUCCEEDED(hr)&&outDev&&*outDev){
        DevKind kind=DevUnknown;
        if(IsEqualGUID(guid,GUID_SysKeyboard))kind=DevKeyboard;
        else if(IsEqualGUID(guid,GUID_SysMouse))kind=DevMouse;
        RegisterDevice(*outDev,kind);
    }
    return hr;
}

static void HookDirectInputObject(void* obj){
    if(!obj||gOrigCreateDevice)return;void**vt=*reinterpret_cast<void***>(obj);if(!vt)return;
    void* orig=nullptr;if(PatchSlot(&vt[3],reinterpret_cast<void*>(&HookCreateDevice),&orig)){
        gOrigCreateDevice=reinterpret_cast<CreateDeviceFn>(orig);
        if(!gLoggedDI){arvr::Log("IDirectInput8::CreateDevice hook installed");gLoggedDI=true;}
    }
}
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE a,DWORD b,REFIID c,LPVOID*d,LPUNKNOWN e){
    if(!gReal){wchar_t p[MAX_PATH]{};GetSystemDirectoryW(p,MAX_PATH);wcscat_s(p,_countof(p),L"\\dinput8.dll");gReal=LoadLibraryW(p);arvr::Log("Loaded real dinput8: %p",gReal);}
    auto f=gReal?(DirectInput8CreateFn)GetProcAddress(gReal,"DirectInput8Create"):nullptr;
    HRESULT hr=f?f(a,b,c,d,e):E_FAIL;
    if(SUCCEEDED(hr)&&d&&*d)HookDirectInputObject(*d);
    return hr;
}
