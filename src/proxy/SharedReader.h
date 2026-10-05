#pragma once
#include "../shared/SharedState.h"
namespace arvr { bool ReadBridgePayload(Payload& out); bool EnsureBridge(bool autoLaunch); void BeginRenderSnapshot(); void EndRenderSnapshot(); uint64_t RenderSnapshotId(); }
