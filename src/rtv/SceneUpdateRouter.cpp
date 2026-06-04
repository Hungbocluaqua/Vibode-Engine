#include "rtv/SceneUpdateRouter.h"

#include <algorithm>
#include <array>

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

SceneUpdateGpuActionMask sceneUpdateGpuActionBit(SceneUpdateGpuAction action) {
    if (action == SceneUpdateGpuAction::None) {
        return 0u;
    }
    return 1u << static_cast<uint32_t>(action);
}

bool sceneUpdateRouteHasAction(const SceneUpdateRoute& route, SceneUpdateGpuAction action) {
    return (route.actionMask & sceneUpdateGpuActionBit(action)) != 0u;
}

std::string sceneUpdateGpuActionMaskName(SceneUpdateGpuActionMask mask) {
    if (mask == 0u) {
        return sceneUpdateGpuActionName(SceneUpdateGpuAction::None);
    }
    const std::array<SceneUpdateGpuAction, 8> actions{{
        SceneUpdateGpuAction::UpdateMaterials,
        SceneUpdateGpuAction::UpdateTransforms,
        SceneUpdateGpuAction::UpdateLights,
        SceneUpdateGpuAction::UpdateEnvironment,
        SceneUpdateGpuAction::UpdateCamera,
        SceneUpdateGpuAction::UpdateVisibility,
        SceneUpdateGpuAction::RebuildTopology,
        SceneUpdateGpuAction::ApplyRendererSettings,
    }};
    std::string result;
    for (SceneUpdateGpuAction action : actions) {
        if ((mask & sceneUpdateGpuActionBit(action)) == 0u) {
            continue;
        }
        if (!result.empty()) {
            result += "+";
        }
        result += sceneUpdateGpuActionName(action);
    }
    return result.empty() ? sceneUpdateGpuActionName(SceneUpdateGpuAction::None) : result;
}

namespace {

void addAction(SceneUpdateRoute& route, SceneUpdateGpuAction action) {
    if (route.action == SceneUpdateGpuAction::None) {
        route.action = action;
    }
    route.actionMask |= sceneUpdateGpuActionBit(action);
}

void setResetReason(SceneUpdateRoute& route, AccumulationResetReason reason) {
    if (route.resetReason == AccumulationResetReason::Manual || reason == AccumulationResetReason::SceneChanged) {
        route.resetReason = reason;
    }
}

} // namespace

SceneUpdateRoute SceneUpdateRouter::route(SceneUpdateKind kind) {
    return route(sceneUpdateKindMask(kind));
}

SceneUpdateRoute SceneUpdateRouter::route(SceneUpdateMask mask) {
    SceneUpdateRoute route;
    route.kind = sceneUpdateKindFromMask(mask);
    if (mask == SceneUpdateMaskNone) {
        return route;
    }
    if ((mask & SceneUpdateMaskTopology) != 0u) {
        addAction(route, SceneUpdateGpuAction::RebuildTopology);
        route.resetReason = AccumulationResetReason::SceneChanged;
        route.requiresGpuSceneBuild = true;
        route.requiresRendererRebuild = true;
        route.resetsAccumulation = true;
        return route;
    }
    if ((mask & SceneUpdateMaskMaterial) != 0u) {
        addAction(route, SceneUpdateGpuAction::UpdateMaterials);
        setResetReason(route, AccumulationResetReason::MaterialChanged);
        route.requiresGpuSceneBuild = true;
        route.resetsAccumulation = true;
    }
    if ((mask & SceneUpdateMaskTransform) != 0u) {
        addAction(route, SceneUpdateGpuAction::UpdateTransforms);
        setResetReason(route, AccumulationResetReason::SceneChanged);
        route.requiresGpuSceneBuild = true;
        route.resetsAccumulation = true;
    }
    if ((mask & SceneUpdateMaskLight) != 0u) {
        addAction(route, SceneUpdateGpuAction::UpdateLights);
        setResetReason(route, AccumulationResetReason::LightingChanged);
        route.requiresGpuSceneBuild = true;
        route.resetsAccumulation = true;
    }
    if ((mask & SceneUpdateMaskEnvironment) != 0u) {
        addAction(route, SceneUpdateGpuAction::UpdateEnvironment);
        setResetReason(route, AccumulationResetReason::EnvironmentChanged);
        route.requiresGpuSceneBuild = true;
        route.resetsAccumulation = true;
    }
    if ((mask & SceneUpdateMaskCamera) != 0u) {
        addAction(route, SceneUpdateGpuAction::UpdateCamera);
        setResetReason(route, AccumulationResetReason::CameraMoved);
        route.resetsAccumulation = true;
    }
    if ((mask & SceneUpdateMaskVisibility) != 0u) {
        addAction(route, SceneUpdateGpuAction::UpdateVisibility);
        route.requiresGpuSceneBuild = true;
    }
    if ((mask & SceneUpdateMaskRendererSettings) != 0u) {
        addAction(route, SceneUpdateGpuAction::ApplyRendererSettings);
        setResetReason(route, AccumulationResetReason::RenderSettingsChanged);
        route.resetsAccumulation = true;
    }
    return route;
}

void SceneUpdateRouter::record(SceneUpdateKind kind) {
    lastKind_ = kind;
    const auto idx = static_cast<uint32_t>(kind);
    if (idx < 10u) {
        ++routeCounts_[idx];
    }
}

} // namespace rtv
