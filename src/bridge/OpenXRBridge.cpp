#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "BridgeLog.h"
#include "GpuBlit.h"
#include "HudCanvas.h"
#include "../shared/SharedState.h"
#include "../shared/FrameAge.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>
#include <cstdint>

namespace arvr {

static SharedState* gShared=nullptr;
static HANDLE gMap=nullptr;
static FrameTransportHeader* gFrameHeader=nullptr;
static HANDLE gFrameMap=nullptr;
static uint8_t* gFrameBase=nullptr;

static void Publish(const Payload&p){
    InterlockedIncrement(&gShared->sequence);
    MemoryBarrier();
    gShared->payload=p;gShared->payload.heartbeatMs=GetTickCount64();
    MemoryBarrier();
    InterlockedIncrement(&gShared->sequence);
}

static bool CreateShared(){
    gMap=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SharedState),kMappingName);
    if(!gMap)return false;
    gShared=(SharedState*)MapViewOfFile(gMap,FILE_MAP_ALL_ACCESS,0,0,sizeof(SharedState));
    if(!gShared)return false;
    *gShared=SharedState{};
    gShared->magic=kMagic;
    gShared->version=kVersion;
    BridgeLog("Shared memory ready");
    return true;
}

static bool OpenFrameShare(){
    if(gFrameHeader) return true;
    gFrameMap=OpenFileMappingW(FILE_MAP_READ,FALSE,kFrameMappingName);
    if(!gFrameMap) return false;
    gFrameHeader=(FrameTransportHeader*)MapViewOfFile(gFrameMap,FILE_MAP_READ,0,0,kFrameMappingSize);
    if(!gFrameHeader){CloseHandle(gFrameMap);gFrameMap=nullptr;return false;}
    if(gFrameHeader->magic!=kFrameMagic || gFrameHeader->version!=kVersion){
        UnmapViewOfFile(gFrameHeader);
        CloseHandle(gFrameMap);
        gFrameHeader=nullptr;
        gFrameMap=nullptr;
        return false;
    }
    gFrameBase=(uint8_t*)gFrameHeader + sizeof(FrameTransportHeader);
    BridgeLog("Frame mapping attached");
    return true;
}

static uint8_t* EyePtr(uint32_t eye){
    return gFrameBase + (eye==StereoEyeRight ? kFrameEyeBytes : 0);
}

static Pose ToPose(const XrSpaceLocation&l){
    Pose p{};
    if(l.locationFlags&XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)p.flags|=PoseOrientationValid;
    if(l.locationFlags&XR_SPACE_LOCATION_POSITION_VALID_BIT)p.flags|=PosePositionValid;
    if((l.locationFlags&(XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT))!=0)p.flags|=PoseTracked;
    p.px=l.pose.position.x;p.py=l.pose.position.y;p.pz=l.pose.position.z;
    p.qx=l.pose.orientation.x;p.qy=l.pose.orientation.y;p.qz=l.pose.orientation.z;p.qw=l.pose.orientation.w;
    return p;
}

static IDXGIAdapter1* FindAdapter(const LUID& wanted){
    IDXGIFactory1*f=nullptr;
    if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void**)&f)))return nullptr;
    IDXGIAdapter1*a=nullptr;
    for(UINT i=0;f->EnumAdapters1(i,&a)!=DXGI_ERROR_NOT_FOUND;i++){
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);
        if(d.AdapterLuid.HighPart==wanted.HighPart&&d.AdapterLuid.LowPart==wanted.LowPart){f->Release();return a;}
        a->Release();a=nullptr;
    }
    f->Release();return nullptr;
}

struct EyeFrameCopy {
    bool cacheValid=false;
    uint64_t capturedMs{};
    uint32_t width{}, height{}, pitch{}, frameId{};
    Pose renderPose{};EyeFov renderFov{};int64_t poseTime{};
    uint32_t sourceWidth{}, sourceHeight{}, generation{};
    SceneMode sceneMode{SceneFlatScreen};
    std::vector<uint8_t> pixels;
};

static bool ReadEyeFrame(uint32_t eye, EyeFrameCopy& out){
    if(!OpenFrameShare()) return false;
    for(int n=0;n<4;n++){
        LONG a=gFrameHeader->sequence;
        if(a&1) continue;
        MemoryBarrier();
        uint32_t w=gFrameHeader->width[eye], h=gFrameHeader->height[eye], p=gFrameHeader->pitch[eye], id=gFrameHeader->frameId[eye];
        if(!w||!h||w>kFrameMaxWidth||h>kFrameMaxHeight||p<w*4||p>kFrameEyeBytes/h||GetTickCount64()-gFrameHeader->capturedMs[eye]>500) return false;
        if(out.cacheValid && out.frameId==id && out.capturedMs==gFrameHeader->capturedMs[eye] && out.generation==gFrameHeader->resolutionGeneration && out.sceneMode==(SceneMode)gFrameHeader->sceneMode){
            MemoryBarrier();if(a==gFrameHeader->sequence)return true;
            continue;
        }
        out.cacheValid=false;
        const uint8_t* src=EyePtr(eye);
        out.capturedMs=gFrameHeader->capturedMs[eye];
        out.renderPose=gFrameHeader->renderPose[eye];out.renderFov=gFrameHeader->renderFov[eye];out.poseTime=gFrameHeader->poseTime[eye];
        out.width=w;out.height=h;out.pitch=p;out.frameId=id;
        out.sourceWidth=gFrameHeader->sourceWidth;out.sourceHeight=gFrameHeader->sourceHeight;out.generation=gFrameHeader->resolutionGeneration;
        out.sceneMode=(SceneMode)gFrameHeader->sceneMode;
        out.pixels.resize(size_t(h)*p);
        std::memcpy(out.pixels.data(),src,out.pixels.size());
        MemoryBarrier();
        LONG b=gFrameHeader->sequence;
        if(a==b && !(b&1)){out.cacheValid=true;return true;}
    }
    return false;
}

