#include "rtv/LightBvh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>

namespace rtv {

namespace {

constexpr uint32_t kBinCount = 8;
constexpr float kMinPower = 1.0e-8f;

struct BuildItem {
    LightBvhPrimitive primitive{};
};

struct Bounds {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};
};

struct RangeSummary {
    Bounds bounds{};
    Bounds centroidBounds{};
    float power = 0.0f;
};

[[nodiscard]] Bounds emptyBounds() {
    return {};
}

void expand(Bounds& bounds, glm::vec3 p) {
    bounds.min = glm::min(bounds.min, p);
    bounds.max = glm::max(bounds.max, p);
}

void expand(Bounds& bounds, const Bounds& other) {
    expand(bounds, other.min);
    expand(bounds, other.max);
}

[[nodiscard]] float surfaceArea(const Bounds& bounds) {
    const glm::vec3 extent = glm::max(bounds.max - bounds.min, glm::vec3(0.0f));
    return 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x);
}

[[nodiscard]] int longestAxis(const Bounds& bounds) {
    const glm::vec3 extent = bounds.max - bounds.min;
    if (extent.x >= extent.y && extent.x >= extent.z) {
        return 0;
    }
    return extent.y >= extent.z ? 1 : 2;
}

[[nodiscard]] float axisValue(glm::vec3 value, int axis) {
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

[[nodiscard]] RangeSummary summarize(const std::vector<BuildItem>& items, uint32_t begin, uint32_t end) {
    RangeSummary summary;
    for (uint32_t i = begin; i < end; ++i) {
        const LightBvhPrimitive& primitive = items[i].primitive;
        expand(summary.bounds, primitive.boundsMin);
        expand(summary.bounds, primitive.boundsMax);
        expand(summary.centroidBounds, primitive.centroid);
        summary.power += std::max(primitive.power, kMinPower);
    }
    return summary;
}

[[nodiscard]] uint32_t fallbackMedianSplit(std::vector<BuildItem>& items, uint32_t begin, uint32_t end, int axis) {
    const uint32_t mid = begin + (end - begin) / 2u;
    std::nth_element(
        items.begin() + begin,
        items.begin() + mid,
        items.begin() + end,
        [axis](const BuildItem& a, const BuildItem& b) {
            const float av = axisValue(a.primitive.centroid, axis);
            const float bv = axisValue(b.primitive.centroid, axis);
            if (av == bv) {
                return a.primitive.lightIndex < b.primitive.lightIndex;
            }
            return av < bv;
        });
    return mid;
}

[[nodiscard]] uint32_t chooseSahSplit(std::vector<BuildItem>& items, uint32_t begin, uint32_t end, const RangeSummary& summary) {
    const uint32_t count = end - begin;
    const int axis = longestAxis(summary.centroidBounds);
    const float cmin = axisValue(summary.centroidBounds.min, axis);
    const float cmax = axisValue(summary.centroidBounds.max, axis);
    const float extent = cmax - cmin;
    if (count <= 2u || extent <= 1.0e-6f) {
        return fallbackMedianSplit(items, begin, end, axis);
    }

    struct Bin {
        Bounds bounds{};
        float power = 0.0f;
        uint32_t count = 0;
    };

    std::array<Bin, kBinCount> bins{};
    for (uint32_t i = begin; i < end; ++i) {
        const LightBvhPrimitive& primitive = items[i].primitive;
        const float normalized = (axisValue(primitive.centroid, axis) - cmin) / extent;
        const uint32_t bin = std::min(static_cast<uint32_t>(normalized * float(kBinCount)), kBinCount - 1u);
        expand(bins[bin].bounds, primitive.boundsMin);
        expand(bins[bin].bounds, primitive.boundsMax);
        bins[bin].power += std::max(primitive.power, kMinPower);
        ++bins[bin].count;
    }

    std::array<Bounds, kBinCount - 1u> leftBounds{};
    std::array<Bounds, kBinCount - 1u> rightBounds{};
    std::array<float, kBinCount - 1u> leftPower{};
    std::array<float, kBinCount - 1u> rightPower{};
    std::array<uint32_t, kBinCount - 1u> leftCount{};
    std::array<uint32_t, kBinCount - 1u> rightCount{};

    Bounds runningBounds = emptyBounds();
    float runningPower = 0.0f;
    uint32_t runningCount = 0;
    for (uint32_t i = 0; i + 1u < kBinCount; ++i) {
        if (bins[i].count > 0u) {
            expand(runningBounds, bins[i].bounds);
        }
        runningPower += bins[i].power;
        runningCount += bins[i].count;
        leftBounds[i] = runningBounds;
        leftPower[i] = runningPower;
        leftCount[i] = runningCount;
    }

    runningBounds = emptyBounds();
    runningPower = 0.0f;
    runningCount = 0;
    for (uint32_t i = kBinCount - 1u; i > 0u; --i) {
        if (bins[i].count > 0u) {
            expand(runningBounds, bins[i].bounds);
        }
        runningPower += bins[i].power;
        runningCount += bins[i].count;
        rightBounds[i - 1u] = runningBounds;
        rightPower[i - 1u] = runningPower;
        rightCount[i - 1u] = runningCount;
    }

    uint32_t bestSplit = kBinCount;
    float bestCost = std::numeric_limits<float>::max();
    for (uint32_t i = 0; i + 1u < kBinCount; ++i) {
        if (leftCount[i] == 0u || rightCount[i] == 0u) {
            continue;
        }
        const float cost =
            surfaceArea(leftBounds[i]) * leftPower[i] +
            surfaceArea(rightBounds[i]) * rightPower[i];
        if (cost < bestCost) {
            bestCost = cost;
            bestSplit = i;
        }
    }

    if (bestSplit == kBinCount) {
        return fallbackMedianSplit(items, begin, end, axis);
    }

    const auto splitIt = std::partition(
        items.begin() + begin,
        items.begin() + end,
        [axis, cmin, extent, bestSplit](const BuildItem& item) {
            const float normalized = (axisValue(item.primitive.centroid, axis) - cmin) / extent;
            const uint32_t bin = std::min(static_cast<uint32_t>(normalized * float(kBinCount)), kBinCount - 1u);
            return bin <= bestSplit;
        });
    const uint32_t split = static_cast<uint32_t>(std::distance(items.begin(), splitIt));
    if (split == begin || split == end) {
        return fallbackMedianSplit(items, begin, end, axis);
    }
    return split;
}

uint32_t buildRecursive(
    std::vector<BuildItem>& items,
    uint32_t begin,
    uint32_t end,
    uint32_t maxLightsPerLeaf,
    std::vector<LightBvhNode>& nodes) {
    const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
    nodes.emplace_back();

    const uint32_t count = end - begin;
    const RangeSummary summary = summarize(items, begin, end);
    LightBvhNode& node = nodes[nodeIndex];
    node.boundsMin = summary.bounds.min;
    node.boundsMax = summary.bounds.max;
    node.totalPower = summary.power;

    if (count <= std::max(maxLightsPerLeaf, 1u)) {
        node.lightIndex = static_cast<int32_t>(items[begin].primitive.lightIndex);
        node.lightCount = count == 1u ? 1u : 0u;
        return nodeIndex;
    }

    const uint32_t split = chooseSahSplit(items, begin, end, summary);
    const uint32_t firstChild = static_cast<uint32_t>(nodes.size());
    buildRecursive(items, begin, split, maxLightsPerLeaf, nodes);
    buildRecursive(items, split, end, maxLightsPerLeaf, nodes);

    LightBvhNode& inner = nodes[nodeIndex];
    inner.lightIndex = -1;
    inner.childOffset = firstChild;
    inner.childCount = 2u;
    inner.lightCount = 0u;
    return nodeIndex;
}

void accumulateStats(
    const std::vector<LightBvhNode>& nodes,
    uint32_t nodeIndex,
    uint32_t depth,
    LightBvhStats& stats,
    float& weightedDepthSum) {
    if (nodeIndex >= nodes.size()) {
        return;
    }

    const LightBvhNode& node = nodes[nodeIndex];
    stats.maxDepth = std::max(stats.maxDepth, depth);
    if (node.lightIndex >= 0) {
        ++stats.leafCount;
        stats.maxLeafLightCount = std::max(stats.maxLeafLightCount, node.lightCount);
        stats.minLeafPower = stats.leafCount == 1u ? node.totalPower : std::min(stats.minLeafPower, node.totalPower);
        stats.maxLeafPower = std::max(stats.maxLeafPower, node.totalPower);
        weightedDepthSum += node.totalPower * static_cast<float>(depth + 1u);
        return;
    }

    ++stats.innerNodeCount;
    for (uint32_t i = 0; i < node.childCount; ++i) {
        accumulateStats(nodes, node.childOffset + i, depth + 1u, stats, weightedDepthSum);
    }
}

} // namespace

