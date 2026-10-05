#include "../src/proxy/D3D9Hook.cpp"
#include <cstdlib>
namespace arvr {
static uint64_t testPair=1;
static Payload testPose{};
uint64_t RenderSnapshotId(){return testPair;}
bool ReadBridgePayload(Payload& p){p=testPose;return true;}
bool GetGameProjection(float&,float&,float&,float&){return false;}
void Log(const char*,...){}
}
static void check(bool ok){if(!ok)std::exit(1);}
int main(){
 using namespace arvr;
 wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);*wcsrchr(path,L'\\')=0;SetCurrentDirectoryW(path);
 WritePrivateProfileStringW(L"Stereo",L"Mode",L"2",L".\\AdventRisingVR.ini");
 WritePrivateProfileStringW(L"Debug",L"DumpFirstFrame",L"0",L".\\AdventRisingVR.ini");
 for(unsigned w=1;w<=17;++w){
  const unsigned pitch=w*4+7;std::vector<uint8_t> input(pitch*3+1),output(w*3*4+6,0xCD);
  for(unsigned i=0;i<input.size();++i)input[i]=(uint8_t)(i*37);
  ConvertFrame(output.data()+3,w,3,input.data()+1,w,3,pitch,D3DFMT_X8R8G8B8);
  for(unsigned y=0;y<3;++y)for(unsigned x=0;x<w;++x)for(unsigned k=0;k<4;++k)
   check(output[3+(y*w+x)*4+k]==(k==3?255:input[1+y*pitch+x*4+k]));
  check(output[0]==0xCD&&output.back()==0xCD);
 }
 testPose.predictedDisplayTime=123;
 std::vector<uint8_t> left(16*16*4,31),right(16*16*4,127);
 check(EnsureFrameShare());
 WriteFrameBoth(left.data(),16,16,64,D3DFMT_A8R8G8B8);
 auto id=gFrameHeader->frameId[0];
 WriteFrameOne(0,left.data(),16,16,64,D3DFMT_A8R8G8B8);
 check(gFrameHeader->sceneMode==SceneFlatScreen && gFrameHeader->frameId[0]==id);
 WriteFrameOne(1,right.data(),16,16,64,D3DFMT_A8R8G8B8);
 check(gFrameHeader->sceneMode==SceneGameplayVR && gFrameHeader->frameId[0]==id+1 && gFrameHeader->frameId[1]==id+1);
 check(EyePtr(0)[0]==31 && EyePtr(1)[0]==127 && !(gFrameHeader->sequence&1));
 id=gFrameHeader->frameId[0];
 ++testPair;WriteFrameOne(0,left.data(),16,16,64,D3DFMT_A8R8G8B8);
 ++testPair;WriteFrameOne(1,right.data(),16,16,64,D3DFMT_A8R8G8B8);
 check(gFrameHeader->frameId[0]==id && gFrameHeader->frameId[1]==id);
 WriteFrameOne(0,left.data(),16,16,64,D3DFMT_A8R8G8B8);
 ++testPose.predictedDisplayTime;WriteFrameOne(1,right.data(),16,16,64,D3DFMT_A8R8G8B8);
 check(gFrameHeader->frameId[0]==id);
 puts("PASS: complete stereo pair committed atomically; incomplete and mismatched pairs do not replace it");
}
