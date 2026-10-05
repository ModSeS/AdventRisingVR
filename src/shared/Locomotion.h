#pragma once
#include <cmath>
namespace arvr {
struct MoveAxes {float forward{},side{};};
inline MoveAxes HeadRelativeAxes(float x,float y,float heading,float dead){
 if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(heading))return {};
 float length=std::sqrt(x*x+y*y);if(length<=dead||dead>=1.f)return {};
 float strength=(std::fmin(length,1.f)-dead)/(1.f-dead);
 x=x/length*strength;y=y/length*strength;
 return {y*std::cos(heading)-x*std::sin(heading),y*std::sin(heading)+x*std::cos(heading)};
}
}
