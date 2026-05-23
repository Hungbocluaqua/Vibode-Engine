#include "rtv/LightBvh.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>

namespace rtv {

std::vector<LightBvhNode> buildLightBvh(
    const std::vector<float>& lightPower,
    uint32_t maxLightsPerLeaf) {
    const uint32_t count = static_cast<uint32_t>(lightPower.size());
    if (count == 0) return {};

    struct Range {
        uint32_t begin;
        uint32_t end;
    };

    std::vector<LightBvhNode> nodes;
    std::vector<Range> stack;
    stack.push_back({0, count});

    while (!stack.empty()) {
        const Range r = stack.back();
        stack.pop_back();
        const uint32_t rangeCount = r.end - r.begin;

        float totalPower = 0.0f;
        for (uint32_t i = r.begin; i < r.end; ++i) {
            totalPower += std::max(lightPower[i], 1e-8f);
        }

        if (rangeCount <= maxLightsPerLeaf) {
            LightBvhNode leaf;
            leaf.totalPower = totalPower;
            leaf.lightIndex = static_cast<int32_t>(r.begin);
            leaf.lightCount = rangeCount;
            nodes.push_back(leaf);
        } else {
            const uint32_t mid = r.begin + rangeCount / 2;

            LightBvhNode inner;
            inner.totalPower = totalPower;
            inner.lightIndex = -1;
            inner.childOffset = static_cast<uint32_t>(nodes.size() + 1);
            inner.childCount = 2;
            nodes.push_back(inner);

            stack.push_back({mid, r.end});
            stack.push_back({r.begin, mid});
        }
    }

    return nodes;
}

std::vector<glm::vec4> packLightBvhNodesForGpu(const std::vector<LightBvhNode>& nodes) {
    std::vector<glm::vec4> packed;
    packed.reserve(nodes.size() * 2);
    for (const LightBvhNode& node : nodes) {
        packed.push_back({0.0f, 0.0f, 0.0f, node.totalPower});
        if (node.lightIndex >= 0) {
            uint32_t leafInfo = (1u << 31u) | (static_cast<uint32_t>(node.lightIndex) & 0x7fffu) | ((node.lightCount & 0x3fffu) << 16u);
            float w;
            std::memcpy(&w, &leafInfo, sizeof(float));
            packed.push_back({0.0f, 0.0f, 0.0f, w});
        } else {
            uint32_t innerInfo = (node.childCount & 0xffffu) | ((node.childOffset & 0xffffu) << 16u);
            float w;
            std::memcpy(&w, &innerInfo, sizeof(float));
            packed.push_back({0.0f, 0.0f, 0.0f, w});
        }
    }
    return packed;
}

} // namespace rtv
