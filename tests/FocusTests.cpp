#include "../src/proxy/UnrealScript.cpp"
#include <cstdio>
#include <cstdlib>
namespace arvr {
static bool menu=false,gameplay=true;
void Log(const char*,...){}
bool ReadBridgePayload(Payload&){return false;}
bool GameplayFirstPersonActive(){return gameplay;}
bool ControllerMenuActive(){return menu;}
}
using namespace arvr;
static unsigned enables=0,assigned=0,original=0,originalCameraFlag=0;
static void* assignedTarget=nullptr;
static bool missing=false;
static unsigned char player[0x900]{},view[0x200]{};
static void check(bool b){if(!b){puts("FAIL focus contract");std::exit(1);}}
static FName* __fastcall Name(FName* n,void*,const char* s,int){
 n->Index=!strcmp(s,"GetCrosshairTargetLocation")?5:!strcmp(s,"ForceSetCurrentTarget")?4:!strcmp(s,"EnableTargeting")?1:!strcmp(s,"EnableTargetReticle")?2:3;return n;
}
static void* __fastcall Find(void*,void*,FName n,int){return missing?nullptr:(void*)(uintptr_t)n.Index;}
static void __fastcall Process(void* p,void*,void* f,void* args,void*){
 check(p==player);
 if((uintptr_t)f==5){*(FVector*)args={50,60,70};return;}
 if((uintptr_t)f<4){check(*(unsigned*)args==1);++enables;}
 else {struct P{void* target;unsigned dontPointCamera;};auto q=(P*)args;check(q->dontPointCamera==1);assignedTarget=q->target;++assigned;}
}
static void __fastcall Original(void*,void*,void*,unsigned camera){++original;originalCameraFlag=camera;}
static void __fastcall Flick(void* v,void*,void*,void*){
 HookSetTargetEvent(*(void**)((BYTE*)v+0x18c),nullptr,(void*)123,0);
}
static unsigned draws=0,renderFallback=0;
static void __fastcall Draw(void* v,void*,void* canvas,void* info){
 check(v==view&&canvas==(void*)42&&info==player+0x830);
 check(*(unsigned*)(player+0x770)==0x12345678);++draws;
}
static void __fastcall Render(void*,void*,void*){++renderFallback;}
static FVector aimStorage{};
static void __fastcall AimString(EngineString*,void*,const wchar_t*){}
static int __fastcall AimStruct(void*,void*,EngineString,FVector** out){*out=&aimStorage;return 1;}
static unsigned aimCalls=0,nativeAimCalls=0;static FVector aimPoint{100,200,300};
static void __fastcall Crosshair(void*,void*,void*,void* result){++nativeAimCalls;if(result)*(FVector*)result={1,2,3};}
static FVector __fastcall Aim(void*,void*,unsigned alternate){check(alternate==0);++aimCalls;return aimPoint;}
static int damageCalls=0;
static void __fastcall Damage(void* target,void*,int amount,void* instigator,FVector hit,FVector momentum,void* type,void* weapon){
 check(target==(void*)123&&amount==17&&instigator==(void*)789&&hit.X==1&&momentum.Y==5&&type==(void*)456&&weapon==(void*)987);++damageCalls;
}
static unsigned virtualCalls=0;static void* ordinaryTarget=(void*)444;
static void __fastcall Virtual(void*,void*,void* frame,void* result){
 ++virtualCalls;*(BYTE**)((BYTE*)frame+0xc)+=4;if(result)*(void**)result=ordinaryTarget;
}
int main(int argc,char** argv){
 if(argc==2){
  SetDllDirectoryA(argv[1]);
  if(!LoadLibraryA("Engine.dll")||!LoadLibraryA("EonEngine.dll")){printf("DLL load failed: %lu\n",GetLastError());return 2;}
  if(!Init()||!InstallFocusHooks()){puts("Native focus hook installation failed");return 3;}
  puts("PASS: both focus hooks installed in real game DLLs; no gameplay objects executed");return 0;
 }
 gQueryHookTried=gQueryHookReady=true;gDamageHooksTried=true;gAimHookTried=gAimHookReady=true;gReticleHookTried=gReticleHookReady=true;gDrawReticle=(DrawReticleFn)&Draw;gRenderReticle=(RenderReticleFn)&Render;
 gInit=gReady=true;gFocusHookTried=gFocusHookReady=true;
 gNameCtor=(FNameCtorFn)&Name;gFindFunction=(FindFunctionFn)&Find;gProcessEvent=(ProcessEventFn)&Process;
 gSetTargetEvent=(SetTargetEventFn)&Original;gKeyboardFlick=(KeyboardFlickFn)&Flick;
 *(void**)(player+0x6e4)=(void*)456;*(void**)(view+0x18c)=player;
 UpdateVRTargeting(player,true);check(enables==3 && gFocusActive);
 UpdateVRTargeting(player,true);check(enables==3); // No repeated enable calls while playing.
 HookKeyboardFlick(view,nullptr,nullptr,nullptr);check(assigned==1 && original==0 && assignedTarget==(void*)123 && !gFlickPlayer);
 HookSetTargetEvent(player,nullptr,(void*)123,0);check(original==1); // Script/AI outside flick is preserved.
 gFlickPlayer=player;HookSetTargetEvent((void*)789,nullptr,(void*)123,7);check(original==2 && originalCameraFlag==7);gFlickPlayer=nullptr;
 menu=true;HookKeyboardFlick(view,nullptr,nullptr,nullptr);check(original==3 && assigned==1);menu=false;
 gameplay=false;HookKeyboardFlick(view,nullptr,nullptr,nullptr);check(original==4);gameplay=true;
 gFocusSeen=GetTickCount64()-300;HookKeyboardFlick(view,nullptr,nullptr,nullptr);check(original==5);
 UpdateVRTargeting(nullptr,false);HookKeyboardFlick(view,nullptr,nullptr,nullptr);check(original==6);
 UpdateVRTargeting(player,true);check(enables==6);
 missing=true;HookKeyboardFlick(view,nullptr,nullptr,nullptr);check(original==7 && assigned==1);missing=false;
 gFlickPlayer=player;HookKeyboardFlick(view,nullptr,nullptr,nullptr);check(gFlickPlayer==player);gFlickPlayer=nullptr;
 check(assigned==2);
 *(void**)(player+0x888)=view;*(void**)(player+0x830)=(void*)123;
 *(unsigned*)(player+0x770)=0x12345678;
 HookRenderReticle(player,nullptr,(void*)42);HookRenderReticle(player,nullptr,(void*)42);check(draws==2&&renderFallback==0);
 check(*(unsigned*)(player+0x770)==0x12345678);
 *(void**)(player+0x830)=(void*)456;HookRenderReticle(player,nullptr,(void*)42);check(draws==3);
 *(void**)(player+0x830)=nullptr;HookRenderReticle(player,nullptr,(void*)42);check(renderFallback==1);
 *(void**)(player+0x830)=(void*)123;*(void**)(player+0x888)=nullptr;HookRenderReticle(player,nullptr,(void*)42);check(renderFallback==2);
 *(void**)(player+0x888)=view;menu=true;HookRenderReticle(player,nullptr,(void*)42);menu=false;
 HookRenderReticle((void*)789,nullptr,(void*)42);HookRenderReticle(player,nullptr,nullptr);
 gFocusSeen=GetTickCount64()-300;HookRenderReticle(player,nullptr,(void*)42);
 check(draws==3&&renderFallback==6);
 gCrosshairLocation=(CrosshairLocationFn)&Crosshair;gAimLocation=(AimLocationFn)&Aim;
 unsigned char enemy[64]{};*(void**)(player+0x830)=enemy;gFocusSeen=GetTickCount64();
 FVector shot{};HookCrosshairLocation(player,nullptr,nullptr,&shot);check(shot.X==100&&shot.Y==200&&shot.Z==300&&aimCalls==1);
 aimPoint={-300,50,10};HookCrosshairLocation(player,nullptr,nullptr,&shot);check(shot.X==-300&&aimCalls==2);
 enemy[0x34]=0x80;HookCrosshairLocation(player,nullptr,nullptr,&shot);check(shot.X==1&&aimCalls==2);enemy[0x34]=0;
 *(void**)(player+0x830)=nullptr;HookCrosshairLocation(player,nullptr,nullptr,&shot);check(shot.X==1&&aimCalls==2);
 *(void**)(player+0x830)=enemy;menu=true;HookCrosshairLocation(player,nullptr,nullptr,&shot);check(shot.X==1&&aimCalls==2);menu=false;
 HookCrosshairLocation((void*)789,nullptr,nullptr,&shot);check(shot.X==1&&aimCalls==2);
 gFocusSeen=GetTickCount64()-300;HookCrosshairLocation(player,nullptr,nullptr,&shot);check(shot.X==1&&aimCalls==2);
 check(nativeAimCalls==7);
 gDamageEvent=(DamageEventFn)&Damage;
 HookDamageEvent((void*)123,nullptr,17,(void*)789,{1,2,3},{4,5,6},(void*)456,(void*)987);check(damageCalls==1);

 gAimString=(AimStringFn)&AimString;gAimStruct=(AimStructFn)&AimStruct;gFocusSeen=GetTickCount64();
 RefreshFocusAim(player);check(aimStorage.X==-300&&gAimOverridden);
 *(void**)(player+0x830)=nullptr;aimStorage={};RefreshFocusAim(player);check(aimStorage.X==50&&!gAimOverridden);
 aimStorage={};RefreshFocusAim(player);check(aimStorage.X==0);

 // Reproduce the training bot's GetCurrentTarget()==self check with a
 // camera that has no matching target: original result null, VR query selected.
 gVirtualFunction=(CrosshairLocationFn)&Virtual;gCurrentTargetQuery={77};
 unsigned char vmFrame[32]{};int functionName=77;void* answer=nullptr;
 auto query=[&](void* obj){*(int**)(vmFrame+0xc)=&functionName;HookVirtualFunction(obj,nullptr,vmFrame,&answer);check(*(BYTE**)(vmFrame+0xc)==(BYTE*)&functionName+4);};
 *(void**)(player+0x830)=enemy;gFocusSeen=GetTickCount64();ordinaryTarget=nullptr;
 query(player);check(answer==enemy); // Training bot now accepts its own target.
 check(*(unsigned*)(player+0x770)==0x12345678);
 ordinaryTarget=(void*)444;functionName=78;query(player);check(answer==ordinaryTarget);functionName=77;
 query((void*)789);check(answer==ordinaryTarget);
 menu=true;query(player);check(answer==ordinaryTarget);menu=false;
 gameplay=false;query(player);check(answer==ordinaryTarget);gameplay=true;
 gFocusSeen=GetTickCount64()-300;query(player);check(answer==ordinaryTarget);gFocusSeen=GetTickCount64();
 enemy[0x34]=0x80;query(player);check(answer==ordinaryTarget);enemy[0x34]=0;
 *(void**)(player+0x830)=nullptr;query(player);check(answer==ordinaryTarget); // Preserve crosshair fallback.
 unsigned char nextEnemy[64]{};*(void**)(player+0x830)=nextEnemy;query(player);check(answer==nextEnemy);
 check(virtualCalls==9);
 puts("PASS: VM target query matches selected VR enemy, preserves camera flags, consumes arguments, target switch/drop and scoped fallback; game calls mocked");
 puts("PASS: live focus aim point, moving target, drop/deleted target and scoped native fallback; engine mocked");
 puts("PASS: selected target render for both eyes, target change/drop, missing view, menu, stale context and actor isolation; flags preserved; engine mocked");
 puts("PASS: scoped native focus dispatch, no camera turn, actor isolation, menu/session expiry, resume and fallback; game calls mocked");
}

