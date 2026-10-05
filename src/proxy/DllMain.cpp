#include <Windows.h>
#include <cwchar>
#include "CameraHook.h"
#include "SharedReader.h"
#include "Log.h"
#include "D3D9Hook.h"
#include "GameDrawHook.h"
#include "InputBridge.h"

static LONG CALLBACK CaptureCrashContext(EXCEPTION_POINTERS* e){
    static volatile LONG count=0;
    if(!e || !e->ExceptionRecord || !e->ContextRecord)return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code=e->ExceptionRecord->ExceptionCode;
    if(code!=EXCEPTION_ACCESS_VIOLATION && code!=EXCEPTION_ILLEGAL_INSTRUCTION && code!=EXCEPTION_STACK_OVERFLOW)return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedIncrement(&count)>4)return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod=nullptr;char path[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)e->ExceptionRecord->ExceptionAddress,&mod);
    if(mod)GetModuleFileNameA(mod,path,MAX_PATH);
    arvr::Log("EXCEPTION code=%08lX address=%p module=%s offset=%08lX eax=%08lX ecx=%08lX edx=%08lX esp=%08lX ebp=%08lX access=%lu target=%p",
        code,e->ExceptionRecord->ExceptionAddress,path,(DWORD)((BYTE*)e->ExceptionRecord->ExceptionAddress-(BYTE*)mod),
        e->ContextRecord->Eax,e->ContextRecord->Ecx,e->ContextRecord->Edx,e->ContextRecord->Esp,e->ContextRecord->Ebp,
        (unsigned long)e->ExceptionRecord->ExceptionInformation[0],(void*)e->ExceptionRecord->ExceptionInformation[1]);
    void* frames[24]{};USHORT n=CaptureStackBackTrace(0,24,frames,nullptr);
    for(USHORT i=0;i<n;i++){
        HMODULE frameModule=nullptr;char framePath[MAX_PATH]{};
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)frames[i],&frameModule);
        if(frameModule)GetModuleFileNameA(frameModule,framePath,MAX_PATH);
        arvr::Log("EXCEPTION stack[%u]=%p module=%s offset=%08lX",i,frames[i],framePath,(DWORD)((BYTE*)frames[i]-(BYTE*)frameModule));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI Worker(void*) {
    AddVectoredExceptionHandler(1,&CaptureCrashContext);
    arvr::Log("AdventRisingVR proxy init (v0.8.30-dev), pid=%lu", GetCurrentProcessId());


    const bool autoLaunch = GetPrivateProfileIntW(L"Bridge", L"AutoLaunch", 1, L".\\AdventRisingVR.ini") != 0;
    const bool installCamera = GetPrivateProfileIntW(L"Debug", L"InstallCameraHook", 1, L".\\AdventRisingVR.ini") != 0;
    const bool installCapture = GetPrivateProfileIntW(L"Stereo", L"InstallCaptureHook", 1, L".\\AdventRisingVR.ini") != 0;
    const int stereoMode = GetPrivateProfileIntW(L"Stereo", L"Mode", 2, L".\\AdventRisingVR.ini");
    arvr::Log("Startup config: camera=%d capture=%d stereoMode=%d", installCamera?1:0, installCapture?1:0, stereoMode);

    if (installCapture) {
        const bool d3d = arvr::InstallD3D9Hooks();
        arvr::Log("D3D9 capture hook result: %s", d3d ? "installed/ready" : "failed");
    }

    const bool bridgeReady = arvr::EnsureBridge(autoLaunch);
    arvr::Log("Bridge shared memory: %s", bridgeReady ? "ready" : "not available");
    arvr::StartControllerInputThread();

    if (installCamera) {
        arvr::Log("Camera hook requested");
        const bool hooked = arvr::InstallCameraHook();
        arvr::Log("Camera hook result: %s", hooked ? "installed" : "failed");
    }

    if (stereoMode == 2) {
        const bool drawHooked = arvr::InstallGameDrawHook();
        arvr::Log("UGameEngine::Draw stereo hook result: %s", drawHooked ? "installed" : "failed");
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
