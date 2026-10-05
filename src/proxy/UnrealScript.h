#pragma once
#include "MathVR.h"
namespace arvr {
void UpdateVRHud(void* pawn,bool active);
void UpdateVRTargeting(void* playerController,bool active);
bool CallNoParamScript(void* object, const char* functionName);
bool CameraSystemIsFirstPerson(void* playerController);
bool EnsureFirstPerson(void* playerController, void* viewActor);
void SetBodyTrackingFrame(const Pose& origin,int baseYaw,float yawScale,int movementYawOffset=0);
bool GetBodyHeading(const Pose& head,int& worldYaw,float& relativeRadians);
bool SetPawnFacingYaw(void* pawn,int yaw);
void* GetPlayerPawn(void* playerController);
// Safer route for skeletal control: call Actor.SetBoneDirection through UObject::ProcessEvent
// instead of dereferencing the actor's Mesh pointer and invoking UMeshInstance directly.
bool SetActorBoneDirection(void* actor, const char* boneName, const FRotator& turn, const FVector& trans, float alpha, int space, int preCalculatedBone);
bool SetActorBoneRotation(void* actor, const char* boneName, const FRotator& turn, int space, float alpha, int preCalculatedBone);
bool PrimeArmIK(void* playerController);
bool SetPlayerFOVNative(void* playerController, float horizontalFovDegrees);
}
