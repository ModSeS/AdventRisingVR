#pragma once
#include "../shared/SharedState.h"
namespace arvr {
struct FVector { float X{},Y{},Z{}; }; struct FRotator { int Pitch{},Yaw{},Roll{}; };
struct Quat { float x{},y{},z{},w{1}; };
Pose YawOnlyOrigin(const Pose& pose);
Quat Conjugate(Quat q); Quat Mul(Quat a,Quat b); FVector Rotate(Quat q,FVector v); FRotator RelativePoseToUnrealRotation(const Pose& origin,const Pose& current,float pitchSign,float yawSign,float rollSign); FVector RelativePoseToUnrealPosition(const Pose& origin,const Pose& current,float unitsPerMeter,int baseYaw);
}
