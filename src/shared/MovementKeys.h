#pragma once
#include "MenuKeys.h"
namespace arvr {
// W,S,D,A. The native WinDrv keyboard poll consumes this held mask.
inline uint32_t MovementMask(float x,float y,float dead,bool gameplay){
    if(!gameplay)return 0;
    return (y>dead?1u:0u)|(y<-dead?2u:0u)|(x>dead?4u:0u)|(x<-dead?8u:0u);
}
inline short MergeMovementKey(int key,short physical,uint32_t mask){
    const int keys[]={'W','S','D','A'};
    for(unsigned i=0;i<4;++i)if(key==keys[i]&&(mask&(1u<<i)))return short(physical|0x8000);
    return physical;
}
class HeldKeys {
    uint32_t held=0;
public:
    MenuKeyEvents Update(uint32_t mask){
        MenuKeyEvents e{mask&~held,held&~mask,0};held=mask;return e;
    }
};
}
