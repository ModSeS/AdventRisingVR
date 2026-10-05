#include "../src/proxy/UnrealScript.cpp"
#include <cstdio>
#include "../src/shared/Locomotion.h"
#include <initializer_list>
namespace arvr {static Payload pose{};static bool tracked=false,menu=false;
void Log(const char*,...){};bool ReadBridgePayload(Payload& p){p=pose;return tracked;}bool GameplayFirstPersonActive(){return true;}bool ControllerMenuActive(){return menu;}}
static int expected=0;static unsigned calls=0;
static void* __fastcall Level(void*,void*){return (void*)2;}
static int __fastcall Move(void* level,void*,void* pawn,arvr::FVector d,arvr::FRotator r,void* h,int a,int b,int c,int e){
 auto hit=(arvr::RotationHit*)h;
 if(level!=(void*)2||pawn!=(void*)1||d.X||d.Y||d.Z||r.Pitch||r.Roll||r.Yaw!=expected||a||b||c||e||hit->time!=1||hit->item!=-1)std::exit(2);
 ++calls;return 1;
}
static arvr::FRotator received{};static unsigned weaponCalls=0;
static int __fastcall Weapon(void*,void*,arvr::FRotator r,arvr::FVector* offset){received=r;++weaponCalls;if(offset)*offset={3,4,5};return 7;}
int main(){using namespace arvr;
 gActorGetLevel=reinterpret_cast<ActorGetLevelFn>(&Level);gLevelMove=reinterpret_cast<LevelMoveFn>(&Move);
 for(int yaw:{0,16384,-16384,65536,81920}){expected=yaw&65535;for(int i=0;i<100;++i)if(!RotatePawnNative((void*)1,yaw))return 3;}
 if(calls!=500)return 4;
 Pose origin{},head{};origin.qw=1;head.qw=std::cos(3.14159265359f/4);head.qy=-std::sin(3.14159265359f/4);head.flags=PoseOrientationValid;
 SetBodyTrackingFrame(origin,1000,1.f);int heading=0;float relative=0;
 if(!GetBodyHeading(head,heading,relative)||heading!=17384||std::fabs(relative-1.57079633f)>.001f)return 5;
 head.flags=0;if(GetBodyHeading(head,heading,relative))return 6;
 gPawnCameraRotation=(PawnCameraRotationFn)&Weapon;
 head.flags=PoseOrientationValid;pose.head=head;pose.sessionState=5;tracked=true;gFacingPawn=(void*)1;gFacingTime=GetTickCount64();
 expected=17384;FVector offset{};const unsigned previousMoves=calls;
 if(HookPawnCameraRotation((void*)1,nullptr,{12,999,34},&offset)!=7||received.Yaw!=expected||received.Pitch!=12||received.Roll!=34||offset.X!=3||offset.Y!=4||offset.Z!=5||calls!=previousMoves+1)return 7;
 menu=true;HookPawnCameraRotation((void*)1,nullptr,{0,999,0},nullptr);if(received.Yaw!=999||calls!=previousMoves+1)return 8;menu=false;
 HookPawnCameraRotation((void*)3,nullptr,{0,777,0},nullptr);if(received.Yaw!=777)return 9;
 pose.sessionState=3;HookPawnCameraRotation((void*)1,nullptr,{0,666,0},nullptr);if(received.Yaw!=666)return 10;pose.sessionState=5;
 gFacingTime=GetTickCount64()-300;HookPawnCameraRotation((void*)1,nullptr,{0,555,0},nullptr);if(received.Yaw!=555)return 11;
 if(calls!=previousMoves+1||weaponCalls!=5)return 12;
 // Verify native forward/strafe axes against the final VR world basis.
 const float unit=6.28318530718f/65536.f;
 for(int nativeYaw:{0,1000,49000})for(int stickYaw:{0,16384,32768,-16384})for(int headYaw:{0,8192,-16384}){
  Pose h{};h.flags=PoseOrientationValid;h.qw=std::cos(headYaw*unit*.5f);h.qy=-std::sin(headYaw*unit*.5f);
  SetBodyTrackingFrame(origin,nativeYaw+stickYaw,1.f,stickYaw);
  if(!GetBodyHeading(h,heading,relative))return 13;
  const float view=(nativeYaw+stickYaw+headYaw)*unit;
  if(std::fabs(std::remainder(heading*unit-view,6.28318530718f))>.001f)return 14;
  for(auto input:{MoveAxes{1,0},MoveAxes{0,1},MoveAxes{-1,0},MoveAxes{0,-1}}){
   auto axes=HeadRelativeAxes(input.side,input.forward,relative,.25f);
   const float base=nativeYaw*unit;
   const float wx=axes.forward*std::cos(base)-axes.side*std::sin(base);
   const float wy=axes.forward*std::sin(base)+axes.side*std::cos(base);
   const float ex=input.forward*std::cos(view)-input.side*std::sin(view);
   const float ey=input.forward*std::sin(view)+input.side*std::cos(view);
   if(std::fabs(wx-ex)>.001f||std::fabs(wy-ey)>.001f)return 15;
  }
 }
 puts("PASS: locomotion world direction for forward/back/strafe after stick and head turns");
 puts("PASS: physics rotation native-call boundary, zero translation, wrapped absolute yaw and verified hit layout; engine calls mocked");
}