static bool IsRGBAFormat(int64_t f){
    return f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}
static bool IsBGRAFormat(int64_t f){
    return f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
static const char* FormatName(int64_t f){
    switch((DXGI_FORMAT)f){
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        default: return "other";
    }
}

struct EyeSwapchain {
    bool transferDst=false;
    XrSwapchain handle{XR_NULL_HANDLE};
    int32_t width{},height{};
    int64_t format{};
    std::vector<XrSwapchainImageD3D11KHR> images;
};

static bool CreateEyeSwapchain(XrSession session,int32_t width,int32_t height,int64_t format,EyeSwapchain& out){
    const XrSwapchainUsageFlags usageAttempts[]={
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
    };
    for(XrSwapchainUsageFlags usage:usageAttempts){
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.arraySize=1;
        sci.format=format;
        sci.width=width;
        sci.height=height;
        sci.mipCount=1;
        sci.faceCount=1;
        sci.sampleCount=1;
        sci.usageFlags=usage;
        XrResult cr=xrCreateSwapchain(session,&sci,&out.handle);
        BridgeLog("xrCreateSwapchain format=%s(%lld) %dx%d usage=0x%llX -> %d",FormatName(format),(long long)format,width,height,(unsigned long long)usage,(int)cr);
        if(XR_FAILED(cr)) continue;
        out.width=width;out.height=height;out.format=format;out.transferDst=(usage&XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)!=0;
        uint32_t count=0;
        XrResult er=xrEnumerateSwapchainImages(out.handle,0,&count,nullptr);
        if(XR_FAILED(er)||!count){xrDestroySwapchain(out.handle);out.handle=XR_NULL_HANDLE;continue;}
        out.images.resize(count);
        for(auto& img:out.images) img={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        er=xrEnumerateSwapchainImages(out.handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(out.images.data()));
        if(XR_FAILED(er)){xrDestroySwapchain(out.handle);out.handle=XR_NULL_HANDLE;out.images.clear();continue;}
        return true;
    }
    return false;
}

static std::vector<int64_t> GetSwapchainFormats(XrSession session){
    uint32_t count=0;
    XrResult r=xrEnumerateSwapchainFormats(session,0,&count,nullptr);
    if(XR_FAILED(r)||!count){BridgeLog("xrEnumerateSwapchainFormats(count) failed: %d",(int)r);return {};}
    std::vector<int64_t> formats(count);
    r=xrEnumerateSwapchainFormats(session,count,&count,formats.data());
    if(XR_FAILED(r)){BridgeLog("xrEnumerateSwapchainFormats(list) failed: %d",(int)r);return {};}
    BridgeLog("OpenXR swapchain formats: count=%u",count);
    for(uint32_t i=0;i<count;++i) BridgeLog("  format[%u]=%s (%lld)",i,FormatName(formats[i]),(long long)formats[i]);
    return formats;
}

static bool CreateStereoSwapchains(XrSession session,const std::vector<XrViewConfigurationView>& views,std::array<EyeSwapchain,2>& eyes){
    if(views.size()<2) return false;
    const auto supported=GetSwapchainFormats(session);
    if(supported.empty()) return false;
    const int64_t preferred[]={
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM
    };
    for(int64_t format:preferred){
        if(std::find(supported.begin(),supported.end(),format)==supported.end()) continue;
        BridgeLog("Trying stereo format %s (%lld)",FormatName(format),(long long)format);
        if(!CreateEyeSwapchain(session,views[0].recommendedImageRectWidth,views[0].recommendedImageRectHeight,format,eyes[0])) continue;
        if(!CreateEyeSwapchain(session,views[1].recommendedImageRectWidth,views[1].recommendedImageRectHeight,format,eyes[1])){
            xrDestroySwapchain(eyes[0].handle);eyes[0]=EyeSwapchain{};
            continue;
        }
        BridgeLog("Stereo swapchains created: format=%s L=%dx%d R=%dx%d",FormatName(format),eyes[0].width,eyes[0].height,eyes[1].width,eyes[1].height);
        return true;
    }
    BridgeLog("No compatible 32-bit RGBA/BGRA swapchain format could be created; tracking will continue without submitted image");
    return false;
}

static float ActionFloat(XrSession session,XrAction action,XrPath subaction){
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};gi.action=action;gi.subactionPath=subaction;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};if(XR_FAILED(xrGetActionStateFloat(session,&gi,&st))||!st.isActive)return 0.0f;return st.currentState;
}
static XrVector2f ActionVec2(XrSession session,XrAction action,XrPath subaction){
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};gi.action=action;gi.subactionPath=subaction;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};if(XR_FAILED(xrGetActionStateVector2f(session,&gi,&st))||!st.isActive)return {0,0};return st.currentState;
}
static bool ActionBool(XrSession session,XrAction action){
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};gi.action=action;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};if(XR_FAILED(xrGetActionStateBoolean(session,&gi,&st))||!st.isActive)return false;return st.currentState!=XR_FALSE;
}

