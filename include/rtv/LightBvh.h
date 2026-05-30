#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace rtv {

struct LightBvhNode {
    glm::vec3 boundsMin{0.0f};
    float totalPower = 0.0f;
    glm::vec3 boundsMax{0.0f};
    int32_t lightIndex = -1;
    uint32_t childOffset = 0;
    uint32_t childCount = 0;
    uint32_t lightCount = 0;
};

struct LightBvhPrimitive {
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    glm::vec3 centroid{0.0f};
    float power = 0.0f;
    uint32_t lightIndex = 0;
};

struct LightBvhStats {
    uint32_t nodeCount = 0;
    uint32_t innerNodeCount = 0;
    uint32_t leafCount = 0;
    uint32_t maxDepth = 0;
    uint32_t maxLeafLightCount = 0;
    float totalPower = 0.0f;
    float minLeafPower = 0.0f;
    float maxLeafPower = 0.0f;
    float estimatedAverageTraversalSteps = 0.0f;
};

[[nodiscard]] std::vector<LightBvhNode> buildLightBvh(
    const std::vector<float>& lightPower,
    uint32_t maxLightsPerLeaf = 4);

[[nodiscard]] std::vector<LightBvhNode> buildLightBvh(
    const std::vector<LightBvhPrimitive>& lights,
    uint32_t maxLightsPerLeaf = 4);

[[nodiscard]] LightBvhStats computeLightBvhStats(
    const std::vector<LightBvhNode>& nodes);

[[nodiscard]] std::vector<glm::vec4> packLightBvhNodesForGpu(
    const std::vector<LightBvhNode>& nodes);

} // namespace rtv
