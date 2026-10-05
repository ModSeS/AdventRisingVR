// Exercise the actual native-call boundary using test doubles, without loading
// game DLLs or touching the player's skeleton.
#include "../src/proxy/UnrealBones.cpp"
#include <cstdio>
#include <cstdlib>
namespace arvr {void Log(const char*,...){};static bool gameplay=true;bool GameplayFirstPersonActive(){return gameplay;}}
namespace arvr {static uint64_t testPair=0;uint64_t RenderSnapshotId(){return testPair;}}
static arvr::FRotator rotations[18]{};static unsigned calls=0,clears=0;
static int __fastcall direction(void*,void*,arvr::FName name,arvr::FRotator r,arvr::FVector,float alpha,int space,int){
    if(space!=1||name.Index<1||name.Index>17)std::exit(1);
    rotations[name.Index]=r;++calls;if(alpha==0)++clears;return 1;
}
static int __fastcall rotation(void*,void*,arvr::FName,arvr::FRotator,int,float,int){return 1;}
static float component(arvr::FRotator r,int baseYaw){
    const float k=6.28318530718f/65536;
    return std::cos(r.Pitch*k)*std::cos((r.Yaw-baseYaw)*k);
}
static float headScale=1.f;static bool expectedHidden=true;
static int __fastcall scaleHead(void*,void*,int slot,float scale,arvr::FName name,int){
 if(slot!=31||name.Index!=18)std::exit(12);headScale=scale;return 1;
}
static void __fastcall frame(void*,void*,void* actor,void*,arvr::FVector*,int,int&,unsigned long){
 if(actor!=(void*)1)return;
 if(headScale!=(expectedHidden?0.f:1.f))std::exit(13);
}
int main(){
    using namespace arvr;
    Pose poleOrigin{};poleOrigin.qw=1;
    for(int angle:{0,8192,16384,32768,-16384})for(bool right:{false,true}){
        Pose h{};h.qw=std::cos(angle*6.28318530718f/65536.f/2);h.qy=-std::sin(angle*6.28318530718f/65536.f/2);
        FVector local=WorldToBody(ElbowPole(right,poleOrigin,h),angle);
        if(local.X>=0||(right?local.Y:-local.Y)<.9f||local.Z>=0)return 20;
    }
    gSetBoneDirection=reinterpret_cast<SetBoneDirectionFn>(&direction);
    gSetBoneRotation=reinterpret_cast<SetBoneRotationFn>(&rotation);
    gRightArm={1};gRightForeArm={2};gRightHand={3};gLeftArm={4};gLeftForeArm={5};gLeftHand={6};
    Pose origin{},head{},hand{};hand.flags=PosePositionValid|PoseOrientationValid;
    for(bool right:{false,true})for(int yaw:{0,16384}){
        hand.px=right?.32f:-.32f;hand.py=-.34f;
        float previous=-100;
        for(float forward:{.15f,.3f,.5f}){
            hand.pz=-forward;unsigned before=calls;
            if(!CallArm((void*)1,right,origin,head,hand,50,yaw)||calls-before!=3)return 2;
            const int first=right?1:4;
            float reached=(right?-1.f:1.f)*(21.77f*component(rotations[first],yaw)+21.73f*component(rotations[first+1],yaw));
            if(std::fabs(reached-forward*50)>.05f||reached<=previous)return 3;
            previous=reached;
        }
        // Reconstruct actual endpoints with the right skeleton's negative X.
        hand.pz=-.3f;float previousHeight=-100;
        for(float height:{-.6f,-.3f,0.f,.2f}){
            hand.py=height;
            CallArm((void*)1,right,origin,head,hand,50,yaw);
            const int first=right?1:4;const float k=6.28318530718f/65536;
            float z=-17+(right?-1.f:1.f)*(21.77f*std::sin(rotations[first].Pitch*k)+21.73f*std::sin(rotations[first+1].Pitch*k));
            if(std::fabs(z-(height*50-3))>.05f||z<=previousHeight)return 5;
            previousHeight=z;
        }
        hand.flags=0;CallArm((void*)1,right,origin,head,hand,50,yaw);hand.flags=3;
    }
    // A real shoulder behind the camera must reach the camera-relative
    // target, rather than reproducing the old offset from a guessed shoulder.
    for(bool right:{false,true})for(int yaw:{0,16384}){
        const FVector shoulder{-12,right?12.f:-12.f,-20};
        hand.px=right?.24f:-.24f;hand.py=-.3f;hand.pz=-.3f;hand.flags=3;
        CallArm((void*)1,right,origin,head,hand,50,yaw,&shoulder);
        const int first=right?1:4;const float sign=right?-1.f:1.f,k=6.28318530718f/65536;
        const float x=shoulder.X+sign*(21.77f*component(rotations[first],yaw)+21.73f*component(rotations[first+1],yaw));
        const float z=shoulder.Z+sign*(21.77f*std::sin(rotations[first].Pitch*k)+21.73f*std::sin(rotations[first+1].Pitch*k));
        if(std::fabs(x-15)>.05f||std::fabs(z+18)>.05f)return 6;
    }
    FVector body=WorldToBody({10,20,30},16384);
    if(std::fabs(body.X-20)>.01f||std::fabs(body.Y+10)>.01f||body.Z!=30)return 7;
    // Right fingers use -X: their direction must match controller forward.
    hand={};hand.flags=3;hand.pz=-.3f;hand.py=-.3f;
    for(float pitch:{-.5f,0.f,.5f}){
        hand.qx=std::sin(pitch*.5f);hand.qw=std::cos(pitch*.5f);
        CallArm((void*)1,true,origin,head,hand,50,0);
        const float k=6.28318530718f/65536;
        FVector forward=Rotate({hand.qx,0,0,hand.qw},{0,0,-1});
        const auto w=rotations[3];
        if(std::fabs(-component(w,0)+forward.Z)>.01f||std::fabs(-std::sin(w.Pitch*k)-forward.Y)>.01f)return 8;
    }
    if(clears!=12)return 4;
    for(unsigned i=0;i<11;++i){gFingers[i]={int(7+i)};gFingerAxis[i]=(i%2)?1.f:-1.f;}
    hand={};hand.qw=1;hand.flags=3;hand.pz=-.3f;hand.py=-.3f;
    unsigned before=calls;
    CallArm((void*)1,true,origin,head,hand,50,0);
    if(calls-before!=14)return 9;
    for(unsigned i=0;i<11;++i)if(std::fabs(gFingerAxis[i]*component(rotations[7+i],0)-1.f)>.01f)return 10;
    before=clears;hand.flags=0;CallArm((void*)1,true,origin,head,hand,50,0);
    if(clears-before!=14)return 11;
    gSetBoneScale=reinterpret_cast<SetBoneScaleFn>(&scaleHead);gHeadBone={18};
    gOrigGetFrame=reinterpret_cast<GetFrameFn>(&frame);gTargetActor=(void*)1;gReady=false;
    int vertexCount=0;
    HookGetFrame((void*)1,nullptr,(void*)1,nullptr,nullptr,0,vertexCount,0);
    if(headScale!=0.f)return 14;
    expectedHidden=false;
    HookGetFrame((void*)1,nullptr,(void*)2,nullptr,nullptr,0,vertexCount,0);
    if(headScale!=0.f)return 15;
    // Both eyes and shadow passes reuse the same bone requests for this pair.
    unsigned char instance[64]{};expectedHidden=true;gReady=true;testPair=1;
    before=calls;
    HookGetFrame(instance,nullptr,(void*)1,nullptr,nullptr,0,vertexCount,0);
    const unsigned pairCalls=calls-before;if(!pairCalls)return 16;
    HookGetFrame(instance,nullptr,(void*)1,nullptr,nullptr,0,vertexCount,0);
    if(calls!=before+pairCalls||headScale!=0.f)return 17;
    ++testPair;
    HookGetFrame(instance,nullptr,(void*)1,nullptr,nullptr,0,vertexCount,0);
    if(calls!=before+2*pairCalls||headScale!=0.f)return 18;
    gameplay=false;expectedHidden=false;
    HookGetFrame(instance,nullptr,(void*)1,nullptr,nullptr,0,vertexCount,0);
    if(headScale!=1.f)return 19;
    puts("PASS: both arms use world coordinates; forward and vertical targets follow both controllers after body turn; tracking loss clears all bones");
}
