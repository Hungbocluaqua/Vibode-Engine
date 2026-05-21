#pragma once

#include "rtv/EntityId.h"
#include "rtv/SceneComponents.h"

#include <optional>
#include <string>
#include <vector>

namespace rtv {

struct Entity {
    EntityId id{};
    std::string name;
    Transform transform{};
    EntityId parent{};
    std::vector<EntityId> children;
    bool locked = false;

    std::optional<MeshRenderer> meshRenderer;
    std::optional<Light> light;
    std::optional<Camera> camera;
};

} // namespace rtv
