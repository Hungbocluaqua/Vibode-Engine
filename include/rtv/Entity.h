#pragma once

#include "rtv/EntityId.h"
#include "rtv/SceneComponents.h"

#include <optional>
#include <string>

namespace rtv {

struct Entity {
    EntityId id{};
    std::string name;
    Transform transform{};

    std::optional<MeshRenderer> meshRenderer;
    std::optional<Light> light;
    std::optional<Camera> camera;
};

} // namespace rtv
