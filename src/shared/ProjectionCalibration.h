#pragma once
#include <cmath>
#include <algorithm>
namespace arvr {
struct ProjectionCalibration {
    float x=1,y=1;
    bool Observe(float degrees,float aspect,float mx,float my){
        if(!std::isfinite(degrees)||!std::isfinite(aspect)||!std::isfinite(mx)||!std::isfinite(my)||degrees<=0||degrees>=179||aspect<=0||mx<=0||my<=0)return false;
        float half=degrees*3.14159265358979323846f/360;
        float sx=std::atan(1/mx)/half,sy=std::atan(aspect/my)/half;
        if(sx<.25f||sx>2||sy<.25f||sy>2)return false;
        x=sx;y=sy;return true;
    }
    float RequiredFov(float horizontalTangent,float verticalTangent,float aspect)const{
        // UE2 independently scales the horizontal and vertical half angles.
        float half=std::max(std::atan(horizontalTangent)/x,std::atan(verticalTangent*aspect)/y);
        return std::min(175.f,half*360/3.14159265358979323846f);
    }
};
}
