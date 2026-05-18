#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace rtv {

struct Bounds3 {
    glm::vec3 min{1.0e30f};
    glm::vec3 max{-1.0e30f};
};

[[nodiscard]] uint32_t expandBits10(uint32_t value);
[[nodiscard]] uint32_t morton3D30(glm::vec3 point, const Bounds3& centroidBounds);

} // namespace rtv
