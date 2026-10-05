// Optional integration probe: actual Engine.dll key state routines, isolated
// storage, mocked binding execution. Does not instantiate or launch the game.
#include "../src/proxy/InputBridge.cpp"
#include <cstdio>
#include <string>
namespace arvr {
bool ReadBridgePayload(Payload&){return false;}
bool GetBodyHeading(const Pose&,int&,float&){return false;}
bool GameplayFirstPersonActive(){return false;}
bool ControllerMenuActive(){return false;}
void SetControllerMenu(bool){}
void Log(const char* fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);puts("");va_end(a);}
}
static unsigned bindingCalls=0;static float heldSeconds=0;static bool badAction=false;
static int __fastcall Bind(void*,void*,void*,int,int action,float dt){++bindingCalls;heldSeconds+=dt;if(action!=2)badAction=true;return 1;}
int main(int argc,char** argv){
 if(argc!=2)return 1;
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 SetDllDirectoryA(argv[1]);
 std::string path=std::string(argv[1])+"/Engine.dll";
 auto m=LoadLibraryA(path.c_str());if(!m){printf("load error %lu\n",GetLastError());return 2;}
 using namespace arvr;
 if(!InstallMovementPoll())return 3;
 gProcess=reinterpret_cast<ProcessFn>(&Bind);
 alignas(16) unsigned char state[0x1000]{};
 // PreProcess and KeyDown only access key bytes at +0xEB4 in this build.
 for(unsigned i=0;i<100;++i)ApplyNativeAxes(state,nullptr,{1,1},0,.01f);
 if(gKeyDown(state,'W')||gKeyDown(state,'D')||bindingCalls!=200||badAction||std::fabs(heldSeconds-2.f)>.001f)return 4;
 ApplyNativeAxes(state,nullptr,{-1,-1},0,.01f);
 if(gKeyDown(state,'S')||gKeyDown(state,'A')||bindingCalls!=202)return 5;
 ApplyNativeAxes(state,nullptr,{},0,.01f);
 if(bindingCalls!=202)return 6;
 gPreProcess(state,'W',1,0);
 ApplyNativeAxes(state,nullptr,{1,0},1,.01f);ApplyNativeAxes(state,nullptr,{},1,.01f);
 if(!gKeyDown(state,'W')||bindingCalls!=202)return 7;
 ApplyNativeAxes(state,nullptr,{1,1},0,0);
 if(bindingCalls!=202)return 8;
 auto closeFloat=[](float a,float b){return std::fabs(a-b)<.0002f;};
 const float pi=3.14159265359f;
 auto a=HeadRelativeAxes(0,1,pi/2,.25f);if(!closeFloat(a.forward,0)||!closeFloat(a.side,1))return 9;
 a=HeadRelativeAxes(0,1,pi,.25f);if(!closeFloat(a.forward,-1)||!closeFloat(a.side,0))return 10;
 a=HeadRelativeAxes(1,1,0,.25f);if(!closeFloat(a.forward*a.forward+a.side*a.side,1))return 11;
 a=HeadRelativeAxes(.1f,.1f,0,.25f);if(a.forward||a.side)return 12;
 a=HeadRelativeAxes(0,.625f,0,.25f);if(!closeFloat(a.forward,.5f))return 13;
 auto b=HeadRelativeAxes(0,1,-pi+.00001f,.25f);a=HeadRelativeAxes(0,1,pi-.00001f,.25f);
 if(!closeFloat(a.forward,b.forward)||!closeFloat(a.side,b.side))return 14;
 float before=heldSeconds;ApplyNativeAxes(state,nullptr,{0,.25f},0,.02f);
 if(!closeFloat(heldSeconds-before,.005f)||badAction)return 15;
 puts("PASS: continuous hold uses frame dt, no key edges over 100 frames, stops at zero, preserves physical keys; actual Engine KeyDown and detour verified; binding execution mocked");
}

