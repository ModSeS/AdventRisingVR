#pragma once
#include <cstdint>
#include "MathVR.h"
#include "../shared/SharedState.h"

namespace arvr {
enum StereoModeValue : int {
    StereoModeMono = 0,
    StereoModeAlternate = 1,
    StereoModeDoubleDraw = 2
};

int StereoMode();
bool StereoEnabled();
void AdvanceStereoEye();
StereoEye GetActiveStereoEye();
void SetForcedStereoEye(StereoEye eye);
StereoEye GetForcedStereoEye();
StereoEye GetCameraStereoEye();
void ApplyStereoEyeOffset(FVector& loc, const Pose& originHead, const Pose& currentHead, float unitsPerMeter, int baseYaw, int finalYaw);
}
