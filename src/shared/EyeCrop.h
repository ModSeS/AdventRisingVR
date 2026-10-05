#pragma once
#include "SharedState.h"
#include <cmath>
#include <algorithm>
namespace arvr {
struct EyeCrop {uint32_t x{},y{},w{},h{};};
inline bool ProjectedEyeCrop(uint32_t width,uint32_t height,const EyeFov& f,float mx,float my,float ox,float oy,EyeCrop& out){
    if(!width||!height||!f.valid||!std::isfinite(mx)||!std::isfinite(my)||!std::isfinite(ox)||!std::isfinite(oy)||mx<=0||my<=0)return false;
    const float l=std::tan(f.angleLeft)*mx+ox,r=std::tan(f.angleRight)*mx+ox;
    const float u=std::tan(f.angleUp)*my+oy,d=std::tan(f.angleDown)*my+oy;
    if(!std::isfinite(l)||!std::isfinite(r)||!std::isfinite(u)||!std::isfinite(d)||l>=r||d>=u||l < -1.002f||r > 1.002f||d < -1.002f||u > 1.002f)return false;
    auto unit=[](float x){return std::clamp(x,0.0f,1.0f);};
    uint32_t x0=uint32_t(std::floor(unit((l+1)*.5f)*width)),x1=uint32_t(std::ceil(unit((r+1)*.5f)*width));
    uint32_t y0=uint32_t(std::floor(unit((1-u)*.5f)*height)),y1=uint32_t(std::ceil(unit((1-d)*.5f)*height));
    if(x1<=x0+8||y1<=y0+8)return false;
    out={x0,y0,x1-x0,y1-y0};return true;
}
}
