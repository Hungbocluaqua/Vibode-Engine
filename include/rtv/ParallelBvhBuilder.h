#pragma once

#include "rtv/BvhBuilder.h"

#include <cstdint>
#include <vector>
#include <optional>
#include <chrono>

namespace rtv {

struct ParallelBvhBuildTask {
    const std::vector<glm::vec3>* vertices = nullptr;
    const std::vector<uint32_t>* indices = nullptr;
    const std::vector<uint32_t>* faceMaterials = nullptr;
    const std::vector<glm::vec2>* texcoords = nullptr;
    const std::vector<glm::vec3>* normals = nullptr;
    const std::vector<glm::vec4>* tangents = nullptr;
    BvhBuildQuality quality = BvhBuildQuality::BinnedSah;
};

struct ParallelBvhBuildResult {
    BvhBuildResult bvh;
    double buildTimeMs = 0.0;
};

class ParallelBvhBuilder {
public:
    [[nodiscard]] static std::vector<ParallelBvhBuildResult> buildAll(
        const std::vector<ParallelBvhBuildTask>& tasks,
        uint32_t maxThreads = 0);
};

} // namespace rtv
