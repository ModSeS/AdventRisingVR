#include <Windows.h>
#include <cstdio>
#include "BridgeLog.h"
namespace arvr { int RunOpenXR(); }
int main(){
    HANDLE singleton=CreateMutexW(nullptr,TRUE,L"Local\\AdventRisingVR_Bridge_v8");
    if(!singleton || GetLastError()==ERROR_ALREADY_EXISTS){if(singleton)CloseHandle(singleton);return 12;}
    SetConsoleOutputCP(65001);
    puts("Advent Rising VR Bridge v0.8.21-dev");
    arvr::BridgeLog("AdventRisingVRBridge v0.8.21-dev start, pid=%lu", GetCurrentProcessId());
    const int result=arvr::RunOpenXR();ReleaseMutex(singleton);CloseHandle(singleton);return result;
}
