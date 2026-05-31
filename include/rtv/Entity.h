#pragma once

#include "rtv/EntityId.h"
#include "rtv/SceneComponents.h"

#include <optional>
#include <string>
#include <vector>

namespace rtv {

struct Entity {
    EntityId id{};
    uint64_t uuid = 0;
    std::string name;
    Transform transform{};
    EntityId parent{};
    std::vector<EntityId> children;
    bool visible = true;
    bool locked = false;

    std::optional<MeshRenderer> meshRenderer;
    std::optional<Light> light;
    std::optional<Sun> sun;
    std::optional<Camera> camera;
    std::optional<EnvironmentLight> environmentLight;
    std::optional<SkyAtmosphere> skyAtmosphere;
    std::optional<HeightFog> heightFog;
    std::optional<VolumetricCloud> volumetricCloud;
    std::optional<PostProcessVolume> postProcessVolume;
    std::optional<CameraPostProcess> cameraPostProcess;
};

} // namespace rtv
