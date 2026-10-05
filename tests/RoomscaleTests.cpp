#include "../src/proxy/CameraHook.cpp"
#include <cstdio>
#include <cstdlib>
namespace arvr {
static unsigned char pawnMemory[512]{};static uint64_t testPair=1;
static Payload testPose{};static int testYaw=0;static unsigned moves=0;static float fraction=1;
static FVector& location(){return *(FVector*)(pawnMemory+0x130);}
void Log(const char*,...){}
void UpdateVRTargeting(void*,bool){}
void UpdateVRHud(void*,bool){}
bool ReadBridgePayload(Payload& p){p=testPose;return true;}
uint64_t RenderSnapshotId(){return testPair;}
void* GetPlayerPawn(void*){return pawnMemory;}
bool EnsureFirstPerson(void*,void*){return true;}
bool CameraSystemIsFirstPerson(void*){return true;}
bool SetPlayerFOVNative(void*,float){return true;}
float GetCurrentGameAspect(){return 16.f/9;}
void SetBodyTrackingFrame(const Pose&,int,float,int){}
bool SetPawnFacingYaw(void*,int){return true;}
int StereoMode(){return StereoModeDoubleDraw;}
void AdvanceStereoEye(){}
void ApplyStereoEyeOffset(FVector&,const Pose&,const Pose&,float,int,int){}
void UpdateExperimentalHandTarget(void*,const Payload&,const Pose&,const Pose&,int,const FVector&){}
static void __fastcall camera(void*,void*,void*& actor,FVector& pos,FRotator& rot){actor=pawnMemory;pos=location();pos.Z+=50;rot={0,testYaw,0};}
static int __fastcall move(void*,void*,FVector d){++moves;location().X+=d.X*fraction;location().Y+=d.Y*fraction;return fraction==1;}
}
static void check(bool ok){if(!ok)std::exit(1);}
static arvr::FVector view(){void* actor=nullptr;arvr::FVector p{};arvr::FRotator r{};arvr::Hook((void*)1,nullptr,actor,p,r);return p;}
int main(){using namespace arvr;
 StickTurn turn;Payload input{};input.head.flags=PoseOrientationValid;input.input.rightStickX=1;input.heartbeatMs=1000;
 check(turn.Update(input,true)==0);
 for(int i=0;i<100;++i){input.heartbeatMs+=10;turn.Update(input,true);}
 check(turn.Update(input,true)==16384); // One second = 90 degrees; duplicate eye does not advance.
 input.input.rightStickX=-1;
 for(int i=0;i<100;++i){input.heartbeatMs+=10;turn.Update(input,true);}check(turn.Update(input,true)==0);
 turn.Update(input,false);input.heartbeatMs+=1000;check(turn.Update(input,true)==0);
 input.input.rightStickX=.1f;input.heartbeatMs+=10;check(turn.Update(input,true)==0);

 wchar_t folder[MAX_PATH]{};GetModuleFileNameW(nullptr,folder,MAX_PATH);*wcsrchr(folder,L'\\')=0;SetCurrentDirectoryW(folder);
 WritePrivateProfileStringW(L"Camera",L"BodyFollowHMD",L"1",L".\\AdventRisingVR.ini");
 gOriginal=reinterpret_cast<OriginalFn>(&camera);gMoveSmooth=reinterpret_cast<MoveSmoothFn>(&move);
 location()={100,100,100};testPose.head.flags=3;testPose.head.py=1.7f;testPose.head.qw=1;
 view();check(moves==0);
 ++testPair;testPose.head.pz=-.2f;fraction=.5f;
 auto left=view(),right=view();check(moves==1 && std::fabs(left.X-105)<.001f && std::fabs(right.X-left.X)<.001f);
 ++testPair;testYaw=16384;fraction=1;view();check(moves==1); // no replay on stick turn
 ++testPair;testPose.head.pz=-.3f;left=view();right=view();check(moves==2 && std::fabs(left.Y-105)<.001f && std::fabs(right.Y-left.Y)<.001f);
 ++testPair;testPose.head.pz=-.4f;fraction=0;view();check(moves==3);
 ++testPair;fraction=1;view();check(moves==3); // no catch-up after wall
 ++testPair;testPose.head.pz=-2.f;view();check(moves==3); // tracking jump
 SetControllerMenu(true);++testPair;testPose.head.pz=-2.2f;view();check(moves==3);
 SetControllerMenu(false);++testPair;view();check(moves==3);
 puts("PASS: roomscale camera hook, partial/blocked native movement, identical eye centers, turn without replay, tracking jumps and pause (native movement mocked)");
}
