#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace rtv {

struct LightBvhNode {
    float totalPower = 0.0f;
    int32_t lightIndex = -1;
    uint32_t childOffset = 0;
    uint32_t childCount = 0;
    uint32_t lightCount = 0;
};

[[nodiscard]] std::vector<LightBvhNode> buildLightBvh(
    const std::vector<float>& lightPower,
    uint32_t maxLightsPerLeaf = 4);

[[nodiscard]] std::vector<glm::vec4> packLightBvhNodesForGpu(
    const std::vector<LightBvhNode>& nodes);

} // namespace rtv
