#include "SharedReader.h"
#include "Log.h"
#include <Windows.h>
#include <cstring>
#include <cwchar>
#include <cstdlib>
namespace arvr {
static thread_local bool gFrozen=false;
static thread_local bool gSnapshotValid=false;
static thread_local Payload gSnapshot{};
static thread_local uint64_t gSnapshotId=0;
static HANDLE gMap=nullptr; static SharedState* gState=nullptr;
static bool OpenMap(){ if(gState) return true; gMap=OpenFileMappingW(FILE_MAP_READ,FALSE,kMappingName); if(!gMap) return false;
 gState=(SharedState*)MapViewOfFile(gMap,FILE_MAP_READ,0,0,sizeof(SharedState)); if(!gState){CloseHandle(gMap);gMap=nullptr;return false;}
 if(gState->magic!=kMagic||gState->version!=kVersion){UnmapViewOfFile(gState);CloseHandle(gMap);gState=nullptr;gMap=nullptr;return false;} return true; }
static void LaunchBridge(){ wchar_t dllPath[MAX_PATH]{}; HMODULE self=nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&LaunchBridge,&self);
 GetModuleFileNameW(self,dllPath,MAX_PATH); wchar_t* slash=std::wcsrchr(dllPath,L'\\'); if(slash) *(slash+1)=0; wcscat_s(dllPath,_countof(dllPath),L"AdventRisingVRBridge.exe");
 STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{}; wchar_t cmd[MAX_PATH+4]{}; swprintf_s(cmd,_countof(cmd),L"\"%ls\"",dllPath);
 if(CreateProcessW(dllPath,cmd,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){Log("Started bridge pid=%lu",pi.dwProcessId);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);} else Log("Bridge launch failed: %lu",GetLastError()); }
bool EnsureBridge(bool autoLaunch){ if(OpenMap()) return true; if(autoLaunch) LaunchBridge(); for(int i=0;i<50;i++){Sleep(100);if(OpenMap()) return true;} return false; }
bool ReadBridgePayload(Payload& out){ if(gFrozen){out=gSnapshot;return gSnapshotValid;} if(!OpenMap()) return false; for(int n=0;n<4;n++){LONG a=gState->sequence;if(a&1) continue; MemoryBarrier(); std::memcpy(&out,&gState->payload,sizeof(out)); MemoryBarrier(); LONG b=gState->sequence;if(a==b && !(b&1)) return out.heartbeatMs && GetTickCount64()>=out.heartbeatMs && GetTickCount64()-out.heartbeatMs<250;} return false; }
void BeginRenderSnapshot(){gSnapshotValid=ReadBridgePayload(gSnapshot);++gSnapshotId;gFrozen=true;}
uint64_t RenderSnapshotId(){return gFrozen?gSnapshotId:0;}
void EndRenderSnapshot(){gFrozen=false;}
}
