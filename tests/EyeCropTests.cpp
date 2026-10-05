#include "shared/EyeCrop.h"
#include "shared/ProjectionCalibration.h"
#include <cstdio>
#include <limits>
int main(){
    using namespace arvr;
    const EyeFov eyes[]={{-.855211f,.785398f,.837758f,-.872665f,1},{-.785398f,.855211f,.837758f,-.872665f,1}};
    for(const auto& eye:eyes){
        EyeCrop c{};const float mx=.43f,my=.76f;
        if(!ProjectedEyeCrop(2560,1440,eye,mx,my,0,0,c))return 1;
        // Project physical rays to desktop pixels, then to the cropped eye.
        // Reconstructed OpenXR rays must agree within a source pixel.
        for(float ray:{-.5f,0.f,.5f}){
            float px=(ray*mx+1)*1280;
            float t=(px-c.x)/c.w;
            float reconstructed=std::tan(eye.angleLeft)+t*(std::tan(eye.angleRight)-std::tan(eye.angleLeft));
            if(std::fabs(reconstructed-ray)>2/(mx*2560))return 2;
        }
    }
    EyeCrop c{};
    // Recorded Advent Rising projection: vertical FOV is narrower than the
    // bridge's old square-pixel assumption. Old crop must fail; calibrate it.
    if(ProjectedEyeCrop(2560,1440,eyes[0],.471993f,1.243748f,0,0,c))return 5;
    ProjectionCalibration calibration;
    if(!calibration.Observe(129.466f,2560.f/1440,.471993f,1.243748f))return 6;
    float fov=calibration.RequiredFov(std::tan(.855211f)*1.001f,std::tan(.872665f)*1.001f,2560.f/1440);
    float half=fov*3.14159265358979323846f/360;
    float mx=1/std::tan(half*calibration.x),my=(2560.f/1440)/std::tan(half*calibration.y);
    for(const auto& eye:eyes)if(!ProjectedEyeCrop(2560,1440,eye,mx,my,0,0,c))return 7;
    if(ProjectedEyeCrop(2560,1440,eyes[0],2,2,0,0,c))return 3;
    if(ProjectedEyeCrop(2560,1440,eyes[0],std::numeric_limits<float>::quiet_NaN(),1,0,0,c))return 4;
    puts("PASS: asymmetric eye crops preserve projected rays; uncovered FOV and invalid matrices rejected");
}
