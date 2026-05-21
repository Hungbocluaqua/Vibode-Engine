#include "rtv/SceneUpdateRouter.h"

namespace rtv {

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

} // namespace rtv
