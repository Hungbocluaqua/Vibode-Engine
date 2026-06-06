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
    std::string layer;
    std::vector<std::string> tags;
    std::vector<std::string> collections;
    Transform transform{};
    Transform defaultTransform{};
    EntityId parent{};
    std::vector<EntityId> children;
    bool visible = true;
    bool locked = false;
    int32_t sourceNodeIndex = -1;

    std::optional<MeshRenderer> meshRenderer;
    std::optional<AnimationPlayer> animationPlayer;
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
