#include "rtv/BvhCollapse.h"

#include <algorithm>
#include <array>

namespace rtv {

namespace {

[[nodiscard]] float surfaceArea(const BinaryBvhNode& node) {
    const glm::vec3 d = glm::max(node.boundsMax - node.boundsMin, glm::vec3(0.0f));
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

[[nodiscard]] bool isLeaf(const BinaryBvhNode& node) {
    return node.triCount > 0;
}

struct Bvh4Children {
    std::array<int, 4> nodes{-1, -1, -1, -1};
    uint32_t count = 0;
};

[[nodiscard]] Bvh4Children collectBvh4Children(const std::vector<BinaryBvhNode>& nodes, int binaryNodeIdx) {
    const BinaryBvhNode& root = nodes[static_cast<size_t>(binaryNodeIdx)];
    Bvh4Children candidates;
    if (root.left >= 0) {
        candidates.nodes[candidates.count++] = root.left;
    }
    if (root.right >= 0) {
        candidates.nodes[candidates.count++] = root.right;
    }

    while (candidates.count < 4) {
        int expandAt = -1;
        float expandArea = -1.0f;
        for (uint32_t i = 0; i < candidates.count; ++i) {
            const BinaryBvhNode& candidate = nodes[static_cast<size_t>(candidates.nodes[i])];
            if (!isLeaf(candidate) && (candidate.left >= 0 || candidate.right >= 0)) {
                const float area = surfaceArea(candidate);
                if (area > expandArea) {
                    expandArea = area;
                    expandAt = static_cast<int>(i);
                }
            }
        }
        if (expandAt < 0) {
            break;
        }

        const BinaryBvhNode& expanded = nodes[static_cast<size_t>(candidates.nodes[static_cast<size_t>(expandAt)])];
        std::array<int, 2> replacement{-1, -1};
        uint32_t replacementCount = 0;
        if (expanded.left >= 0) {
            replacement[replacementCount++] = expanded.left;
        }
        if (expanded.right >= 0) {
            replacement[replacementCount++] = expanded.right;
        }

        const uint32_t oldCount = candidates.count;
        const uint32_t tailStart = static_cast<uint32_t>(expandAt) + 1u;
        const uint32_t tailCount = oldCount - tailStart;
        const uint32_t newCount = std::min<uint32_t>(4u, oldCount - 1u + replacementCount);
        for (uint32_t i = tailCount; i > 0; --i) {
            const uint32_t source = tailStart + i - 1u;
            const uint32_t destination = tailStart + replacementCount + i - 2u;
            if (destination < 4u) {
                candidates.nodes[destination] = candidates.nodes[source];
            }
        }
        for (uint32_t i = 0; i < replacementCount && static_cast<uint32_t>(expandAt) + i < 4u; ++i) {
            candidates.nodes[static_cast<uint32_t>(expandAt) + i] = replacement[i];
        }
        candidates.count = newCount;
    }
    return candidates;
}

[[nodiscard]] uint32_t addPacked(BvhBuildResult& result, int binaryNodeIdx) {
    const BinaryBvhNode& binaryNode = result.binaryNodes[static_cast<size_t>(binaryNodeIdx)];
    const uint32_t packedIdx = static_cast<uint32_t>(result.packedNodes.size());
    PackedBvhNode packed;
    packed.boundsMin = binaryNode.boundsMin;
    packed.boundsMax = binaryNode.boundsMax;
    packed.leaf = isLeaf(binaryNode);
    packed.triOffset = packed.leaf ? binaryNode.triOffset : 0;
    packed.triCount = packed.leaf ? binaryNode.triCount : 0;
    packed.mortonFirst = packed.leaf && binaryNode.triCount > 0 ? result.mortonCodes[result.leafTriangleIndices[binaryNode.triOffset]] : 0;
    result.packedNodes.push_back(packed);

    if (!packed.leaf) {
        const Bvh4Children childBinaryNodes = collectBvh4Children(result.binaryNodes, binaryNodeIdx);
        result.packedNodes[packedIdx].childCount = childBinaryNodes.count;
        for (uint32_t i = 0; i < childBinaryNodes.count; ++i) {
            result.packedNodes[packedIdx].children[i] = addPacked(result, childBinaryNodes.nodes[i]);
        }
    }

    return packedIdx;
}

void threadRopes(std::vector<PackedBvhNode>& nodes, uint32_t nodeIdx, int ropeIdx) {
    PackedBvhNode& node = nodes[nodeIdx];
    node.rope = ropeIdx;
    if (node.leaf) {
        return;
    }
    for (uint32_t i = 0; i < node.childCount; ++i) {
        const int nextSibling = i + 1u < node.childCount ? static_cast<int>(node.children[i + 1u]) : ropeIdx;
        threadRopes(nodes, node.children[i], nextSibling);
    }
}

} // namespace

void collapseBinaryBvhToBvh4(BvhBuildResult& result) {
    result.packedNodes.clear();
    if (result.binaryNodes.empty()) {
        return;
    }
    result.packedNodes.reserve(result.binaryNodes.size());
    (void)addPacked(result, 0);
    threadRopes(result.packedNodes, 0, -1);
}

} // namespace rtv
