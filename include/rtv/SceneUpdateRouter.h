#pragma once

#include "rtv/PathTracerRenderer.h"
#include "rtv/SceneComponents.h"

#include <string>

namespace rtv {

enum class SceneUpdateGpuAction : uint8_t {
    None,
    UpdateMaterials,
    UpdateTransforms,
    UpdateLights,
    UpdateEnvironment,
    UpdateCamera,
    UpdateVisibility,
    RebuildTopology,
    ApplyRendererSettings,
};

using SceneUpdateGpuActionMask = uint32_t;

struct SceneUpdateRoute {
    SceneUpdateKind kind = SceneUpdateKind::None;
    SceneUpdateGpuAction action = SceneUpdateGpuAction::None;
    SceneUpdateGpuActionMask actionMask = 0u;
    AccumulationResetReason resetReason = AccumulationResetReason::Manual;
    bool requiresGpuSceneBuild = false;
    bool requiresRendererRebuild = false;
    bool resetsAccumulation = false;
};

class SceneUpdateRouter {
public:
    [[nodiscard]] static SceneUpdateRoute route(SceneUpdateKind kind);
    [[nodiscard]] static SceneUpdateRoute route(SceneUpdateMask mask);

    [[nodiscard]] const char* lastUpdateKindName() const { return sceneUpdateKindName(lastKind_); }
    [[nodiscard]] SceneUpdateKind lastUpdateKind() const { return lastKind_; }
    [[nodiscard]] uint64_t routeCount(SceneUpdateKind kind) const;

    static SceneUpdateRouter& instance();

private:
    SceneUpdateRouter() = default;
    void record(SceneUpdateKind kind);

    SceneUpdateKind lastKind_ = SceneUpdateKind::TopologyChanged;
    uint64_t routeCounts_[10] = {};
};

[[nodiscard]] const char* sceneUpdateGpuActionName(SceneUpdateGpuAction action);
[[nodiscard]] SceneUpdateGpuActionMask sceneUpdateGpuActionBit(SceneUpdateGpuAction action);
[[nodiscard]] bool sceneUpdateRouteHasAction(const SceneUpdateRoute& route, SceneUpdateGpuAction action);
[[nodiscard]] std::string sceneUpdateGpuActionMaskName(SceneUpdateGpuActionMask mask);

} // namespace rtv
