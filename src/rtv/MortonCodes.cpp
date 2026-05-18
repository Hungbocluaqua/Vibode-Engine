#include "rtv/MortonCodes.h"

#include <algorithm>

namespace rtv {

uint32_t expandBits10(uint32_t value) {
    uint32_t x = value & 0x3ffu;
    x = (x | (x << 16u)) & 0x030000ffu;
    x = (x | (x << 8u)) & 0x0300f00fu;
    x = (x | (x << 4u)) & 0x030c30c3u;
    x = (x | (x << 2u)) & 0x09249249u;
    return x;
}

uint32_t morton3D30(glm::vec3 point, const Bounds3& centroidBounds) {
    const glm::vec3 extent = glm::max(centroidBounds.max - centroidBounds.min, glm::vec3(1.0e-8f));
    const glm::vec3 normalized = glm::clamp((point - centroidBounds.min) / extent, glm::vec3(0.0f), glm::vec3(1.0f));
    const auto quantize = [](float value) {
        return static_cast<uint32_t>(std::clamp(value * 1023.0f, 0.0f, 1023.0f));
    };
    const uint32_t x = quantize(normalized.x);
    const uint32_t y = quantize(normalized.y);
    const uint32_t z = quantize(normalized.z);
    return expandBits10(x) | (expandBits10(y) << 1u) | (expandBits10(z) << 2u);
}

} // namespace rtv
