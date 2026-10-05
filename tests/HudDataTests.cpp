#include "../src/proxy/UnrealScript.cpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
namespace arvr {void Log(const char*,...){};bool ReadBridgePayload(Payload&){return false;}bool GameplayFirstPersonActive(){return true;}bool ControllerMenuActive(){return false;}}
using namespace arvr;
static void check(bool ok){if(!ok)std::exit(1);}
static float health=.73f;static void* left=(void*)2;static void* right=(void*)3;
static float __fastcall Health(void* pawn,void*,unsigned clamp){check(pawn==(void*)1&&clamp==1);return health;}
static EngineString* __fastcall String(EngineString* s,void*,const wchar_t* t){s->data=(wchar_t*)t;return s;}
static int __fastcall Property(void* pawn,void*,EngineString s,void*** out){check(pawn==(void*)1);*out=!wcscmp(s.data,L"LeftWeapon")?&left:&right;return 1;}
static float __fastcall Ammo(void* weapon,void*,int mode,unsigned reserve,unsigned shared){check(mode==0&&shared==reserve);return weapon==(void*)2?(reserve?90.f:12.f):(reserve?40.f:7.f);}
int main(){
 gHudHealth=(HealthEventFn)&Health;gHudString=(StringCtorFn)&String;gHudProperty=(ObjectPropertyFn)&Property;gHudAmmo=(AmmoEventFn)&Ammo;
 auto d=ReadHudNative((void*)1);check(d.active&&d.health==.73f&&d.leftClip==12&&d.leftReserve==90&&d.rightClip==7&&d.rightReserve==40);
 left=nullptr;health=std::numeric_limits<float>::quiet_NaN();d=ReadHudNative((void*)1);check(d.leftClip==-1&&d.leftReserve==-1&&d.health==-1&&d.rightClip==7);
 check(AmmoCount(-1)==-1 && AmmoCount(std::numeric_limits<float>::infinity())==-1);
 HudChannel writer,reader;d.timestamp=1000;check(writer.Write(d));HudData copy{};check(reader.Read(copy)&&copy.rightClip==7);
 check(HudFresh(copy,1000)&&HudFresh(copy,1499)&&!HudFresh(copy,1500)&&!HudFresh(copy,999));
 d.active=0;check(writer.Write(d)&&reader.Read(copy)&&!HudFresh(copy,1000));
 puts("PASS: HUD native getter contract, independent hands/reserve, missing values, shared layout and expiry; game reads mocked");
}
