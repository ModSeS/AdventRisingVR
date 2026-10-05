#pragma once
#include "../shared/SharedState.h"
#include "MathVR.h"
namespace arvr {
void UpdateExperimentalHandTarget(void* actor,const Payload& now,const Pose& originHead,const Pose& currentHead,int baseYaw,const FVector& cameraCenter);
void ApplyExperimentalHands(void* actor,const Payload& now,const Pose& originHead,const Pose& currentHead);
}
