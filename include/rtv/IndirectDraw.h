#pragma once

#include <glm/glm.hpp>
#include <Volk/volk.h>

#include <cstdint>

namespace rtv {

struct GpuDrawMetadata {
    glm::mat4 worldFromLocal{1.0f};
    uint32_t materialIndex = 0;
    uint32_t meshIndex = 0;
    uint32_t flags = 0;
    uint32_t padding = 0;
};

struct MeshDrawCommand {
    VkDrawIndexedIndirectCommand command{};
    uint32_t metadataIndex = 0;
    uint32_t boundsIndex = 0;
    uint32_t padding0 = 0;
    uint32_t padding1 = 0;
};

} // namespace rtv