static float IniFloat(const wchar_t*section,const wchar_t*key,float def){
    wchar_t b[64]{};GetPrivateProfileStringW(section,key,L"",b,64,L".\\AdventRisingVR.ini");return *b?(float)_wtof(b):def;
}

int RunOpenXR(){
    if(!CreateShared()){BridgeLog("Shared memory failed: %lu",GetLastError());return 2;}
    HudChannel hudChannel;HudCanvas hudCanvas;EyeSwapchain hudSwap{};GpuBlit hudBlit;
    Payload out{};out.bridgePid=GetCurrentProcessId();Publish(out);

    const char* exts[]={XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName,"Advent Rising VR Bridge");
    strcpy_s(ici.applicationInfo.engineName,"AdventRisingVR");
    ici.applicationInfo.applicationVersion=41;
    ici.applicationInfo.engineVersion=41;
    ici.applicationInfo.apiVersion=XR_API_VERSION_1_0;
    ici.enabledExtensionCount=1;ici.enabledExtensionNames=exts;
    XrInstance inst=XR_NULL_HANDLE;
    XrResult r=xrCreateInstance(&ici,&inst);
    if(XR_FAILED(r)){BridgeLog("xrCreateInstance failed: %d",r);return 3;}
    BridgeLog("OpenXR instance created");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};sgi.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys{};
    if(XR_FAILED(r=xrGetSystem(inst,&sgi,&sys))){BridgeLog("xrGetSystem failed: %d",r);return 4;}
    BridgeLog("OpenXR HMD system acquired");

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq=nullptr;
    xrGetInstanceProcAddr(inst,"xrGetD3D11GraphicsRequirementsKHR",(PFN_xrVoidFunction*)&getReq);
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if(!getReq||XR_FAILED(r=getReq(inst,sys,&req))){BridgeLog("D3D11 requirements failed: %d",r);return 5;}
    IDXGIAdapter1* adapter=FindAdapter(req.adapterLuid);
    if(!adapter){BridgeLog("Required DXGI adapter not found");return 6;}
    ID3D11Device*dev=nullptr;ID3D11DeviceContext*ctx=nullptr;D3D_FEATURE_LEVEL got{};
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    HRESULT hr=D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,_countof(levels),D3D11_SDK_VERSION,&dev,&got,&ctx);
    adapter->Release();
    if(FAILED(hr)){BridgeLog("D3D11CreateDevice failed: 0x%08X",(unsigned)hr);return 7;}
    BridgeLog("D3D11 device created, featureLevel=0x%X",(unsigned)got);

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};gb.device=dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};sci.next=&gb;sci.systemId=sys;
    XrSession session{};
    if(XR_FAILED(r=xrCreateSession(inst,&sci,&session))){BridgeLog("xrCreateSession failed: %d",r);return 8;}
    BridgeLog("OpenXR session created");

    XrPosef ident{{0,0,0,1},{0,0,0}};
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};rs.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;rs.poseInReferenceSpace=ident;
    XrSpace local{};
    if(XR_FAILED(r=xrCreateReferenceSpace(session,&rs,&local))){BridgeLog("local space failed: %d",r);return 9;}
    rs.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;
    XrSpace view{};
    if(XR_FAILED(r=xrCreateReferenceSpace(session,&rs,&view))){BridgeLog("view space failed: %d",r);return 10;}

    XrPath leftPath{},rightPath{};
    xrStringToPath(inst,"/user/hand/left",&leftPath);xrStringToPath(inst,"/user/hand/right",&rightPath);
    XrPath subs[]={leftPath,rightPath};
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};strcpy_s(asci.actionSetName,"gameplay");strcpy_s(asci.localizedActionSetName,"Gameplay");asci.priority=0;
    XrActionSet aset{};if(XR_FAILED(xrCreateActionSet(inst,&asci,&aset)))return 13;

    auto makeAction=[&](XrActionType type,const char*name,const char*label,const XrPath*paths,uint32_t count){
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};ci.actionType=type;strcpy_s(ci.actionName,name);strcpy_s(ci.localizedActionName,label);ci.countSubactionPaths=count;ci.subactionPaths=paths;
        XrAction a{XR_NULL_HANDLE};XrResult rr=xrCreateAction(aset,&ci,&a);if(XR_FAILED(rr))BridgeLog("xrCreateAction %s failed: %d",name,(int)rr);return a;
    };
    XrAction hand=makeAction(XR_ACTION_TYPE_POSE_INPUT,"hand_pose","Hand pose",subs,2);
    XrAction stick=makeAction(XR_ACTION_TYPE_VECTOR2F_INPUT,"thumbstick","Thumbstick",subs,2);
    XrAction trigger=makeAction(XR_ACTION_TYPE_FLOAT_INPUT,"trigger","Trigger",subs,2);
    XrAction squeeze=makeAction(XR_ACTION_TYPE_FLOAT_INPUT,"squeeze","Grip",subs,2);
    XrAction btnA=makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,"button_a","A",nullptr,0);
    XrAction btnB=makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,"button_b","B",nullptr,0);
    XrAction btnX=makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,"button_x","X",nullptr,0);
    XrAction btnY=makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,"button_y","Y",nullptr,0);
    XrAction lStickClick=makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,"left_stick_click","Left stick click",nullptr,0);
    XrAction rStickClick=makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,"right_stick_click","Right stick click",nullptr,0);

    if(!hand||!stick||!trigger||!squeeze||!btnA||!btnB||!btnX||!btnY||!lStickClick||!rStickClick)return 14;
    XrPath profile{},lp{},rp{},ls{},rsPath{},lt{},rt{},lg{},rg{},pa{},pb{},px{},py{},plc{},prc{};
    xrStringToPath(inst,"/interaction_profiles/oculus/touch_controller",&profile);
    xrStringToPath(inst,"/user/hand/left/input/grip/pose",&lp);xrStringToPath(inst,"/user/hand/right/input/grip/pose",&rp);
    xrStringToPath(inst,"/user/hand/left/input/thumbstick",&ls);xrStringToPath(inst,"/user/hand/right/input/thumbstick",&rsPath);
    xrStringToPath(inst,"/user/hand/left/input/trigger/value",&lt);xrStringToPath(inst,"/user/hand/right/input/trigger/value",&rt);
    xrStringToPath(inst,"/user/hand/left/input/squeeze/value",&lg);xrStringToPath(inst,"/user/hand/right/input/squeeze/value",&rg);
    xrStringToPath(inst,"/user/hand/right/input/a/click",&pa);xrStringToPath(inst,"/user/hand/right/input/b/click",&pb);
    xrStringToPath(inst,"/user/hand/left/input/x/click",&px);xrStringToPath(inst,"/user/hand/left/input/y/click",&py);
    xrStringToPath(inst,"/user/hand/left/input/thumbstick/click",&plc);xrStringToPath(inst,"/user/hand/right/input/thumbstick/click",&prc);
    std::vector<XrActionSuggestedBinding> binds={{hand,lp},{hand,rp},{stick,ls},{stick,rsPath},{trigger,lt},{trigger,rt},{squeeze,lg},{squeeze,rg},{btnA,pa},{btnB,pb},{btnX,px},{btnY,py},{lStickClick,plc},{rStickClick,prc}};
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};sb.interactionProfile=profile;sb.countSuggestedBindings=(uint32_t)binds.size();sb.suggestedBindings=binds.data();
    XrResult bindResult=xrSuggestInteractionProfileBindings(inst,&sb);BridgeLog("Suggest Touch bindings=%d count=%u",(int)bindResult,(unsigned)binds.size());
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};ai.countActionSets=1;ai.actionSets=&aset;if(XR_FAILED(xrAttachSessionActionSets(session,&ai)))return 15;
    XrActionSpaceCreateInfo asp{XR_TYPE_ACTION_SPACE_CREATE_INFO};asp.action=hand;asp.poseInActionSpace=ident;asp.subactionPath=leftPath;
    XrSpace leftSpace{};if(XR_FAILED(xrCreateActionSpace(session,&asp,&leftSpace)))return 16;asp.subactionPath=rightPath;XrSpace rightSpace{};if(XR_FAILED(xrCreateActionSpace(session,&asp,&rightSpace)))return 17;

    uint32_t viewCount=0;
    r=xrEnumerateViewConfigurationViews(inst,sys,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&viewCount,nullptr);
    std::vector<XrViewConfigurationView> viewCfg;
    if(XR_SUCCEEDED(r)&&viewCount){
        viewCfg.resize(viewCount);
        for(auto& v:viewCfg) v={XR_TYPE_VIEW_CONFIGURATION_VIEW};
        r=xrEnumerateViewConfigurationViews(inst,sys,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,viewCount,&viewCount,viewCfg.data());
    }
    BridgeLog("View configuration count=%u result=%d",viewCount,(int)r);
    for(uint32_t i=0;i<viewCount;++i){BridgeLog("  view[%u] recommended=%ux%u max=%ux%u samples=%u",i,viewCfg[i].recommendedImageRectWidth,viewCfg[i].recommendedImageRectHeight,viewCfg[i].maxImageRectWidth,viewCfg[i].maxImageRectHeight,viewCfg[i].recommendedSwapchainSampleCount);}

    std::array<EyeSwapchain,2> eyes{};
    bool renderReady=(viewCount>=2)&&CreateStereoSwapchains(session,viewCfg,eyes);
    BridgeLog("OpenXR rendering path: %s",renderReady?"ready":"disabled; tracking-only mode");

    bool running=false,quit=false;
    XrSessionState state=XR_SESSION_STATE_UNKNOWN;
    BridgeLog("OpenXR bridge ready; waiting for session state");
    GpuBlit eyeBlit[2];
    if(!eyeBlit[0].Init(dev)||!eyeBlit[1].Init(dev))return 11;
    GpuBlit blit;if(!blit.Init(dev)){BridgeLog("GPU blit initialization failed");return 11;}
    const bool cacheFrames=GetPrivateProfileIntW(L"Render",L"CacheBridgeFrames",1,L".\\AdventRisingVR.ini")!=0;
    const bool smoothEdges=GetPrivateProfileIntW(L"Render",L"EdgeSmoothing",1,L".\\AdventRisingVR.ini")!=0;
    BridgeLog("Gameplay edge smoothing: %s",smoothEdges?"on":"off");
    std::array<EyeFrameCopy,2> lastPair{},captured{};ULONGLONG pairTick=0;

    std::vector<XrView> xrViews(viewCount?viewCount:2);
    for(auto& v:xrViews) v={XR_TYPE_VIEW};
    bool trackingLogged=false;
    bool controllersLogged=false;
    bool fovLogged=false;
    bool frameLogged=false;
    int lastSceneLogged=-1;ULONGLONG nextDiagnostics=0;
    ULONGLONG nextRenderReport=0;unsigned framesWithLayers=0,framesWithoutLayers=0;


    while(!quit){
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while(xrPollEvent(inst,&ev)==XR_SUCCESS){
            if(ev.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){
                auto&e=*reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                state=e.state;out.sessionState=(uint32_t)state;
                BridgeLog("Session state=%u",(unsigned)state);
                if(state==XR_SESSION_STATE_READY&&!running){
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};bi.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult br=xrBeginSession(session,&bi);
                    BridgeLog("xrBeginSession=%d",(int)br);
                    if(XR_SUCCEEDED(br))running=true;
                }else if(state==XR_SESSION_STATE_STOPPING&&running){
                    xrEndSession(session);running=false;
                }else if(state==XR_SESSION_STATE_EXITING||state==XR_SESSION_STATE_LOSS_PENDING){
                    quit=true;
                }
            }
            ev={XR_TYPE_EVENT_DATA_BUFFER};
        }
        if(quit)break;
        if(!running){out.head={};out.left={};out.right={};out.input={};out.eyeFov[0]={};out.eyeFov[1]={};Publish(out);Sleep(20);continue;}

        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};XrFrameState fs{XR_TYPE_FRAME_STATE};
        if(XR_FAILED(xrWaitFrame(session,&wi,&fs))){Sleep(10);continue;}
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};if(XR_FAILED(xrBeginFrame(session,&fbi)))break;
        out.predictedDisplayTime=fs.predictedDisplayTime;

        XrActiveActionSet active{aset,XR_NULL_PATH};
        XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO};sy.countActiveActionSets=1;sy.activeActionSets=&active;
        const bool focused=state==XR_SESSION_STATE_FOCUSED && xrSyncActions(session,&sy)==XR_SUCCESS;
        XrSpaceLocation h{XR_TYPE_SPACE_LOCATION},l{XR_TYPE_SPACE_LOCATION},rr{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(view,local,fs.predictedDisplayTime,&h);
        xrLocateSpace(leftSpace,local,fs.predictedDisplayTime,&l);
        xrLocateSpace(rightSpace,local,fs.predictedDisplayTime,&rr);
        out.head=ToPose(h);out.left=ToPose(l);out.right=ToPose(rr);
        XrVector2f lv=ActionVec2(session,stick,leftPath),rv=ActionVec2(session,stick,rightPath);
        out.input.leftStickX=lv.x;out.input.leftStickY=lv.y;out.input.rightStickX=rv.x;out.input.rightStickY=rv.y;
        out.input.leftTrigger=ActionFloat(session,trigger,leftPath);out.input.rightTrigger=ActionFloat(session,trigger,rightPath);
        out.input.leftGrip=ActionFloat(session,squeeze,leftPath);out.input.rightGrip=ActionFloat(session,squeeze,rightPath);
        out.input.buttons=0;if(ActionBool(session,btnA))out.input.buttons|=ButtonA;if(ActionBool(session,btnB))out.input.buttons|=ButtonB;if(ActionBool(session,btnX))out.input.buttons|=ButtonX;if(ActionBool(session,btnY))out.input.buttons|=ButtonY;if(ActionBool(session,lStickClick))out.input.buttons|=ButtonLStick;if(ActionBool(session,rStickClick))out.input.buttons|=ButtonRStick;

        for(int side=0;side<2;++side){
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};gi.action=hand;gi.subactionPath=side?rightPath:leftPath;
            XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
            if(!focused || XR_FAILED(xrGetActionStatePose(session,&gi,&ps)) || !ps.isActive){if(side)out.right={};else out.left={};}
        }
        if(!focused)out.input={};
        // Locate the two OpenXR views before publishing the shared payload so the
        // 32-bit renderer can build the same asymmetric per-eye projection that
        // SteamVR expects for this predicted display time.
        XrViewLocateInfo sharedVli{XR_TYPE_VIEW_LOCATE_INFO};
        sharedVli.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        sharedVli.displayTime=fs.predictedDisplayTime;
        sharedVli.space=local;
        XrViewState sharedVs{XR_TYPE_VIEW_STATE};
        uint32_t sharedVc=0;
        XrResult sharedVr=xrLocateViews(session,&sharedVli,&sharedVs,(uint32_t)xrViews.size(),&sharedVc,xrViews.data());
        if(XR_SUCCEEDED(sharedVr) && sharedVc==2 && (sharedVs.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (sharedVs.viewStateFlags&XR_VIEW_STATE_POSITION_VALID_BIT)){
            for(uint32_t eye=0;eye<2;++eye){
                out.eyeFov[eye].angleLeft=xrViews[eye].fov.angleLeft;
                out.eyeFov[eye].angleRight=xrViews[eye].fov.angleRight;
                out.eyeFov[eye].angleUp=xrViews[eye].fov.angleUp;
                out.eyeFov[eye].angleDown=xrViews[eye].fov.angleDown;
                out.eyeFov[eye].valid=1;
                const auto& ep=xrViews[eye].pose;
                out.eyes[eye]={PoseOrientationValid|PosePositionValid,ep.position.x,ep.position.y,ep.position.z,ep.orientation.x,ep.orientation.y,ep.orientation.z,ep.orientation.w};
            }
        } else {
            out.eyeFov[0].valid=out.eyeFov[1].valid=0;
        }
        Publish(out);
        if(GetTickCount64()>=nextDiagnostics){
            nextDiagnostics=GetTickCount64()+5000;
            BridgeLog("Tracking status: state=%u head=0x%X left=0x%X right=0x%X buttons=0x%X",out.sessionState,out.head.flags,out.left.flags,out.right.flags,out.input.buttons);
            for(XrPath path:subs){
                XrInteractionProfileState profileState{XR_TYPE_INTERACTION_PROFILE_STATE};
                if(XR_SUCCEEDED(xrGetCurrentInteractionProfile(session,path,&profileState)) && profileState.interactionProfile){
                    char text[XR_MAX_PATH_LENGTH]{};uint32_t written=0;
                    if(XR_SUCCEEDED(xrPathToString(inst,profileState.interactionProfile,sizeof(text),&written,text)))BridgeLog("%s controller profile: %s",path==leftPath?"Left":"Right",text);
                }
            }
        }
        if(!trackingLogged && (out.head.flags&PoseOrientationValid)){
            BridgeLog("HMD tracking valid: flags=0x%X pos=(%.3f %.3f %.3f) quat=(%.3f %.3f %.3f %.3f)",out.head.flags,out.head.px,out.head.py,out.head.pz,out.head.qx,out.head.qy,out.head.qz,out.head.qw);
            trackingLogged=true;
        }
        if(!controllersLogged && (out.left.flags&PoseOrientationValid) && (out.right.flags&PoseOrientationValid)){
            BridgeLog("Quest controller poses valid: L flags=0x%X pos=(%.3f %.3f %.3f) R flags=0x%X pos=(%.3f %.3f %.3f)",out.left.flags,out.left.px,out.left.py,out.left.pz,out.right.flags,out.right.px,out.right.py,out.right.pz);
            controllersLogged=true;
        }
        if(!fovLogged && out.eyeFov[0].valid && out.eyeFov[1].valid){
            BridgeLog("OpenXR per-eye FOV published: L=(%.4f %.4f %.4f %.4f) R=(%.4f %.4f %.4f %.4f)",
                out.eyeFov[0].angleLeft,out.eyeFov[0].angleRight,out.eyeFov[0].angleUp,out.eyeFov[0].angleDown,
                out.eyeFov[1].angleLeft,out.eyeFov[1].angleRight,out.eyeFov[1].angleUp,out.eyeFov[1].angleDown);
            fovLogged=true;
        }

        std::array<const XrCompositionLayerBaseHeader*,3> submittedLayers{};
        XrCompositionLayerQuad hudLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        uint32_t submittedLayerCount=0;
        bool hudGameplayFrame=false;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::array<XrCompositionLayerProjectionView,2> projViews{};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

        if(renderReady && fs.shouldRender && viewCount>=2){
            if(out.eyeFov[0].valid && out.eyeFov[1].valid){
                const LONG pairSequence=gFrameHeader?gFrameHeader->sequence:-1;MemoryBarrier();
                bool haveLeft=ReadEyeFrame(StereoEyeLeft,captured[0]);
                bool haveRight=ReadEyeFrame(StereoEyeRight,captured[1]);
                MemoryBarrier();if(!gFrameHeader || pairSequence!=gFrameHeader->sequence || (pairSequence&1)){haveLeft=haveRight=false;}
                if(!frameLogged && (haveLeft||haveRight)){
                    BridgeLog("Captured game frame visible to bridge: left=%d id=%u %ux%u right=%d id=%u %ux%u scene=%u source=%ux%u gen=%u",haveLeft?1:0,captured[0].frameId,captured[0].width,captured[0].height,haveRight?1:0,captured[1].frameId,captured[1].width,captured[1].height,(unsigned)captured[0].sceneMode,captured[0].sourceWidth,captured[0].sourceHeight,captured[0].generation);
                    frameLogged=true;
                }

                if(!haveLeft && !haveRight && pairTick && GetTickCount64()-pairTick<250){captured=lastPair;haveLeft=haveRight=true;}
                const SceneMode rawScene=(haveLeft?captured[0].sceneMode:(haveRight?captured[1].sceneMode:SceneFlatScreen));
                const SceneMode scene=rawScene;
                if((int)scene!=lastSceneLogged){ BridgeLog("VR scene mode -> %s%s",scene==SceneGameplayVR?"immersive gameplay projection":"flat startup/menu",""); lastSceneLogged=(int)scene; }
                if(scene==SceneFlatScreen && (haveLeft||haveRight)){
                    EyeFrameCopy& src=haveLeft?captured[0]:captured[1];
                    uint32_t imageIndex=0;XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XrResult ar=xrAcquireSwapchainImage(eyes[0].handle,&acq,&imageIndex);XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
                    XrResult wr=XR_ERROR_RUNTIME_FAILURE;if(XR_SUCCEEDED(ar))wr=xrWaitSwapchainImage(eyes[0].handle,&wait);
                    if(XR_SUCCEEDED(ar)&&XR_SUCCEEDED(wr)){
                        const bool menuOk=blit.Draw(dev,ctx,eyes[0].images[imageIndex].texture,src.pixels.data(),src.width,src.height,src.pitch,(DXGI_FORMAT)eyes[0].format);
                        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(eyes[0].handle,&rel);
                        const float width=IniFloat(L"Menu",L"WidthMeters",2.2f);float aspect=src.height?((float)src.width/src.height):(16.0f/9.0f);if(aspect<0.2f)aspect=16.0f/9.0f;
                        quad.space=view;quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;quad.layerFlags=0;
                        quad.subImage.swapchain=eyes[0].handle;quad.subImage.imageRect.offset={0,0};quad.subImage.imageRect.extent={eyes[0].width,eyes[0].height};
                        quad.pose.orientation={0,0,0,1};quad.pose.position={0,IniFloat(L"Menu",L"VerticalOffsetMeters",0.0f),-IniFloat(L"Menu",L"DistanceMeters",2.0f)};
                        quad.size={width,width/aspect};

                        // Give menus/cutscenes a deterministic black background instead of leaving
                        // the compositor with no full-field layer behind the quad.
                        uint32_t bgIndex=0; XrSwapchainImageAcquireInfo bgAcq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        XrResult bgAr=xrAcquireSwapchainImage(eyes[1].handle,&bgAcq,&bgIndex);
                        XrSwapchainImageWaitInfo bgWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};bgWait.timeout=XR_INFINITE_DURATION;
                        XrResult bgWr=XR_ERROR_RUNTIME_FAILURE;if(XR_SUCCEEDED(bgAr))bgWr=xrWaitSwapchainImage(eyes[1].handle,&bgWait);
                        if(XR_SUCCEEDED(bgAr)&&XR_SUCCEEDED(bgWr)){
                            blit.Draw(dev,ctx,eyes[1].images[bgIndex].texture,nullptr,0,0,0,(DXGI_FORMAT)eyes[1].format);
                            XrSwapchainImageReleaseInfo bgRel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(eyes[1].handle,&bgRel);
                            for(uint32_t eye=0;eye<2;++eye){
                                projViews[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                                projViews[eye].pose=xrViews[eye].pose;projViews[eye].fov=xrViews[eye].fov;
                                projViews[eye].subImage.swapchain=eyes[1].handle;
                                projViews[eye].subImage.imageRect.offset={0,0};
                                projViews[eye].subImage.imageRect.extent={eyes[1].width,eyes[1].height};
                            }
                            layer.space=local;layer.viewCount=2;layer.views=projViews.data();
                            submittedLayers[submittedLayerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                        }
                        if(menuOk)submittedLayers[submittedLayerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                    }
                }else{
                    bool eyesOk=haveLeft&&haveRight && captured[0].poseTime>0 && captured[0].poseTime==captured[1].poseTime
                        && captured[0].renderFov.valid && captured[1].renderFov.valid;
                    eyesOk=eyesOk && FrameFresh(captured[0].capturedMs,GetTickCount64()) && FrameFresh(captured[1].capturedMs,GetTickCount64());
                    if(eyesOk){
                        const auto tick=std::min(captured[0].capturedMs,captured[1].capturedMs);
                        if(tick!=pairTick || lastPair[0].frameId!=captured[0].frameId || lastPair[1].frameId!=captured[1].frameId){lastPair=captured;pairTick=tick;}
                    }
                    else if(pairTick && GetTickCount64()-pairTick<250){captured=lastPair;eyesOk=true;}
                    for(uint32_t eye=0;eyesOk && eye<2;++eye){
                        uint32_t imageIndex=0;XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};XrResult ar=xrAcquireSwapchainImage(eyes[eye].handle,&acq,&imageIndex);
                        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;XrResult wr=XR_ERROR_RUNTIME_FAILURE;if(XR_SUCCEEDED(ar))wr=xrWaitSwapchainImage(eyes[eye].handle,&wait);
                        if(XR_FAILED(ar)||XR_FAILED(wr)){eyesOk=false;break;}
                        const auto& src=captured[eye];
                        if(!eyeBlit[eye].Draw(dev,ctx,eyes[eye].images[imageIndex].texture,src.pixels.data(),src.width,src.height,src.pitch,(DXGI_FORMAT)eyes[eye].format,smoothEdges,cacheFrames&&eyes[eye].transferDst?(uint64_t)src.poseTime:0))eyesOk=false;
                        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};if(XR_FAILED(xrReleaseSwapchainImage(eyes[eye].handle,&rel))){eyesOk=false;break;}
                        projViews[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};projViews[eye].pose={{captured[eye].renderPose.qx,captured[eye].renderPose.qy,captured[eye].renderPose.qz,captured[eye].renderPose.qw},{captured[eye].renderPose.px,captured[eye].renderPose.py,captured[eye].renderPose.pz}};
                        projViews[eye].fov={captured[eye].renderFov.angleLeft,captured[eye].renderFov.angleRight,captured[eye].renderFov.angleUp,captured[eye].renderFov.angleDown};projViews[eye].subImage.swapchain=eyes[eye].handle;projViews[eye].subImage.imageRect.offset={0,0};projViews[eye].subImage.imageRect.extent={eyes[eye].width,eyes[eye].height};
                    }
                    if(eyesOk){hudGameplayFrame=true;layer.space=local;layer.viewCount=2;layer.views=projViews.data();submittedLayers[submittedLayerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);}
                }
            }
        }


        // One compositor quad in VIEW space: OpenXR supplies consistent eye
        // disparity instead of drawing independent screen-space HUDs per eye.
        HudData hud{};
        if(fs.shouldRender && focused && submittedLayerCount==1 &&
           submittedLayers[0]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
           hudGameplayFrame &&
           IniFloat(L"HUD",L"Enable",1)!=0 && hudChannel.Read(hud) && HudFresh(hud,GetTickCount64())){
            static bool hudTried=false;
            if(!hudTried){
                hudTried=true;
                if(!hudBlit.Init(dev)||!CreateEyeSwapchain(session,HudCanvas::Width,HudCanvas::Height,eyes[0].format,hudSwap))
                    BridgeLog("VR HUD swapchain initialization failed");
            }
            if(hudSwap.handle){
                if(const uint8_t* pixels=hudCanvas.Draw(hud)){
                    uint32_t index=0;XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    if(XR_SUCCEEDED(xrAcquireSwapchainImage(hudSwap.handle,&acquire,&index))){
                        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
                        if(XR_SUCCEEDED(xrWaitSwapchainImage(hudSwap.handle,&wait))){
                            const bool drawn=hudBlit.Draw(dev,ctx,hudSwap.images[index].texture,pixels,HudCanvas::Width,HudCanvas::Height,HudCanvas::Width*4,(DXGI_FORMAT)hudSwap.format,false,hudSwap.transferDst?hudCanvas.Revision():0,true);
                            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            if(XR_SUCCEEDED(xrReleaseSwapchainImage(hudSwap.handle,&release))&&drawn){
                                hudLayer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                                hudLayer.space=view;hudLayer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                                hudLayer.pose.orientation={0,0,0,1};
                                const float distance=std::clamp(IniFloat(L"HUD",L"DistanceMeters",1.5f),.8f,3.f);
                                const float width=std::clamp(IniFloat(L"HUD",L"WidthMeters",.65f),.4f,1.5f);
                                hudLayer.pose.position={0,std::clamp(IniFloat(L"HUD",L"VerticalOffsetMeters",-.50f),-.8f,.2f),-distance};
                                hudLayer.size={width,width*HudCanvas::Height/HudCanvas::Width};
                                hudLayer.subImage.swapchain=hudSwap.handle;hudLayer.subImage.imageRect.extent={HudCanvas::Width,HudCanvas::Height};
                                submittedLayers[submittedLayerCount++]=(const XrCompositionLayerBaseHeader*)&hudLayer;
                            }
                        }
                    }
                }
            }
        }

        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime=fs.predictedDisplayTime;
        fe.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        if(submittedLayerCount){fe.layerCount=submittedLayerCount;fe.layers=submittedLayers.data();}else{fe.layerCount=0;fe.layers=nullptr;}
        XrResult endr=xrEndFrame(session,&fe);
        if(XR_FAILED(endr)){BridgeLog("xrEndFrame failed: %d",(int)endr);}
        if(XR_SUCCEEDED(endr)&&submittedLayerCount)++framesWithLayers;else ++framesWithoutLayers;
        if(GetTickCount64()>=nextRenderReport){
            BridgeLog("Render delivery: acceptedWithLayers=%u emptyOrFailed=%u lastLayers=%u shouldRender=%d state=%d endResult=%d",framesWithLayers,framesWithoutLayers,submittedLayerCount,(int)fs.shouldRender,(int)state,(int)endr);
            BridgeLog("GPU frame cache: uploads=%llu filters=%llu reused=%llu",eyeBlit[0].UploadCount()+eyeBlit[1].UploadCount(),eyeBlit[0].DrawCount()+eyeBlit[1].DrawCount(),eyeBlit[0].ReuseCount()+eyeBlit[1].ReuseCount());
            nextRenderReport=GetTickCount64()+5000;framesWithLayers=framesWithoutLayers=0;
        }
    }

    out={};Publish(out);
    if(rightSpace)xrDestroySpace(rightSpace);if(leftSpace)xrDestroySpace(leftSpace);
    if(rStickClick)xrDestroyAction(rStickClick);if(lStickClick)xrDestroyAction(lStickClick);if(btnY)xrDestroyAction(btnY);if(btnX)xrDestroyAction(btnX);if(btnB)xrDestroyAction(btnB);if(btnA)xrDestroyAction(btnA);if(squeeze)xrDestroyAction(squeeze);if(trigger)xrDestroyAction(trigger);if(stick)xrDestroyAction(stick);if(hand)xrDestroyAction(hand);if(aset)xrDestroyActionSet(aset);
    if(view)xrDestroySpace(view);if(local)xrDestroySpace(local);
    if(hudSwap.handle)xrDestroySwapchain(hudSwap.handle);
    for(auto& eye:eyes)if(eye.handle)xrDestroySwapchain(eye.handle);
    xrDestroySession(session);xrDestroyInstance(inst);
    if(ctx)ctx->Release();if(dev)dev->Release();
    return 0;
}
}
