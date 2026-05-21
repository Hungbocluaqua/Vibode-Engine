#include "rtv/SceneComponents.h"

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

} // namespace rtv
