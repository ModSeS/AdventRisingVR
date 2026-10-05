#pragma once
#include <Windows.h>
#include <cstddef>
namespace arvr {
void StartControllerInputThread();
void ApplyQuestKeyboardState(unsigned char* keys, size_t count);
void ApplyQuestMouseState(void* state, DWORD bytes);
}
