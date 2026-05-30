#pragma once

#include "rtv/PathTracerRenderer.h"
#include "rtv/SceneComponents.h"

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

struct SceneUpdateRoute {
    SceneUpdateKind kind = SceneUpdateKind::None;
    SceneUpdateGpuAction action = SceneUpdateGpuAction::None;
    AccumulationResetReason resetReason = AccumulationResetReason::Manual;
    bool requiresGpuSceneBuild = false;
    bool requiresRendererRebuild = false;
    bool resetsAccumulation = false;
};

class SceneUpdateRouter {
public:
    [[nodiscard]] static SceneUpdateRoute route(SceneUpdateKind kind);

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

} // namespace rtv
