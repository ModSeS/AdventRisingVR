#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>

namespace arvr {
static constexpr uint32_t kMagic = 0x52565241; // ARVR
static constexpr uint32_t kVersion = 8;
#ifdef ARVR_TEST_IPC
static constexpr wchar_t kMappingName[] = L"Local\\AdventRisingVR_Test_Poses_v8";
#else
static constexpr wchar_t kMappingName[] = L"Local\\AdventRisingVR_Poses_v8";
#endif

static constexpr uint32_t kFrameMagic = 0x46565241; // ARVF
#ifdef ARVR_TEST_IPC
static constexpr wchar_t kFrameMappingName[] = L"Local\\AdventRisingVR_Test_Frame_v8";
#else
static constexpr wchar_t kFrameMappingName[] = L"Local\\AdventRisingVR_Frame_v8";
#endif
static constexpr uint32_t kFrameMaxWidth = 4096;
static constexpr uint32_t kFrameMaxHeight = 2160;
static constexpr uint32_t kFrameBytesPerPixel = 4;
static constexpr size_t kFrameEyeBytes = size_t(kFrameMaxWidth) * size_t(kFrameMaxHeight) * kFrameBytesPerPixel;

enum PoseFlags : uint32_t {
    PoseOrientationValid=1u<<0,
    PosePositionValid=1u<<1,
    PoseTracked=1u<<2
};
enum StereoEye : uint32_t { StereoEyeLeft=0, StereoEyeRight=1, StereoEyeMono=2 };
enum SceneMode : uint32_t { SceneFlatScreen=0, SceneGameplayVR=1 };
enum InputButtons : uint32_t {
    ButtonA=1u<<0,
    ButtonB=1u<<1,
    ButtonX=1u<<2,
    ButtonY=1u<<3,
    ButtonLStick=1u<<4,
    ButtonRStick=1u<<5
};

#pragma pack(push, 4)
struct Pose {
    uint32_t flags{};
    float px{},py{},pz{};
    float qx{},qy{},qz{},qw{1.0f};
};
struct EyeFov {
    float angleLeft{};
    float angleRight{};
    float angleUp{};
    float angleDown{};
    uint32_t valid{};
};
struct InputState {
    float leftStickX{}, leftStickY{};
    float rightStickX{}, rightStickY{};
    float leftTrigger{}, rightTrigger{};
    float leftGrip{}, rightGrip{};
    uint32_t buttons{};
};
struct Payload {
    uint64_t heartbeatMs{};
    uint32_t bridgePid{};
    uint32_t sessionState{};
    int64_t predictedDisplayTime{};
    Pose head{}, left{}, right{};
    Pose eyes[2]{};
    EyeFov eyeFov[2]{};
    InputState input{};
};
struct SharedState {
    uint32_t magic{kMagic};
    uint32_t version{kVersion};
    volatile LONG sequence{};
    uint32_t reserved{};
    Payload payload{};
};
struct FrameTransportHeader {
    uint32_t magic{kFrameMagic};
    uint32_t version{kVersion};
    volatile LONG sequence{};
    uint32_t reserved{};
    uint32_t width[2]{};
    uint32_t height[2]{};
    uint32_t pitch[2]{};
    uint32_t frameId[2]{};
    uint64_t capturedMs[2]{};
    int64_t poseTime[2]{};
    Pose renderPose[2]{};
    EyeFov renderFov[2]{};
    uint32_t sourceWidth{};
    uint32_t sourceHeight{};
    uint32_t viewportX{};
    uint32_t viewportY{};
    uint32_t viewportWidth{};
    uint32_t viewportHeight{};
    uint32_t resolutionGeneration{};
    uint32_t sceneMode{SceneFlatScreen};
};
#pragma pack(pop)

static constexpr size_t kFrameMappingSize = sizeof(FrameTransportHeader) + kFrameEyeBytes * 2;
}
