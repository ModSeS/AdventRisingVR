#pragma once
#include "SharedState.h"
#include <cmath>
#include <algorithm>
namespace arvr {
struct MeleeGesture {
    struct V {float x,y,z;};
    struct Hand {V previous{},start{};bool ready=false,swing=false;float settled=0,age=0;};
    Hand hands[2]{};uint64_t time=0,cooldownUntil=0;Pose previousHead{};
    static V sub(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
    static float dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
    static float length(V a){return std::sqrt(dot(a,a));}
    static V relative(const Pose& hand,const Pose& head){return {hand.px-head.px,hand.py-head.py,hand.pz-head.pz};}
    static bool TriggerHeld(const Payload& p,unsigned hand){
        const float value=hand==1?p.input.leftTrigger:p.input.rightTrigger;
        return (hand==1||hand==2)&&std::isfinite(value)&&value>.35f;
    }
    void Reset(){hands[0]={};hands[1]={};time=0;}
    unsigned Update(const Payload& p,bool enabled,float speedThreshold=.9f,float distanceThreshold=.06f){
        if(!enabled || (p.head.flags&7)!=7 || !p.heartbeatMs){Reset();return 0;}
        if(p.heartbeatMs==time)return 0;
        float dt=time&&p.heartbeatMs>time?float(p.heartbeatMs-time)/1000.f:0;
        const Pose controller[2]={p.left,p.right};
        float qdot=std::fabs(p.head.qx*previousHead.qx+p.head.qy*previousHead.qy+p.head.qz*previousHead.qz+p.head.qw*previousHead.qw);
        bool valid=dt>=.003f&&dt<=.08f&&std::isfinite(qdot);
        bool turning=valid&&2*std::acos(std::clamp(qdot,0.f,1.f))/dt>4.f;
        time=p.heartbeatMs;previousHead=p.head;
        const auto& q=p.head;
        V forward={-2*(q.qx*q.qz+q.qw*q.qy),2*(q.qw*q.qx-q.qy*q.qz),2*(q.qx*q.qx+q.qy*q.qy)-1};
        unsigned result=0;
        for(unsigned i=0;i<2;++i){
            auto& h=hands[i];V pos=relative(controller[i],p.head),step=sub(pos,h.previous);
            if(!TriggerHeld(p,1u<<i)||!valid||turning||(controller[i].flags&7)!=7||!std::isfinite(length(pos))||length(step)>.25f){h={};h.previous=pos;continue;}
            float speed=length(step)/dt;V velocity={step.x/dt,step.y/dt,step.z/dt};
            if(speed<.6f){h.settled+=dt;h.swing=false;h.age=0;if(h.settled>=.04f)h.ready=true;}
            else h.settled=0;
            if(dot(velocity,pos)<-.10f){h.ready=true;h.swing=false;h.age=0;}
            bool directed=dot(pos,forward)>.05f && dot(velocity,pos)>-.02f && pos.y>-.85f&&pos.y<.45f&&length(pos)<1.1f;
            if(!directed){h.swing=false;h.age=0;}
            if(h.ready&&directed&&speed>.6f){
                if(!h.swing){h.start=h.previous;h.age=0;h.swing=true;}
                h.age+=dt;
                if(h.age<=.35f&&speed>=speedThreshold&&length(sub(pos,h.start))>=distanceThreshold&&time>=cooldownUntil){
                    result=1u<<i;cooldownUntil=time+300;h.ready=false;h.swing=false;
                }
                if(h.age>.35f)h.ready=false;
            }
            h.previous=pos;
        }
        return result;
    }
};
}
