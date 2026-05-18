#include "rtv/SahBuilder.h"

#include <algorithm>
#include <array>
#include <numeric>

namespace rtv {

namespace {

constexpr uint32_t binCount = 12;
constexpr uint32_t leafTriLimit = 4;
constexpr uint32_t maxDepth = 24;

[[nodiscard]] glm::vec3 minVec(glm::vec3 a, glm::vec3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

[[nodiscard]] glm::vec3 maxVec(glm::vec3 a, glm::vec3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

[[nodiscard]] Bounds3 emptyBounds() {
    return {};
}

void growBounds(Bounds3& bounds, const BvhTriangle& tri) {
    bounds.min = minVec(bounds.min, tri.boundsMin);
    bounds.max = maxVec(bounds.max, tri.boundsMax);
}

[[nodiscard]] Bounds3 mergeBounds(const Bounds3& left, const Bounds3& right) {
    return {minVec(left.min, right.min), maxVec(left.max, right.max)};
}

[[nodiscard]] float surfaceArea(const Bounds3& bounds) {
    const glm::vec3 d = glm::max(bounds.max - bounds.min, glm::vec3(0.0f));
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

[[nodiscard]] Bounds3 triangleBounds(const std::vector<BvhTriangle>& tris, const std::vector<uint32_t>& refs, size_t begin, size_t end) {
    Bounds3 bounds = emptyBounds();
    for (size_t i = begin; i < end; ++i) {
        growBounds(bounds, tris[refs[i]]);
    }
    return bounds;
}

[[nodiscard]] Bounds3 centroidBounds(const std::vector<BvhTriangle>& tris, const std::vector<uint32_t>& refs, size_t begin, size_t end) {
    Bounds3 bounds = emptyBounds();
    for (size_t i = begin; i < end; ++i) {
        const uint32_t ref = refs[i];
        bounds.min = minVec(bounds.min, tris[ref].centroid);
        bounds.max = maxVec(bounds.max, tris[ref].centroid);
    }
    return bounds;
}

[[nodiscard]] int longestCentroidAxis(const Bounds3& bounds) {
    const glm::vec3 e = bounds.max - bounds.min;
    if (e.y > e.x && e.y >= e.z) {
        return 1;
    }
    if (e.z > e.x && e.z > e.y) {
        return 2;
    }
    return 0;
}

struct Split {
    int axis = -1;
    int splitBin = -1;
};

[[nodiscard]] Split findBinnedSahSplit(const std::vector<BvhTriangle>& tris, const std::vector<uint32_t>& refs, size_t begin, size_t end, const Bounds3& cBounds) {
    Split best;
    float bestCost = 1.0e30f;

    for (int axis = 0; axis < 3; ++axis) {
        const float minC = cBounds.min[axis];
        const float extent = cBounds.max[axis] - minC;
        if (extent <= 1.0e-8f) {
            continue;
        }

        struct Bin {
            uint32_t count = 0;
            Bounds3 bounds{};
        };
        std::array<Bin, binCount> bins{};
        for (size_t i = begin; i < end; ++i) {
            const uint32_t ref = refs[i];
            const BvhTriangle& tri = tris[ref];
            const float normalized = (tri.centroid[axis] - minC) / extent;
            const uint32_t bin = std::min(binCount - 1u, static_cast<uint32_t>(std::max(0.0f, std::floor(normalized * static_cast<float>(binCount)))));
            bins[bin].count++;
            growBounds(bins[bin].bounds, tri);
        }

        std::array<uint32_t, binCount> prefixCounts{};
        std::array<uint32_t, binCount> suffixCounts{};
        std::array<Bounds3, binCount> prefixBounds{};
        std::array<Bounds3, binCount> suffixBounds{};

        for (uint32_t i = 0; i < binCount; ++i) {
            prefixCounts[i] = bins[i].count + (i > 0 ? prefixCounts[i - 1] : 0);
            prefixBounds[i] = i > 0 ? mergeBounds(prefixBounds[i - 1], bins[i].bounds) : bins[i].bounds;
        }
        for (int i = static_cast<int>(binCount) - 1; i >= 0; --i) {
            suffixCounts[static_cast<size_t>(i)] = bins[static_cast<size_t>(i)].count + (i < static_cast<int>(binCount) - 1 ? suffixCounts[static_cast<size_t>(i + 1)] : 0);
            suffixBounds[static_cast<size_t>(i)] = i < static_cast<int>(binCount) - 1 ? mergeBounds(suffixBounds[static_cast<size_t>(i + 1)], bins[static_cast<size_t>(i)].bounds) : bins[static_cast<size_t>(i)].bounds;
        }

        for (uint32_t splitBin = 0; splitBin + 1 < binCount; ++splitBin) {
            const uint32_t leftCount = prefixCounts[splitBin];
            const uint32_t rightCount = suffixCounts[splitBin + 1];
            if (leftCount == 0 || rightCount == 0) {
                continue;
            }
            const float cost = static_cast<float>(leftCount) * surfaceArea(prefixBounds[splitBin]) +
                               static_cast<float>(rightCount) * surfaceArea(suffixBounds[splitBin + 1]);
            if (cost < bestCost) {
                bestCost = cost;
                best.axis = axis;
                best.splitBin = static_cast<int>(splitBin);
            }
        }
    }

    return best;
}

[[nodiscard]] int buildNode(BvhBuildResult& result, std::vector<uint32_t>& refs, size_t begin, size_t end, uint32_t depth) {
    const Bounds3 bounds = triangleBounds(result.triangles, refs, begin, end);
    const size_t count = end - begin;
    if (count <= leafTriLimit || depth >= maxDepth) {
        BinaryBvhNode leaf;
        leaf.boundsMin = bounds.min;
        leaf.boundsMax = bounds.max;
        leaf.triOffset = static_cast<uint32_t>(result.leafTriangleIndices.size());
        leaf.triCount = static_cast<uint32_t>(count);
        result.leafTriangleIndices.insert(result.leafTriangleIndices.end(), refs.begin() + static_cast<std::ptrdiff_t>(begin), refs.begin() + static_cast<std::ptrdiff_t>(end));
        const int nodeIdx = static_cast<int>(result.binaryNodes.size());
        result.binaryNodes.push_back(leaf);
        return nodeIdx;
    }

    const Bounds3 cBounds = centroidBounds(result.triangles, refs, begin, end);
    const Split split = findBinnedSahSplit(result.triangles, refs, begin, end, cBounds);
    size_t mid = begin;

    if (split.axis >= 0) {
        const float minC = cBounds.min[split.axis];
        const float extent = cBounds.max[split.axis] - minC;
        auto midIt = std::partition(refs.begin() + static_cast<std::ptrdiff_t>(begin), refs.begin() + static_cast<std::ptrdiff_t>(end), [&](uint32_t ref) {
            const float normalized = (result.triangles[ref].centroid[split.axis] - minC) / extent;
            const uint32_t bin = std::min(binCount - 1u, static_cast<uint32_t>(std::max(0.0f, std::floor(normalized * static_cast<float>(binCount)))));
            return bin <= static_cast<uint32_t>(split.splitBin);
        });
        mid = static_cast<size_t>(std::distance(refs.begin(), midIt));
    }

    if (mid == begin || mid == end) {
        const int axis = longestCentroidAxis(cBounds);
        mid = begin + count / 2u;
        std::nth_element(refs.begin() + static_cast<std::ptrdiff_t>(begin), refs.begin() + static_cast<std::ptrdiff_t>(mid), refs.begin() + static_cast<std::ptrdiff_t>(end), [&](uint32_t a, uint32_t b) {
            return result.triangles[a].centroid[axis] < result.triangles[b].centroid[axis];
        });
    }

    if (mid == begin || mid == end) {
        BinaryBvhNode leaf;
        leaf.boundsMin = bounds.min;
        leaf.boundsMax = bounds.max;
        leaf.triOffset = static_cast<uint32_t>(result.leafTriangleIndices.size());
        leaf.triCount = static_cast<uint32_t>(count);
        result.leafTriangleIndices.insert(result.leafTriangleIndices.end(), refs.begin() + static_cast<std::ptrdiff_t>(begin), refs.begin() + static_cast<std::ptrdiff_t>(end));
        const int nodeIdx = static_cast<int>(result.binaryNodes.size());
        result.binaryNodes.push_back(leaf);
        return nodeIdx;
    }

    BinaryBvhNode node;
    node.boundsMin = bounds.min;
    node.boundsMax = bounds.max;
    const int nodeIdx = static_cast<int>(result.binaryNodes.size());
    result.binaryNodes.push_back(node);
    result.binaryNodes[nodeIdx].left = buildNode(result, refs, begin, mid, depth + 1u);
    result.binaryNodes[nodeIdx].right = buildNode(result, refs, mid, end, depth + 1u);
    return nodeIdx;
}

} // namespace

void buildSahBinaryBvh(BvhBuildResult& result) {
    result.binaryNodes.clear();
    result.leafTriangleIndices.clear();
    result.mortonCodes.clear();

    if (result.triangles.empty()) {
        return;
    }

    std::vector<uint32_t> refs(result.triangles.size());
    std::iota(refs.begin(), refs.end(), 0u);
    const Bounds3 cBounds = centroidBounds(result.triangles, refs, 0, refs.size());
    result.mortonCodes.resize(result.triangles.size());
    for (size_t i = 0; i < result.triangles.size(); ++i) {
        result.mortonCodes[i] = morton3D30(result.triangles[i].centroid, cBounds);
    }
    std::stable_sort(refs.begin(), refs.end(), [&](uint32_t a, uint32_t b) {
        if (result.mortonCodes[a] == result.mortonCodes[b]) {
            return a < b;
        }
        return result.mortonCodes[a] < result.mortonCodes[b];
    });
    result.binaryNodes.reserve(result.triangles.size());
    result.leafTriangleIndices.reserve(result.triangles.size());
    (void)buildNode(result, refs, 0, refs.size(), 0);
}

int buildMortonNode(BvhBuildResult& result, const std::vector<uint32_t>& refs, size_t begin, size_t end, uint32_t depth) {
    const Bounds3 bounds = triangleBounds(result.triangles, refs, begin, end);
    const size_t count = end - begin;
    if (count <= leafTriLimit || depth >= maxDepth) {
        BinaryBvhNode leaf;
        leaf.boundsMin = bounds.min;
        leaf.boundsMax = bounds.max;
        leaf.triOffset = static_cast<uint32_t>(result.leafTriangleIndices.size());
        leaf.triCount = static_cast<uint32_t>(count);
        result.leafTriangleIndices.insert(result.leafTriangleIndices.end(), refs.begin() + static_cast<std::ptrdiff_t>(begin), refs.begin() + static_cast<std::ptrdiff_t>(end));
        const int nodeIdx = static_cast<int>(result.binaryNodes.size());
        result.binaryNodes.push_back(leaf);
        return nodeIdx;
    }

    BinaryBvhNode node;
    node.boundsMin = bounds.min;
    node.boundsMax = bounds.max;
    const int nodeIdx = static_cast<int>(result.binaryNodes.size());
    result.binaryNodes.push_back(node);
    const size_t mid = begin + count / 2u;
    result.binaryNodes[nodeIdx].left = buildMortonNode(result, refs, begin, mid, depth + 1u);
    result.binaryNodes[nodeIdx].right = buildMortonNode(result, refs, mid, end, depth + 1u);
    return nodeIdx;
}

void buildMortonBinaryBvh(BvhBuildResult& result) {
    result.binaryNodes.clear();
    result.leafTriangleIndices.clear();
    result.mortonCodes.clear();

    if (result.triangles.empty()) {
        return;
    }

    std::vector<uint32_t> refs(result.triangles.size());
    std::iota(refs.begin(), refs.end(), 0u);
    const Bounds3 cBounds = centroidBounds(result.triangles, refs, 0, refs.size());
    result.mortonCodes.resize(result.triangles.size());
    for (size_t i = 0; i < result.triangles.size(); ++i) {
        result.mortonCodes[i] = morton3D30(result.triangles[i].centroid, cBounds);
    }
    std::sort(refs.begin(), refs.end(), [&](uint32_t a, uint32_t b) {
        if (result.mortonCodes[a] == result.mortonCodes[b]) {
            return a < b;
        }
        return result.mortonCodes[a] < result.mortonCodes[b];
    });
    result.binaryNodes.reserve(result.triangles.size());
    result.leafTriangleIndices.reserve(result.triangles.size());
    (void)buildMortonNode(result, refs, 0, refs.size(), 0);
}

} // namespace rtv
