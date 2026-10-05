#pragma once
#include "SharedState.h"
#include <cmath>
#include <algorithm>
namespace arvr {
struct StickTurn {
 uint64_t time=0;float degrees=0;
 int Update(const Payload& p,bool active,float speed=90.f,float dead=.25f){
  if(!active || !(p.head.flags&PoseOrientationValid) || !p.heartbeatMs){time=0;return (int)std::lround(degrees*(65536.f/360.f));}
  if(p.heartbeatMs!=time){
   const float dt=time&&p.heartbeatMs>time?(p.heartbeatMs-time)/1000.f:0;time=p.heartbeatMs;
   float x=p.input.rightStickX;dead=std::clamp(dead,0.f,.9f);
   if(std::isfinite(x)&&std::isfinite(speed)&&dt>0&&dt<=.25f&&std::fabs(x)>dead){
    const float axis=std::copysign((std::min(std::fabs(x),1.f)-dead)/(1-dead),x);
    degrees=std::remainder(degrees+axis*std::clamp(speed,0.f,360.f)*dt,360.f);
   }
  }
  return (int)std::lround(degrees*(65536.f/360.f));
 }
};
}