std::vector<LightBvhNode> buildLightBvh(
    const std::vector<float>& lightPower,
    uint32_t maxLightsPerLeaf) {
    std::vector<LightBvhPrimitive> primitives;
    primitives.reserve(lightPower.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(lightPower.size()); ++i) {
        const glm::vec3 p(static_cast<float>(i), 0.0f, 0.0f);
        primitives.push_back(LightBvhPrimitive{
            .boundsMin = p,
            .boundsMax = p,
            .centroid = p,
            .power = std::max(lightPower[i], kMinPower),
            .lightIndex = i,
        });
    }
    return buildLightBvh(primitives, maxLightsPerLeaf);
}

std::vector<LightBvhNode> buildLightBvh(
    const std::vector<LightBvhPrimitive>& lights,
    uint32_t maxLightsPerLeaf) {
    if (lights.empty()) {
        return {};
    }

    std::vector<BuildItem> items;
    items.reserve(lights.size());
    for (const LightBvhPrimitive& light : lights) {
        LightBvhPrimitive primitive = light;
        primitive.power = std::max(primitive.power, kMinPower);
        primitive.centroid = (primitive.boundsMin + primitive.boundsMax) * 0.5f;
        items.push_back({primitive});
    }

    std::vector<LightBvhNode> nodes;
    nodes.reserve(items.size() * 2u);
    // GPU leaves encode one arbitrary light index. Keep explicit-primitive builds to single-light
    // leaves so spatially sorted leaves never imply a contiguous range in the original light buffer.
    const uint32_t effectiveMaxLightsPerLeaf = std::min(std::max(maxLightsPerLeaf, 1u), 1u);
    buildRecursive(items, 0u, static_cast<uint32_t>(items.size()), effectiveMaxLightsPerLeaf, nodes);
    return nodes;
}

