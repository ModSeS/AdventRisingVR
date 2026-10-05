#include "proxy/SharedReader.h"
#include <cstdio>
#include <cstdlib>
namespace arvr {void Log(const char*,...){};}
using namespace arvr;
static void check(bool v,const char* msg){if(!v){std::printf("FAIL: %s\n",msg);std::exit(1);}}
int main(){
    HANDLE m=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SharedState),kMappingName);
    check(m && GetLastError()!=ERROR_ALREADY_EXISTS,"isolated mapping");
    auto s=(SharedState*)MapViewOfFile(m,FILE_MAP_ALL_ACCESS,0,0,sizeof(SharedState));check(s!=nullptr,"mapping view");
    *s=SharedState{};Payload p{};check(!ReadBridgePayload(p),"zero heartbeat rejected");
    s->payload.heartbeatMs=GetTickCount64();s->payload.input.buttons=ButtonA;
    check(ReadBridgePayload(p)&&p.input.buttons==ButtonA,"fresh payload");
    s->sequence=1;check(!ReadBridgePayload(p),"incomplete write rejected");s->sequence=2;
    BeginRenderSnapshot();s->payload.input.buttons=ButtonB;
    check(ReadBridgePayload(p)&&p.input.buttons==ButtonA,"both eyes share frozen pose");EndRenderSnapshot();
    check(ReadBridgePayload(p)&&p.input.buttons==ButtonB,"new draw reads latest pose");
    s->payload.heartbeatMs=GetTickCount64()-1000;check(!ReadBridgePayload(p),"dead bridge rejected");
    std::puts("PASS: 6 shared-memory regression checks");UnmapViewOfFile(s);CloseHandle(m);
}
