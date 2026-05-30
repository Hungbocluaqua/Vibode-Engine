#include "rtv/SceneUpdateRouter.h"

#include <algorithm>
#include <cassert>

namespace rtv {

SceneUpdateRouter& SceneUpdateRouter::instance() {
    static SceneUpdateRouter router;
    return router;
}

uint64_t SceneUpdateRouter::routeCount(SceneUpdateKind kind) const {
    const auto idx = static_cast<uint32_t>(kind);
    if (idx >= 10u) return 0;
    return routeCounts_[idx];
}

const char* sceneUpdateGpuActionName(SceneUpdateGpuAction action) {
    switch (action) {
    case SceneUpdateGpuAction::None: return "None";
    case SceneUpdateGpuAction::UpdateMaterials: return "UpdateMaterials";
    case SceneUpdateGpuAction::UpdateTransforms: return "UpdateTransforms";
    case SceneUpdateGpuAction::UpdateLights: return "UpdateLights";
    case SceneUpdateGpuAction::UpdateEnvironment: return "UpdateEnvironment";
    case SceneUpdateGpuAction::UpdateCamera: return "UpdateCamera";
    case SceneUpdateGpuAction::UpdateVisibility: return "UpdateVisibility";
    case SceneUpdateGpuAction::RebuildTopology: return "RebuildTopology";
    case SceneUpdateGpuAction::ApplyRendererSettings: return "ApplyRendererSettings";
    }
    return "Unknown";
}

SceneUpdateRoute SceneUpdateRouter::route(SceneUpdateKind kind) {
    SceneUpdateRoute route;
    route.kind = kind;
    switch (kind) {
    case SceneUpdateKind::None:
        return route;
    case SceneUpdateKind::MaterialOnly:
        route.action = SceneUpdateGpuAction::UpdateMaterials;
        route.resetReason = AccumulationResetReason::MaterialChanged;
        route.requiresGpuSceneBuild = true;
        route.resetsAccumulation = true;
        return route;
    case SceneUpdateKind::TransformOnly:
        route.action = SceneUpdateGpuAction::UpdateTransforms;
        route.resetReason = AccumulationResetReason::SceneChanged;
        route.requiresGpuSceneBuild = true;
        route.resetsAccumulation = true;
        return route;
    case SceneUpdateKind::LightOnly:
        route.action = SceneUpdateGpuAction::UpdateLights;
        route.resetReason = AccumulationResetReason::LightingChanged;
        route.requiresGpuSceneBuild = true;
        route.resetsAccumulation = true;
        return route;
    case SceneUpdateKind::EnvironmentOnly:
        route.action = SceneUpdateGpuAction::UpdateEnvironment;
        route.resetReason = AccumulationResetReason::EnvironmentChanged;
        route.requiresGpuSceneBuild = true;
        route.resetsAccumulation = true;
        return route;
    case SceneUpdateKind::CameraOnly:
        route.action = SceneUpdateGpuAction::UpdateCamera;
        route.resetReason = AccumulationResetReason::CameraMoved;
        route.resetsAccumulation = true;
        return route;
    case SceneUpdateKind::VisibilityOnly:
        route.action = SceneUpdateGpuAction::UpdateVisibility;
        route.requiresGpuSceneBuild = true;
        return route;
    case SceneUpdateKind::TopologyChanged:
        route.action = SceneUpdateGpuAction::RebuildTopology;
        route.resetReason = AccumulationResetReason::SceneChanged;
        route.requiresGpuSceneBuild = true;
        route.requiresRendererRebuild = true;
        route.resetsAccumulation = true;
        return route;
    case SceneUpdateKind::RendererSettingsOnly:
        route.action = SceneUpdateGpuAction::ApplyRendererSettings;
        route.resetReason = AccumulationResetReason::RenderSettingsChanged;
        route.resetsAccumulation = true;
        return route;
    }
    return route;
}

void SceneUpdateRouter::record(SceneUpdateKind kind) {
    lastKind_ = kind;
    const auto idx = static_cast<uint32_t>(kind);
    if (idx < 10u) {
        ++routeCounts_[idx];
    }
    if (kind == SceneUpdateKind::TopologyChanged) {
        assert(routeCounts_[idx] <= 2u && "TopologyChanged (full rebuild) should only occur during initial scene load or explicit user request");
    }
}

} // namespace rtv