LightBvhStats computeLightBvhStats(const std::vector<LightBvhNode>& nodes) {
    LightBvhStats stats;
    stats.nodeCount = static_cast<uint32_t>(nodes.size());
    if (nodes.empty()) {
        return stats;
    }

    stats.totalPower = nodes.front().totalPower;
    float weightedDepthSum = 0.0f;
    accumulateStats(nodes, 0u, 0u, stats, weightedDepthSum);
    stats.estimatedAverageTraversalSteps = stats.totalPower > 0.0f ? weightedDepthSum / stats.totalPower : 0.0f;
    return stats;
}

std::vector<glm::vec4> packLightBvhNodesForGpu(const std::vector<LightBvhNode>& nodes) {
    std::vector<glm::vec4> packed;
    packed.reserve(nodes.size() * 2);
    for (const LightBvhNode& node : nodes) {
        packed.push_back({node.boundsMin, node.totalPower});
        if (node.lightIndex >= 0) {
            uint32_t leafInfo = (1u << 31u) |
                (static_cast<uint32_t>(node.lightIndex) & 0x7fffu) |
                ((node.lightCount & 0x3fffu) << 16u);
            float w;
            std::memcpy(&w, &leafInfo, sizeof(float));
            packed.push_back({node.boundsMax, w});
        } else {
            uint32_t innerInfo = (node.childCount & 0xffffu) | ((node.childOffset & 0xffffu) << 16u);
            float w;
            std::memcpy(&w, &innerInfo, sizeof(float));
            packed.push_back({node.boundsMax, w});
        }
    }
    return packed;
}

} // namespace rtv
