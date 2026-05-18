#include "rtv/BvhBuilder.h"

#include "rtv/BvhCollapse.h"
#include "rtv/SahBuilder.h"

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

constexpr float triangleBoundsPadding = 0.0005f;

[[nodiscard]] glm::vec3 minVec(glm::vec3 a, glm::vec3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

[[nodiscard]] glm::vec3 maxVec(glm::vec3 a, glm::vec3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

void buildTriangleFrame(BvhTriangle& tri) {
    const glm::vec3 dp1 = tri.v1 - tri.v0;
    const glm::vec3 dp2 = tri.v2 - tri.v0;
    const glm::vec2 duv1 = tri.uv1 - tri.uv0;
    const glm::vec2 duv2 = tri.uv2 - tri.uv0;
    const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
    if (std::abs(determinant) > 1.0e-8f) {
        const float invDet = 1.0f / determinant;
        glm::vec3 tangent = (dp1 * duv2.y - dp2 * duv1.y) * invDet;
        tangent -= tri.normal * glm::dot(tri.normal, tangent);
        const float tangentLen2 = glm::dot(tangent, tangent);
        if (tangentLen2 > 1.0e-10f) {
            tri.tangent = tangent / std::sqrt(tangentLen2);
            tri.bitangent = glm::normalize(glm::cross(tri.normal, tri.tangent)) * (determinant < 0.0f ? -1.0f : 1.0f);
            return;
        }
    }

    const glm::vec3 helper = std::abs(tri.normal.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    tri.tangent = glm::normalize(glm::cross(helper, tri.normal));
    tri.bitangent = glm::normalize(glm::cross(tri.normal, tri.tangent));
}

[[nodiscard]] glm::vec3 normalizedOr(glm::vec3 v, glm::vec3 fallback) {
    const float len2 = glm::dot(v, v);
    return len2 > 1.0e-10f ? v / std::sqrt(len2) : fallback;
}

} // namespace

BvhBuildResult buildBvh(
    const std::vector<glm::vec3>& vertices,
    const std::vector<uint32_t>& indices,
    const std::vector<uint32_t>& faceMaterials,
    const std::vector<glm::vec2>* texcoords,
    const std::vector<glm::vec3>* normals,
    const std::vector<glm::vec4>* tangents,
    BvhBuildQuality quality) {
    BvhBuildResult result;
    result.triangles.reserve(indices.size() / 3);
    for (size_t i = 0; i < indices.size() / 3; ++i) {
        const glm::vec3 v0 = vertices[indices[i * 3 + 0]];
        const glm::vec3 v1 = vertices[indices[i * 3 + 1]];
        const glm::vec3 v2 = vertices[indices[i * 3 + 2]];
        const glm::vec3 e1 = v1 - v0;
        const glm::vec3 e2 = v2 - v0;
        glm::vec3 normal = glm::cross(e1, e2);
        const float normalLen2 = glm::dot(normal, normal);
        normal = normalLen2 > 0.0f ? normal / std::sqrt(normalLen2) : glm::vec3(0.0f, 1.0f, 0.0f);

        BvhTriangle tri;
        tri.v0 = v0;
        tri.v1 = v1;
        tri.v2 = v2;
        if (texcoords != nullptr &&
            indices[i * 3 + 0] < texcoords->size() &&
            indices[i * 3 + 1] < texcoords->size() &&
            indices[i * 3 + 2] < texcoords->size()) {
            tri.uv0 = (*texcoords)[indices[i * 3 + 0]];
            tri.uv1 = (*texcoords)[indices[i * 3 + 1]];
            tri.uv2 = (*texcoords)[indices[i * 3 + 2]];
        }
        tri.normal = normal;
        tri.n0 = normal;
        tri.n1 = normal;
        tri.n2 = normal;
        buildTriangleFrame(tri);
        tri.t0 = {tri.tangent, 1.0f};
        tri.t1 = {tri.tangent, 1.0f};
        tri.t2 = {tri.tangent, 1.0f};
        if (normals != nullptr &&
            indices[i * 3 + 0] < normals->size() &&
            indices[i * 3 + 1] < normals->size() &&
            indices[i * 3 + 2] < normals->size()) {
            tri.n0 = normalizedOr((*normals)[indices[i * 3 + 0]], normal);
            tri.n1 = normalizedOr((*normals)[indices[i * 3 + 1]], normal);
            tri.n2 = normalizedOr((*normals)[indices[i * 3 + 2]], normal);
        }
        if (tangents != nullptr &&
            indices[i * 3 + 0] < tangents->size() &&
            indices[i * 3 + 1] < tangents->size() &&
            indices[i * 3 + 2] < tangents->size()) {
            const glm::vec4 src0 = (*tangents)[indices[i * 3 + 0]];
            const glm::vec4 src1 = (*tangents)[indices[i * 3 + 1]];
            const glm::vec4 src2 = (*tangents)[indices[i * 3 + 2]];
            tri.t0 = {normalizedOr(glm::vec3(src0), tri.tangent), src0.w < 0.0f ? -1.0f : 1.0f};
            tri.t1 = {normalizedOr(glm::vec3(src1), tri.tangent), src1.w < 0.0f ? -1.0f : 1.0f};
            tri.t2 = {normalizedOr(glm::vec3(src2), tri.tangent), src2.w < 0.0f ? -1.0f : 1.0f};
        }
        tri.boundsMin = minVec(minVec(v0, v1), v2) - glm::vec3(triangleBoundsPadding);
        tri.boundsMax = maxVec(maxVec(v0, v1), v2) + glm::vec3(triangleBoundsPadding);
        tri.centroid = (v0 + v1 + v2) / 3.0f;
        tri.material = i < faceMaterials.size() ? faceMaterials[i] : 0u;
        tri.sourceIndex = static_cast<uint32_t>(i);
        result.triangles.push_back(tri);
    }

    if (quality == BvhBuildQuality::MortonFast) {
        buildMortonBinaryBvh(result);
    } else {
        buildSahBinaryBvh(result);
    }
    collapseBinaryBvhToBvh4(result);
    return result;
}

std::vector<glm::vec4> packBvhNodesForGpu(const std::vector<PackedBvhNode>& nodes) {
    std::vector<glm::vec4> packed;
    packed.reserve(nodes.size() * 4);
    for (const PackedBvhNode& node : nodes) {
        packed.push_back({node.boundsMin, node.leaf ? 1.0f : 0.0f});
        packed.push_back({node.boundsMax, node.rope >= 0 ? static_cast<float>(node.rope + 1) : 0.0f});
        if (node.leaf) {
            packed.push_back({static_cast<float>(node.triOffset), static_cast<float>(node.triCount), 0.0f, 0.0f});
        } else {
            packed.push_back({
                static_cast<float>(node.children[0]),
                static_cast<float>(node.children[1]),
                static_cast<float>(node.children[2]),
                static_cast<float>(node.children[3]),
            });
        }
        packed.push_back({static_cast<float>(node.childCount), static_cast<float>(node.mortonFirst), 0.0f, 0.0f});
    }
    return packed;
}

std::vector<glm::vec4> packTrianglesForGpu(const BvhBuildResult& bvh) {
    std::vector<glm::vec4> packed;
    packed.reserve(bvh.leafTriangleIndices.size() * 12);
    for (uint32_t idx : bvh.leafTriangleIndices) {
        const BvhTriangle& tri = bvh.triangles[idx];
        packed.push_back({tri.v0, 0.0f});
        packed.push_back({tri.v1, 0.0f});
        packed.push_back({tri.v2, 0.0f});
        packed.push_back({tri.normal, static_cast<float>(tri.material)});
        packed.push_back({tri.uv0, tri.uv1});
        packed.push_back({tri.uv2, 0.0f, 0.0f});
        packed.push_back({tri.n0, 0.0f});
        packed.push_back({tri.n1, 0.0f});
        packed.push_back({tri.n2, 0.0f});
        packed.push_back(tri.t0);
        packed.push_back(tri.t1);
        packed.push_back(tri.t2);
    }
    return packed;
}

} // namespace rtv
