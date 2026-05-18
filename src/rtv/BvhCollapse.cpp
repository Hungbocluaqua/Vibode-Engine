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

[[nodiscard]] std::vector<int> collectBvh4Children(const std::vector<BinaryBvhNode>& nodes, int binaryNodeIdx) {
    const BinaryBvhNode& root = nodes[static_cast<size_t>(binaryNodeIdx)];
    std::vector<int> candidates;
    if (root.left >= 0) {
        candidates.push_back(root.left);
    }
    if (root.right >= 0) {
        candidates.push_back(root.right);
    }

    while (candidates.size() < 4) {
        int expandAt = -1;
        float expandArea = -1.0f;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const BinaryBvhNode& candidate = nodes[static_cast<size_t>(candidates[i])];
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

        const BinaryBvhNode& expanded = nodes[static_cast<size_t>(candidates[static_cast<size_t>(expandAt)])];
        std::vector<int> replacement;
        if (expanded.left >= 0) {
            replacement.push_back(expanded.left);
        }
        if (expanded.right >= 0) {
            replacement.push_back(expanded.right);
        }
        candidates.erase(candidates.begin() + expandAt);
        candidates.insert(candidates.begin() + expandAt, replacement.begin(), replacement.end());
    }
    if (candidates.size() > 4) {
        candidates.resize(4);
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
        const std::vector<int> childBinaryNodes = collectBvh4Children(result.binaryNodes, binaryNodeIdx);
        result.packedNodes[packedIdx].childCount = static_cast<uint32_t>(childBinaryNodes.size());
        for (size_t i = 0; i < childBinaryNodes.size(); ++i) {
            result.packedNodes[packedIdx].children[i] = addPacked(result, childBinaryNodes[i]);
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
    (void)addPacked(result, 0);
    threadRopes(result.packedNodes, 0, -1);
}

} // namespace rtv
