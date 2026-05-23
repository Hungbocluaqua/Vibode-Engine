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
    std::optional<Camera> camera;
};

} // namespace rtv
