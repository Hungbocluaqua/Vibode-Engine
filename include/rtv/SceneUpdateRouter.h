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
};

[[nodiscard]] const char* sceneUpdateGpuActionName(SceneUpdateGpuAction action);

} // namespace rtv
