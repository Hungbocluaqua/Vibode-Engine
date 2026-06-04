#include "rtv/SceneComponents.h"

#include <array>
#include <utility>

namespace rtv {

const char* sceneUpdateKindName(SceneUpdateKind kind) {
    switch (kind) {
    case SceneUpdateKind::None: return "None";
    case SceneUpdateKind::MaterialOnly: return "MaterialOnly";
    case SceneUpdateKind::TransformOnly: return "TransformOnly";
    case SceneUpdateKind::LightOnly: return "LightOnly";
    case SceneUpdateKind::EnvironmentOnly: return "EnvironmentOnly";
    case SceneUpdateKind::CameraOnly: return "CameraOnly";
    case SceneUpdateKind::VisibilityOnly: return "VisibilityOnly";
    case SceneUpdateKind::TopologyChanged: return "TopologyChanged";
    case SceneUpdateKind::RendererSettingsOnly: return "RendererSettingsOnly";
    }
    return "Unknown";
}

SceneUpdateMask sceneUpdateKindMask(SceneUpdateKind kind) {
    switch (kind) {
    case SceneUpdateKind::None: return SceneUpdateMaskNone;
    case SceneUpdateKind::MaterialOnly: return SceneUpdateMaskMaterial;
    case SceneUpdateKind::TransformOnly: return SceneUpdateMaskTransform;
    case SceneUpdateKind::LightOnly: return SceneUpdateMaskLight;
    case SceneUpdateKind::EnvironmentOnly: return SceneUpdateMaskEnvironment;
    case SceneUpdateKind::CameraOnly: return SceneUpdateMaskCamera;
    case SceneUpdateKind::VisibilityOnly: return SceneUpdateMaskVisibility;
    case SceneUpdateKind::TopologyChanged: return SceneUpdateMaskTopology;
    case SceneUpdateKind::RendererSettingsOnly: return SceneUpdateMaskRendererSettings;
    }
    return SceneUpdateMaskNone;
}

SceneUpdateKind sceneUpdateKindFromMask(SceneUpdateMask mask) {
    if ((mask & SceneUpdateMaskTopology) != 0u) {
        return SceneUpdateKind::TopologyChanged;
    }
    switch (mask) {
    case SceneUpdateMaskNone: return SceneUpdateKind::None;
    case SceneUpdateMaskMaterial: return SceneUpdateKind::MaterialOnly;
    case SceneUpdateMaskTransform: return SceneUpdateKind::TransformOnly;
    case SceneUpdateMaskLight: return SceneUpdateKind::LightOnly;
    case SceneUpdateMaskEnvironment: return SceneUpdateKind::EnvironmentOnly;
    case SceneUpdateMaskCamera: return SceneUpdateKind::CameraOnly;
    case SceneUpdateMaskVisibility: return SceneUpdateKind::VisibilityOnly;
    case SceneUpdateMaskRendererSettings: return SceneUpdateKind::RendererSettingsOnly;
    default:
        if ((mask & SceneUpdateMaskTransform) != 0u) return SceneUpdateKind::TransformOnly;
        if ((mask & SceneUpdateMaskLight) != 0u) return SceneUpdateKind::LightOnly;
        if ((mask & SceneUpdateMaskEnvironment) != 0u) return SceneUpdateKind::EnvironmentOnly;
        if ((mask & SceneUpdateMaskCamera) != 0u) return SceneUpdateKind::CameraOnly;
        if ((mask & SceneUpdateMaskMaterial) != 0u) return SceneUpdateKind::MaterialOnly;
        if ((mask & SceneUpdateMaskVisibility) != 0u) return SceneUpdateKind::VisibilityOnly;
        if ((mask & SceneUpdateMaskRendererSettings) != 0u) return SceneUpdateKind::RendererSettingsOnly;
        return SceneUpdateKind::None;
    }
}

std::string sceneUpdateMaskName(SceneUpdateMask mask) {
    if (mask == SceneUpdateMaskNone) {
        return sceneUpdateKindName(SceneUpdateKind::None);
    }
    const std::array<std::pair<SceneUpdateMask, SceneUpdateKind>, 8> entries{{
        {SceneUpdateMaskMaterial, SceneUpdateKind::MaterialOnly},
        {SceneUpdateMaskTransform, SceneUpdateKind::TransformOnly},
        {SceneUpdateMaskLight, SceneUpdateKind::LightOnly},
        {SceneUpdateMaskEnvironment, SceneUpdateKind::EnvironmentOnly},
        {SceneUpdateMaskCamera, SceneUpdateKind::CameraOnly},
        {SceneUpdateMaskVisibility, SceneUpdateKind::VisibilityOnly},
        {SceneUpdateMaskTopology, SceneUpdateKind::TopologyChanged},
        {SceneUpdateMaskRendererSettings, SceneUpdateKind::RendererSettingsOnly},
    }};
    std::string result;
    for (const auto& [bit, kind] : entries) {
        if ((mask & bit) == 0u) {
            continue;
        }
        if (!result.empty()) {
            result += "+";
        }
        result += sceneUpdateKindName(kind);
    }
    return result.empty() ? sceneUpdateKindName(SceneUpdateKind::None) : result;
}

} // namespace rtv
