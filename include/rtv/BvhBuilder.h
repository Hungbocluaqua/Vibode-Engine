#pragma once

#include "rtv/MortonCodes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace rtv {

struct BvhTriangle {
    glm::vec3 v0{};
    glm::vec3 v1{};
    glm::vec3 v2{};
    glm::vec2 uv0{};
    glm::vec2 uv1{};
    glm::vec2 uv2{};
    glm::vec3 normal{};
    glm::vec3 n0{};
    glm::vec3 n1{};
    glm::vec3 n2{};
    glm::vec3 tangent{1.0f, 0.0f, 0.0f};
    glm::vec3 bitangent{0.0f, 1.0f, 0.0f};
    glm::vec4 t0{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 t1{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 t2{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec3 boundsMin{};
    glm::vec3 boundsMax{};
    glm::vec3 centroid{};
    uint32_t material = 0;
    uint32_t sourceIndex = 0;
};

struct BinaryBvhNode {
    glm::vec3 boundsMin{};
    glm::vec3 boundsMax{};
    int left = -1;
    int right = -1;
    uint32_t triOffset = 0;
    uint32_t triCount = 0;
};

struct PackedBvhNode {
    glm::vec3 boundsMin{};
    glm::vec3 boundsMax{};
    bool leaf = false;
    int rope = -1;
    uint32_t children[4]{0, 0, 0, 0};
    uint32_t childCount = 0;
    uint32_t triOffset = 0;
    uint32_t triCount = 0;
    uint32_t mortonFirst = 0;
};

struct BvhBuildResult {
    std::vector<BvhTriangle> triangles;
    std::vector<uint32_t> leafTriangleIndices;
    std::vector<uint32_t> mortonCodes;
    std::vector<BinaryBvhNode> binaryNodes;
    std::vector<PackedBvhNode> packedNodes;
};

enum class BvhBuildQuality {
    BinnedSah,
    MortonFast,
};

[[nodiscard]] BvhBuildResult buildBvh(
    const std::vector<glm::vec3>& vertices,
    const std::vector<uint32_t>& indices,
    const std::vector<uint32_t>& faceMaterials,
    const std::vector<glm::vec2>* texcoords = nullptr,
    const std::vector<glm::vec3>* normals = nullptr,
    const std::vector<glm::vec4>* tangents = nullptr,
    BvhBuildQuality quality = BvhBuildQuality::BinnedSah);

[[nodiscard]] std::vector<glm::vec4> packBvhNodesForGpu(const std::vector<PackedBvhNode>& nodes);
[[nodiscard]] std::vector<glm::vec4> packTrianglesForGpu(const BvhBuildResult& bvh);

} // namespace rtv
