#include "proxy/MathVR.h"
#include "proxy/StereoState.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace arvr;
namespace arvr {bool ReadBridgePayload(Payload&){return false;}}
static void check(bool ok,const char* what){if(!ok){std::printf("FAIL: %s\n",what);std::exit(1);}}
static bool approx(float a,float b){return std::fabs(a-b)<0.002f;}
int main(){
    const float s=std::sqrt(.5f);Pose origin{},head{};
    auto zero=RelativePoseToUnrealRotation(origin,head,1,1,1);
    check(zero.Yaw==0&&zero.Pitch==0&&zero.Roll==0,"identity");
    head.qy=s;head.qw=s;
    auto r=RelativePoseToUnrealRotation(origin,head,1,1,1);
    check(r.Yaw==-16384&&r.Pitch==0,"left 90 degrees maps once");
    auto rec=YawOnlyOrigin(head);r=RelativePoseToUnrealRotation(rec,head,1,1,1);check(r.Yaw==0,"yaw recenter");
    head={};head.qx=std::sin(.25f);head.qw=std::cos(.25f);rec=YawOnlyOrigin(head);
    r=RelativePoseToUnrealRotation(rec,head,1,1,1);check(r.Pitch>5000,"recenter preserves physical pitch");
    head={};head.pz=-1;auto v=RelativePoseToUnrealPosition(origin,head,50,0);check(approx(v.X,50)&&approx(v.Z,0),"forward meter");
    v=RelativePoseToUnrealPosition(origin,head,50,16384);check(approx(v.X,0)&&approx(v.Y,50),"body yaw applied once");
    WritePrivateProfileStringW(L"Stereo",L"Mode",L"2",L".\\AdventRisingVR.ini");
    head={};head.qy=s;head.qw=s;SetForcedStereoEye(StereoEyeRight);v={};
    ApplyStereoEyeOffset(v,origin,head,50,0,-16384);
    check(approx(v.X,1.6f)&&approx(v.Y,0),"eye baseline does not double head yaw");
    std::printf("PASS: 7 camera/stereo regression checks; SharedState=%zu Payload=%zu FrameHeader=%zu\n",sizeof(SharedState),sizeof(Payload),sizeof(FrameTransportHeader));
}

