#pragma once
#include <cstdint>
#include "../shared/SharedState.h"
namespace arvr {
void MarkCameraGameplay(bool firstPerson);
void ForceFlatOverlayFor(uint32_t milliseconds);
void ClearFlatOverlay();
void SetControllerMenu(bool enabled);
bool ControllerMenuActive();
SceneMode CurrentSceneMode();
bool GameplayFirstPersonActive();
}
