#pragma once
#include "MeleeGesture.h"
namespace arvr {
struct PocketGrenade {
    struct Hand {bool grip=false,armed=false;MeleeGesture::V previous{},pocket{};uint64_t armedAt=0,fastAt=0;};
    Hand hands[2]{};uint64_t time=0,cooldown=0;
    unsigned suppress=0;
    void Reset(){hands[0]={};hands[1]={};time=0;suppress=0;}
    bool Update(const Payload& p,bool active){
        if(!active||(p.head.flags&7)!=7||!p.heartbeatMs){Reset();return false;}
        if(time==p.heartbeatMs)return false;
        float dt=time&&p.heartbeatMs>time?float(p.heartbeatMs-time)/1000.f:0;time=p.heartbeatMs;
        const auto& q=p.head;
        MeleeGesture::V f={-2*(q.qx*q.qz+q.qw*q.qy),0,2*(q.qx*q.qx+q.qy*q.qy)-1};
        float norm=MeleeGesture::length(f);if(norm<.2f){Reset();return false;}f={f.x/norm,0,f.z/norm};
        MeleeGesture::V right={-f.z,0,f.x};
        const Pose controllers[]={p.left,p.right};const float grips[]={p.input.leftGrip,p.input.rightGrip};
        bool thrown=false;suppress=0;
        for(unsigned i=0;i<2;++i){
            auto& h=hands[i];const bool grip=grips[i]>.55f;
            auto pos=MeleeGesture::relative(controllers[i],p.head),delta=MeleeGesture::sub(pos,h.previous);
            if(dt<.003f||dt>.08f||(controllers[i].flags&7)!=7||!std::isfinite(MeleeGesture::length(pos))||MeleeGesture::length(delta)>.30f){h={};h.previous=pos;h.grip=grip;continue;}
            float side=MeleeGesture::dot(pos,right)*(i?1.f:-1.f),front=MeleeGesture::dot(pos,f);
            bool pocket=side>.10f&&side<.55f&&pos.y<-.40f&&pos.y>-.95f&&std::fabs(front)<.35f;
            if(grip&&!h.grip&&pocket&&time>=cooldown){h.armed=true;h.armedAt=time;h.pocket=pos;h.fastAt=0;}
            if(h.armed){
                suppress|=1u<<i;
                if(MeleeGesture::length(delta)/dt>.8f&&front>.1f)h.fastAt=time;
                if(!grip){
                    if(time-h.armedAt>=150&&time-h.armedAt<5000&&h.fastAt&&time-h.fastAt<=100&&front>.1f&&MeleeGesture::length(MeleeGesture::sub(pos,h.pocket))>.20f&&time>=cooldown){thrown=true;cooldown=time+700;}
                    h.armed=false;
                }else if(time-h.armedAt>5000)h.armed=false;
            }
            h.grip=grip;h.previous=pos;
        }
        return thrown;
    }
};
}
