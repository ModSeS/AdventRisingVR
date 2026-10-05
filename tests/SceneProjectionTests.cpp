#include "../src/proxy/SceneViewHook.cpp"
#include "shared/EyeCrop.h"
#include <cstdio>
namespace arvr {
static uint64_t testPair=1;static StereoEye testEye=StereoEyeLeft;
bool ReadBridgePayload(Payload& p){p.predictedDisplayTime=123;p.eyeFov[0]={-.855211f,.785398f,.837758f,-.872665f,1};p.eyeFov[1]={-.785398f,.855211f,.837758f,-.872665f,1};return true;}
StereoEye GetForcedStereoEye(){return testEye;}
uint64_t RenderSnapshotId(){return testPair;}
SceneMode CurrentSceneMode(){return SceneGameplayVR;}
float GetCurrentGameAspect(){return 2560.f/1440;}
void Log(const char*,...){}
}
static unsigned calls=0;
static void* __fastcall build(void* self,void*,void* viewport,void*,void*,arvr::FVector,arvr::FRotator,float fov){
 auto s=(float*)((char*)viewport+0x98),m=(float*)((char*)self+0x98);
 const float half=fov*3.14159265359f/360;
 m[0]=1/std::tan(half*s[0]);m[5]=arvr::GetCurrentGameAspect()/std::tan(half*s[1]);m[8]=m[9]=0;
 ++calls;return self;
}
int main(){using namespace arvr;
 alignas(16) unsigned char viewport[512]{},scene[512]{};
 auto s=(float*)(viewport+0x98);s[0]=1.f;s[1]=.85f;
 original=reinterpret_cast<ConstructFn>(&build);
 for(int i=0;i<4;++i){
  testEye=(StereoEye)(i%2);testPair=1+i/2;
  Construct(scene,nullptr,viewport,nullptr,nullptr,{}, {},75);
  if(s[0]!=1.f||s[1]!=.85f)return 1;
  float x,y,ox,oy;if(!GetGameProjection(x,y,ox,oy))return 2;
  if(std::fabs(y/x-GetCurrentGameAspect())>.001f)return 3;
  Payload p{};ReadBridgePayload(p);EyeCrop c{};
  if(i>0&&!ProjectedEyeCrop(2560,1440,p.eyeFov[(int)testEye],x,y,ox,oy,c))return 4;
  if(i>0&&c.w<1200)return 5;
 }
 if(calls!=4)return 6;
 ++testPair;float x,y,ox,oy;if(GetGameProjection(x,y,ox,oy))return 7;
 puts("PASS: scene construction restores viewport scales, uses square-pixel projection, covers Quest FOV and rejects stale pairs (native constructor mocked)");
}
