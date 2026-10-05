#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
namespace arvr {
struct alignas(8) HudData {
 uint64_t timestamp=0;
 uint32_t active=0;
 float health=-1;
 int32_t leftClip=-1,leftReserve=-1,rightClip=-1,rightReserve=-1;
};
struct alignas(8) HudShared {uint32_t magic=0,version=1;volatile LONG sequence=0;uint32_t reserved=0;HudData data{};};
static_assert(sizeof(HudData)==32 && sizeof(HudShared)==48);
inline bool HudFresh(const HudData& d,uint64_t now){return d.active && d.timestamp && now>=d.timestamp && now-d.timestamp<500;}
class HudChannel {
 HANDLE map=nullptr;HudShared* state=nullptr;
 bool Open(bool write){
  if(state)return true;
#ifdef ARVR_TEST_IPC
  const wchar_t* name=L"Local\\AdventRisingVR_Test_Hud_v1";
#else
  const wchar_t* name=L"Local\\AdventRisingVR_Hud_v1";
#endif
  map=write?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(HudShared),name):OpenFileMappingW(FILE_MAP_READ,FALSE,name);
  if(!map)return false;
  state=(HudShared*)MapViewOfFile(map,write?FILE_MAP_ALL_ACCESS:FILE_MAP_READ,0,0,sizeof(HudShared));
  if(!state){CloseHandle(map);map=nullptr;return false;}return true;
 }
public:
 ~HudChannel(){if(state)UnmapViewOfFile(state);if(map)CloseHandle(map);}
 bool Write(const HudData& d){
  if(!Open(true))return false;
  InterlockedIncrement(&state->sequence);MemoryBarrier();
  state->magic=0x48565241;state->version=1;state->data=d;
  MemoryBarrier();InterlockedIncrement(&state->sequence);return true;
 }
 bool Read(HudData& d){
  if(!Open(false))return false;
  for(int i=0;i<4;++i){LONG seq=state->sequence;if(seq&1)continue;MemoryBarrier();
   const bool valid=state->magic==0x48565241 && state->version==1;
   d=state->data;MemoryBarrier();if(seq==state->sequence)return valid;
  }return false;
 }
};
}
