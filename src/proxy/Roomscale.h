#pragma once
#include "MathVR.h"
namespace arvr {
// Keep the consumed position in tracking space. Turning with the stick must
// rotate future physical steps, not replay the distance already walked.
struct RoomscaleTracker {
    Pose anchor{};
    bool valid=false;
    void Reset(const Pose& origin,const Pose& head){anchor=origin;Consume(head);valid=true;}
    void Consume(const Pose& head){anchor.px=head.px;anchor.py=head.py;anchor.pz=head.pz;}
    FVector Delta(const Pose& head,float units,int yaw)const{
        auto d=RelativePoseToUnrealPosition(anchor,head,units,yaw);d.Z=0;return d;
    }
};
}
