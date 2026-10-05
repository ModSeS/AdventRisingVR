#include "proxy/D3D9Hook.h"
#include "shared/SharedState.h"
#include "proxy/StereoState.h"
#include "proxy/GameState.h"
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
namespace arvr {
static uint64_t testPair=0;
bool ReadBridgePayload(Payload& p){if(!testPair)return false;p.heartbeatMs=GetTickCount64();p.predictedDisplayTime=123;p.sessionState=5;p.eyeFov[0]={-.855211f,.785398f,.837758f,-.872665f,1};p.eyeFov[1]={-.785398f,.855211f,.837758f,-.872665f,1};return true;}
void Log(const char* fmt,...){}
bool GetGameProjection(float& x,float& y,float& ox,float& oy){x=.43f;y=.76f;ox=oy=0;return testPair!=0;}
uint64_t RenderSnapshotId(){return testPair;}
}
static void check(bool good,const char* text){if(!good){printf("FAIL: %s\n",text);exit(1);}}
static IDirect3DDevice9* device(IDirect3D9* d,HWND w,D3DPRESENT_PARAMETERS& pp){
 pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=w;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
 IDirect3DDevice9* dev=nullptr;
 HRESULT hr=d->CreateDevice(0,D3DDEVTYPE_HAL,w,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&dev);
 if(FAILED(hr))printf("CreateDevice HRESULT=0x%08lX\n",(unsigned long)hr);
 check(SUCCEEDED(hr),"create D3D9 device");return dev;
}
int main(){
 wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);wchar_t* slash=wcsrchr(path,L'\\');if(slash)*slash=0;SetCurrentDirectoryW(path);
 FILE* report=nullptr;freopen_s(&report,"capture-test-result.txt","w",stdout);setvbuf(stdout,nullptr,_IONBF,0);
 WritePrivateProfileStringW(L"Stereo",L"InstallCaptureHook",L"1",L".\\AdventRisingVR.ini");
 WritePrivateProfileStringW(L"Stereo",L"Mode",L"0",L".\\AdventRisingVR.ini");
 WritePrivateProfileStringW(L"Debug",L"DumpFirstFrame",L"0",L".\\AdventRisingVR.ini");
 WritePrivateProfileStringW(L"Debug",L"CaptureWithoutRuntime",L"1",L".\\AdventRisingVR.ini");
 HWND w=CreateWindowExW(0,L"STATIC",L"Capture regression",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 check(w!=nullptr,"hidden window");IDirect3D9* d=Direct3DCreate9(D3D_SDK_VERSION);check(d!=nullptr,"D3D9");
 D3DPRESENT_PARAMETERS pp{};auto before=device(d,w,pp);
 check(arvr::InstallD3D9Hooks(),"install hooks");check(arvr::InstallD3D9Hooks(),"idempotent installation");
 HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,arvr::kFrameMappingName);check(mapping!=nullptr,"frame mapping");
 auto frame=(arvr::FrameTransportHeader*)MapViewOfFile(mapping,FILE_MAP_READ,0,0,arvr::kFrameMappingSize);check(frame!=nullptr,"mapped frame");
 auto after=device(d,w,pp);IDirect3DDevice9* devices[]={before,after};
 for(auto dev:devices){
  D3DMATRIX mat{};mat._11=mat._22=mat._33=mat._44=1;
  check(SUCCEEDED(dev->SetTransform(D3DTS_WORLD,&mat)),"forward SetTransform");
  for(int i=0;i<3;i++){
   check(SUCCEEDED(dev->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(255,0,0),1,0)),"clear");
   check(SUCCEEDED(dev->Present(nullptr,nullptr,nullptr,nullptr)),"Present returns without recursion");
   check(frame->width[0]==64 && frame->height[0]==64 && frame->frameId[0]>0,"capture dimensions and frame id");
   auto pixel=(const unsigned char*)(frame+1);check(pixel[0]==0&&pixel[1]==0&&pixel[2]==255,"captured actual red pixel");
  }
  check(SUCCEEDED(dev->Reset(&pp)),"Reset returns without recursion");
 }
 // Reproduce Advent's ordering: Draw/EndScene twice, then one desktop Present.
 WritePrivateProfileStringW(L"Stereo",L"Mode",L"2",L".\\AdventRisingVR.ini");
 arvr::MarkCameraGameplay(true);arvr::testPair=1;
 auto oldId=frame->frameId[0];
 for(int eye=0;eye<2;++eye){
  arvr::SetForcedStereoEye((arvr::StereoEye)eye);
  check(SUCCEEDED(after->BeginScene()),"stereo BeginScene");
  check(SUCCEEDED(after->Clear(0,nullptr,D3DCLEAR_TARGET,eye?D3DCOLOR_XRGB(0,0,255):D3DCOLOR_XRGB(255,0,0),1,0)),"stereo clear");
  check(SUCCEEDED(after->EndScene()),"stereo EndScene");
  arvr::CaptureRenderedStereoEye();
  if(!eye)check(frame->frameId[0]==oldId,"left eye staged without exposing half a pair");
 }
 check(frame->sceneMode==arvr::SceneGameplayVR && frame->frameId[0]>oldId && frame->frameId[0]==frame->frameId[1],"stereo pair published before desktop Present");
 auto left=(const unsigned char*)(frame+1);auto right=left+arvr::kFrameEyeBytes;
 check(left[2]==255 && left[0]==0 && right[0]==255 && right[2]==0,"independent red and blue eye pixels");
 oldId=frame->frameId[0];arvr::testPair=0;arvr::SetForcedStereoEye(arvr::StereoEyeMono);
 check(SUCCEEDED(after->Present(nullptr,nullptr,nullptr,nullptr)),"delayed desktop Present");
 check(frame->frameId[0]==oldId && frame->sceneMode==arvr::SceneGameplayVR,"desktop Present preserves stereo pair");
 after->Release();before->Release();d->Release();DestroyWindow(w);UnmapViewOfFile(frame);CloseHandle(mapping);
 puts("PASS: hooks install once; existing/new D3D9 devices; six real captures; transforms; two resets");
 puts("PASS: two real stereo eye captures at EndScene/Draw, atomic pair, delayed desktop Present does not erase stereo");
}
