#include "../src/proxy/InputBridge.cpp"
#include <cstdio>
#include <cstdlib>
namespace arvr {
bool ReadBridgePayload(Payload&){return false;}bool GetBodyHeading(const Pose&,int&,float&){return false;}
bool GameplayFirstPersonActive(){return true;}bool ControllerMenuActive(){return false;}void SetControllerMenu(bool){}
void Log(const char*,...){}
}
static void check(bool ok){if(!ok)std::exit(1);}
static int actions[8]{};static unsigned count=0;
static int __fastcall process(void*,void*,void*,int key,int action,float){check(key=='F');actions[count++]=action;return 1;}
static unsigned char __fastcall down(void*,void*,int){return 0;}
struct ControlEvent{int key,action;};static ControlEvent events[128]{};static unsigned eventCount=0;
static int __fastcall controls(void*,void*,void*,int key,int action,float){events[eventCount++]={key,action};return 1;}
static int __fastcall findKey(const void*,void*,const wchar_t* name,int& key){
 const wchar_t* names[]={L"LeftMouse",L"RightMouse",L"Q",L"E",L"Space",L"R",L"MouseWheelUp",L"MiddleMouse",L"3",L"MouseWheelDown",L"G"};
 const int keys[]={1,2,'Q','E',32,'R',236,4,'3',237,'G'};
 for(int i=0;i<11;++i)if(!wcscmp(names[i],name)){key=keys[i];return 1;}return 0;
}
int main(){using namespace arvr;
 Payload p{};p.head.flags=p.left.flags=p.right.flags=7;p.head.py=1.7f;p.left.py=p.right.py=1.3f;
 p.left.px=-.2f;p.right.px=.2f;p.left.pz=p.right.pz=-.25f;p.heartbeatMs=1000;
 p.input.leftTrigger=p.input.rightTrigger=1;
 MeleeGesture g;unsigned attacks=0;
 auto tick=[&](){p.heartbeatMs+=10;auto fired=g.Update(p,true);attacks+=fired!=0;return fired;};
 for(int i=0;i<20;++i)check(tick()==0);
 for(int i=0;i<10;++i){p.right.pz-=.02f;tick();}
 check(attacks==1);check(g.Update(p,true)==0); // duplicate payload
 for(int i=0;i<10;++i){p.right.pz+=.02f;check(tick()==0);} // retract
 for(int i=0;i<55;++i)tick();
 for(int i=0;i<10;++i){p.left.pz-=.02f;tick();}check(attacks==2);
 for(int i=0;i<55;++i)tick();
 for(int i=0;i<30;++i){p.head.pz-=.03f;p.left.pz-=.03f;p.right.pz-=.03f;check(tick()==0);} // walking
 p.right.flags=0;for(int i=0;i<10;++i){p.right.pz-=.04f;check(tick()==0);}
 p.right.flags=7;check(tick()==0); // reacquisition
 p.heartbeatMs+=1000;check(g.Update(p,true)==0); // tracking gap
 check(g.Update(p,false)==0);
 for(int i=0;i<20;++i)tick();
 for(int i=1;i<=10;++i){p.head.qy=std::sin(i*.03f);p.head.qw=std::cos(i*.03f);p.left.pz-=.02f;check(tick()==0);} // fast head turning
 // Only the swinging hand's trigger qualifies; release discards its swing.
 for(unsigned hand=1;hand<=2;++hand){
  Payload q{};q.head.flags=q.left.flags=q.right.flags=7;q.head.py=1.7f;q.left.py=q.right.py=1.3f;
  q.left.px=-.2f;q.right.px=.2f;q.left.pz=q.right.pz=-.25f;q.heartbeatMs=1000;
  auto& pose=hand==1?q.left:q.right;auto& trigger=hand==1?q.input.leftTrigger:q.input.rightTrigger;
  auto& other=hand==1?q.input.rightTrigger:q.input.leftTrigger;
  MeleeGesture detector;
  auto advance=[&](){q.heartbeatMs+=10;return detector.Update(q,true);};
  other=1;for(int i=0;i<20;++i)check(!advance());
  for(int i=0;i<10;++i){pose.pz-=.02f;check(!advance());}
  trigger=1;other=0;for(int i=0;i<20;++i)check(!advance());
  pose.pz-=.02f;check(!advance());trigger=0;
  for(int i=0;i<5;++i){pose.pz-=.02f;check(!advance());}
  pose.pz=-.25f;trigger=1;for(int i=0;i<20;++i)check(!advance());
  unsigned fired=0;for(int i=0;i<10;++i){pose.pz-=.02f;fired|=advance();}check(fired==hand);
 }
 gProcess=reinterpret_cast<ProcessFn>(&process);gKeyDown=reinterpret_cast<KeyDownFn>(&down);
 InterlockedExchange64(&gMeleeRequest,(1000<<2)|2);ApplyMeleePulse((void*)1,nullptr,true,1001,&p);check(count==1&&actions[0]==1);
 ApplyMeleePulse((void*)1,nullptr,false,1010);check(count==2&&actions[1]==3); // release even on pause
 ApplyMeleePulse((void*)1,nullptr,true,1020);check(count==2);
 InterlockedExchange64(&gMeleeRequest,(1000<<2)|2);ApplyMeleePulse((void*)1,nullptr,true,1500);check(count==2);
 InterlockedExchange64(&gMeleeRequest,1600<<2);ApplyMeleePulse((void*)1,nullptr,true,1600,&p);check(count==3);
 ApplyMeleePulse((void*)1,nullptr,true,1650,&p);check(count==3);
 ApplyMeleePulse((void*)1,nullptr,true,1700,&p);check(count==4&&actions[3]==3);
 p.input.rightTrigger=0;
 InterlockedExchange64(&gMeleeRequest,(1800<<2)|2);ApplyMeleePulse((void*)1,nullptr,true,1801,&p);check(count==4);
 p.input.leftTrigger=0;
 InterlockedExchange64(&gMeleeRequest,1900<<2);ApplyMeleePulse((void*)1,nullptr,true,1901,&p);check(count==5); // B remains independent.
 ApplyMeleePulse((void*)1,nullptr,false,1910,&p);check(count==6);
 gFindKey=reinterpret_cast<FindKeyFn>(&findKey);gProcess=reinterpret_cast<ProcessFn>(&controls);
 InputState input{};input.leftTrigger=1;ApplyNativeButtons((void*)2,nullptr,input,true,.02f);
 check(eventCount==2&&events[0].key==1&&events[0].action==1&&events[1].action==2);
 input={};ApplyNativeButtons((void*)2,nullptr,input,true,.02f);check(eventCount==3&&events[2].key==1&&events[2].action==3);
 input.rightTrigger=1;ApplyNativeButtons((void*)2,nullptr,input,true,.02f);check(events[3].key==2&&events[3].action==1);
 ApplyNativeButtons((void*)2,nullptr,input,false,.02f);check(events[eventCount-1].key==2&&events[eventCount-1].action==3);
 input={};input.leftGrip=input.rightGrip=1;unsigned start=eventCount;ApplyNativeButtons((void*)2,nullptr,input,true,.02f);
 check(events[start].key=='Q'&&events[start+2].key=='E');
 ApplyNativeButtons((void*)2,nullptr,input,false,.02f);
 input={};input.buttons=ButtonY;start=eventCount;ApplyNativeButtons((void*)2,nullptr,input,true,.02f);
 check(eventCount==start+1&&events[start].key==236&&events[start].action==1);
 ApplyNativeButtons((void*)2,nullptr,input,true,.02f);check(eventCount==start+1);
 input={};ApplyNativeButtons((void*)2,nullptr,input,true,.02f);check(events[eventCount-1].action==3);
 start=eventCount;ApplyNativeButtons((void*)2,nullptr,input,true,.02f,1u<<6);check(eventCount==start+1&&events[start].key==236);
 ApplyNativeButtons((void*)2,nullptr,input,false,.02f);
 input.buttons=ButtonLStick;ApplyNativeButtons((void*)2,nullptr,input,true,.02f);check(events[eventCount-1].key==4);
 input={};ApplyNativeButtons((void*)2,nullptr,input,false,.02f);
 input.rightStickY=1;ApplyNativeButtons((void*)2,nullptr,input,true,.02f);check(events[eventCount-1].key=='3');
 input={};ApplyNativeButtons((void*)2,nullptr,input,true,.02f);
 input.rightStickY=-1;ApplyNativeButtons((void*)2,nullptr,input,true,.02f);check(events[eventCount-1].key==237);
 input={};ApplyNativeButtons((void*)2,nullptr,input,false,.02f);
 ApplyNativeButtons((void*)2,nullptr,input,true,.02f,1u<<10);check(events[eventCount-1].key=='G');
 ApplyNativeButtons((void*)2,nullptr,input,false,.02f);
 InterlockedExchange(&gPocketGripMask,2);input.rightGrip=1;start=eventCount;
 ApplyNativeButtons((void*)2,nullptr,input,true,.02f);check(eventCount==start);InterlockedExchange(&gPocketGripMask,0);
 PocketGrenade grenade;Payload gp{};gp.head.flags=gp.left.flags=gp.right.flags=7;gp.head.py=1.7f;
 gp.right.px=.25f;gp.right.py=1.05f;gp.left.px=-.25f;gp.left.py=1.3f;gp.left.pz=-.3f;gp.heartbeatMs=1000;
 auto gt=[&](){gp.heartbeatMs+=10;return grenade.Update(gp,true);};
 for(int i=0;i<10;++i)check(!gt());
 gp.input.rightGrip=1;check(!gt()&&(grenade.suppress&2));
 for(int i=0;i<20;++i)check(!gt());
 for(int i=0;i<15;++i){gp.right.pz-=.02f;check(!gt());}
 gp.input.rightGrip=0;check(gt());check(!gt());
 gp.heartbeatMs+=1000;check(!gt());gp.right.pz=0;for(int i=0;i<10;++i)check(!gt());
 gp.input.rightGrip=1;check(!gt());for(int i=0;i<20;++i)check(!gt());
 gp.input.rightGrip=0;check(!gt()); // putting it down is not a throw
 check(!grenade.Update(gp,false)&&grenade.suppress==0);
 puts("PASS: either-hand gestures, one attack per swing, retraction/walking/turning/tracking guards, native press and release, stale request rejection");
}
