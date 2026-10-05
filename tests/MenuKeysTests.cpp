#include "shared/MenuKeys.h"
#include <cstdio>
#include "shared/MovementKeys.h"
int main(){
 arvr::MenuKeys keys;
 auto e=keys.Update(1|16,100);if(e.down!=17||e.up||e.repeat)return 1;
 e=keys.Update(17,499);if(e.down||e.repeat)return 2;
 e=keys.Update(17,500);if(e.repeat!=1)return 3;
 e=keys.Update(17,640);if(e.repeat!=1)return 4;
 e=keys.Update(0,641);if(e.up!=17||e.down||e.repeat)return 5;
 e=keys.Update(64,700);if(e.down!=64)return 6;
 e=keys.Update(64,1500);if(e.repeat)return 7;
 arvr::HeldKeys movement;
 e=movement.Update(arvr::MovementMask(.8f,.9f,.25f,true));if(e.down!=5||e.up)return 8;
 e=movement.Update(arvr::MovementMask(.8f,.9f,.25f,true));if(e.down||e.up||e.repeat)return 9;
 e=movement.Update(arvr::MovementMask(-.8f,-.9f,.25f,true));if(e.down!=10||e.up!=5)return 10;
 e=movement.Update(arvr::MovementMask(0,0,.25f,true));if(e.up!=10||e.down)return 11;
 movement.Update(5);e=movement.Update(arvr::MovementMask(1,1,.25f,false));if(e.up!=5||e.down)return 12;
 // Reproduce WinDrv polling: a posted event cannot keep the physical key
 // down, whereas merging the held stick mask sustains it across frames.
 bool nativeHeld=false;unsigned presses=0,releases=0;
 for(unsigned frame=0;frame<120;++frame){
     const uint32_t mask=frame<100?1u:0u;
     const bool down=(arvr::MergeMovementKey('W',0,mask)&0x8000)!=0;
     if(down&&!nativeHeld)++presses;if(!down&&nativeHeld)++releases;nativeHeld=down;
 }
 if(presses!=1||releases!=1||nativeHeld)return 13;
 if(arvr::MergeMovementKey('W',short(0x8001),0)!=short(0x8001))return 14;
 if(arvr::MergeMovementKey('Z',1,15)!=1)return 15;
 puts("PASS: menu press/release, directional repeat, no repeated confirm/pause, focus-loss release");
}
