#include "GameState.h"
#include <Windows.h>
namespace arvr {
static volatile LONG gFirstPersonNow=0;
static volatile LONG gControllerMenu=0;
void SetControllerMenu(bool enabled){InterlockedExchange(&gControllerMenu,enabled?1:0);}
bool ControllerMenuActive(){return InterlockedCompareExchange(&gControllerMenu,0,0)!=0;}
static volatile LONG gGameplayLatched=0;
alignas(8) static volatile LONG64 gLastCameraTick=0;
alignas(8) static volatile LONG64 gFlatUntil=0;

void MarkCameraGameplay(bool firstPerson){
    InterlockedExchange(&gFirstPersonNow, firstPerson?1:0);
    InterlockedExchange64(&gLastCameraTick, (LONG64)GetTickCount64());
    // Once actual first-person gameplay has been positively identified, keep the
    // immersive scene selected until a real menu/cutscene explicitly forces flat mode.
    // The old 750 ms timeout could incorrectly drop gameplay back to a head-locked quad.
    if(firstPerson) InterlockedExchange(&gGameplayLatched,1);
}
void ForceFlatOverlayFor(uint32_t milliseconds){
    const LONG64 until=(LONG64)(GetTickCount64()+milliseconds);
    LONG64 old=InterlockedCompareExchange64(&gFlatUntil,0,0);
    while(old<until){
        const LONG64 prev=InterlockedCompareExchange64(&gFlatUntil,until,old);
        if(prev==old) break;
        old=prev;
    }
}
void ClearFlatOverlay(){InterlockedExchange64(&gFlatUntil,0);}
bool GameplayFirstPersonActive(){
    const auto cameraTick=(ULONGLONG)InterlockedCompareExchange64(&gLastCameraTick,0,0);
    return InterlockedCompareExchange(&gFirstPersonNow,0,0)!=0 && cameraTick && GetTickCount64()-cameraTick<500;
}
SceneMode CurrentSceneMode(){
    if(ControllerMenuActive())return SceneFlatScreen;
    const ULONGLONG now=GetTickCount64();
    const ULONGLONG flat=(ULONGLONG)InterlockedCompareExchange64(&gFlatUntil,0,0);
    if(now<flat) return SceneFlatScreen;
    return GameplayFirstPersonActive()?SceneGameplayVR:SceneFlatScreen;
}
}
