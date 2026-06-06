#pragma once

#include "rtv/TextureAsset.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <cmath>
#include <string>
#include <vector>

namespace rtv {

struct MeshAssetHandle {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

struct MaterialAssetHandle {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

struct MeshVertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 texcoord{};
    glm::vec2 texcoord1{};
    glm::vec4 color{1.0f};
    glm::uvec4 joints{0u};
    glm::vec4 weights{0.0f};
};

struct TextureTransformAsset {
    uint32_t enabled = 0;
    glm::vec2 offset{0.0f};
    glm::vec2 scale{1.0f};
    float rotation = 0.0f;
    uint32_t texCoord = 0;
};

struct MaterialAsset {
    std::string name;
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float iorFactor = 1.5f;
    float alphaCutoff = 0.5f;
    uint32_t alphaMode = 0;
    uint32_t doubleSided = 0;
    uint32_t hasIor = 0;
    uint32_t hasClearcoat = 0;
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    uint32_t hasTransmission = 0;
    float transmissionFactor = 0.0f;
    uint32_t hasVolume = 0;
    float volumeThicknessFactor = 0.0f;
    float volumeAttenuationDistance = 0.0f;
    glm::vec3 volumeAttenuationColor{1.0f};
    uint32_t hasDispersion = 0;
    float dispersionFactor = 0.0f;
    uint32_t hasSpecular = 0;
    float specularFactor = 1.0f;
    glm::vec3 specularColorFactor{1.0f};
    uint32_t hasSheen = 0;
    glm::vec3 sheenColorFactor{0.0f};
    float sheenRoughnessFactor = 0.0f;
    uint32_t hasIridescence = 0;
    float iridescenceFactor = 0.0f;
    float iridescenceIor = 1.3f;
    float iridescenceThicknessMinimum = 100.0f;
    float iridescenceThicknessMaximum = 400.0f;
    uint32_t hasEmissiveStrength = 0;
    float emissiveStrength = 1.0f;
    uint32_t hasAnisotropy = 0;
    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    float occlusionStrength = 1.0f;
    uint32_t useConductorOptics = 0;
    glm::vec3 conductorEta{0.0f};
    glm::vec3 conductorK{0.0f};
    TextureAssetHandle baseColorTexture{};
    TextureAssetHandle normalTexture{};
    TextureAssetHandle metallicRoughnessTexture{};
    TextureAssetHandle emissiveTexture{};
    TextureAssetHandle clearcoatTexture{};
    TextureAssetHandle clearcoatRoughnessTexture{};
    TextureAssetHandle clearcoatNormalTexture{};
    TextureAssetHandle transmissionTexture{};
    TextureAssetHandle volumeThicknessTexture{};
    TextureAssetHandle specularTexture{};
    TextureAssetHandle specularColorTexture{};
    TextureAssetHandle sheenColorTexture{};
    TextureAssetHandle sheenRoughnessTexture{};
    TextureAssetHandle iridescenceTexture{};
    TextureAssetHandle iridescenceThicknessTexture{};
    TextureAssetHandle anisotropyTexture{};
    TextureAssetHandle occlusionTexture{};
    TextureTransformAsset baseColorTextureTransform{};
    TextureTransformAsset metallicRoughnessTextureTransform{};
    TextureTransformAsset normalTextureTransform{};
    TextureTransformAsset emissiveTextureTransform{};
    TextureTransformAsset occlusionTextureTransform{};
    TextureTransformAsset clearcoatTextureTransform{};
    TextureTransformAsset clearcoatRoughnessTextureTransform{};
    TextureTransformAsset clearcoatNormalTextureTransform{};
    TextureTransformAsset transmissionTextureTransform{};
    TextureTransformAsset volumeThicknessTextureTransform{};
    TextureTransformAsset specularTextureTransform{};
    TextureTransformAsset specularColorTextureTransform{};
    TextureTransformAsset sheenColorTextureTransform{};
    TextureTransformAsset sheenRoughnessTextureTransform{};
    TextureTransformAsset iridescenceTextureTransform{};
    TextureTransformAsset iridescenceThicknessTextureTransform{};
    TextureTransformAsset anisotropyTextureTransform{};
    uint32_t shaderCompatibilityMask = 1u;
};

constexpr uint32_t kMaterialAlphaModeOpaque = 0u;
constexpr uint32_t kMaterialAlphaModeMask = 1u;
constexpr uint32_t kMaterialAlphaModeBlend = 2u;

constexpr uint32_t kPrimitiveAlphaClassOpaque = 0u;
constexpr uint32_t kPrimitiveAlphaClassAlphaTested = 1u;
constexpr uint32_t kPrimitiveAlphaClassBlended = 2u;

[[nodiscard]] inline uint32_t primitiveAlphaClassForMaterial(const MaterialAsset* material) {
    if (material == nullptr) {
        return kPrimitiveAlphaClassOpaque;
    }
    if (material->alphaMode == kMaterialAlphaModeMask) {
        return kPrimitiveAlphaClassAlphaTested;
    }
    if (material->alphaMode == kMaterialAlphaModeBlend) {
        return kPrimitiveAlphaClassBlended;
    }
    return kPrimitiveAlphaClassOpaque;
}

constexpr uint32_t kMaterialClosureFlagDiffuse      = 1u << 0u;
constexpr uint32_t kMaterialClosureFlagSpecular     = 1u << 1u;
constexpr uint32_t kMaterialClosureFlagSss          = 1u << 2u;
constexpr uint32_t kMaterialClosureFlagTransmission = 1u << 3u;
constexpr uint32_t kMaterialClosureFlagClearcoat    = 1u << 4u;
constexpr uint32_t kMaterialClosureFlagSheen        = 1u << 5u;
constexpr uint32_t kMaterialClosureFlagThinFilm     = 1u << 6u;
constexpr uint32_t kMaterialClosureFlagMetal        = 1u << 7u;
constexpr uint32_t kMaterialClosureFlagUnlit        = 1u << 8u;
constexpr uint32_t kMaterialClosureFlagVolume       = 1u << 9u;
constexpr uint32_t kMaterialClosureFlagDispersion   = 1u << 10u;

struct MeshPrimitiveAsset {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    MaterialAssetHandle material{};
    struct MorphTarget {
        std::string name;
        std::vector<glm::vec3> positionDeltas;
        std::vector<glm::vec3> normalDeltas;
        std::vector<glm::vec3> tangentDeltas;
    };
    struct MaterialVariant {
        uint32_t variantIndex = UINT32_MAX;
        std::string variantName;
        MaterialAssetHandle material{};
    };
    std::vector<MorphTarget> morphTargets;
    std::vector<MaterialVariant> materialVariants;
    float alphaCutoff = 0.5f;
    uint32_t alphaMode = kMaterialAlphaModeOpaque;
    bool containsAlphaTestedGeometry = false;
    bool containsBlendedGeometry = false;
};

inline void updatePrimitiveAlphaClassification(MeshPrimitiveAsset& primitive, const MaterialAsset* material) {
    primitive.alphaCutoff = material != nullptr ? material->alphaCutoff : 0.5f;
    primitive.alphaMode = material != nullptr ? material->alphaMode : kMaterialAlphaModeOpaque;
    const uint32_t alphaClass = primitiveAlphaClassForMaterial(material);
    primitive.containsAlphaTestedGeometry = alphaClass == kPrimitiveAlphaClassAlphaTested;
    primitive.containsBlendedGeometry = alphaClass == kPrimitiveAlphaClassBlended;
}

struct MeshAsset {
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshPrimitiveAsset> primitives;
    std::vector<float> defaultMorphWeights;
};

[[nodiscard]] inline bool hasActiveMorphTargetWeights(const MeshAsset& mesh, const std::vector<float>& weights) {
    if (weights.empty()) {
        return false;
    }
    for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
        const size_t targetCount = std::min(primitive.morphTargets.size(), weights.size());
        for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex) {
            const MeshPrimitiveAsset::MorphTarget& target = primitive.morphTargets[targetIndex];
            if (std::abs(weights[targetIndex]) > 1.0e-6f &&
                (!target.positionDeltas.empty() || !target.normalDeltas.empty() || !target.tangentDeltas.empty())) {
                return true;
            }
        }
    }
    return false;
}

inline void applyMorphTargetWeights(MeshAsset& mesh, const std::vector<float>& weights) {
    if (!hasActiveMorphTargetWeights(mesh, weights)) {
        return;
    }

    std::vector<uint8_t> touchedNormals(mesh.vertices.size(), 0u);
    std::vector<uint8_t> touchedTangents(mesh.vertices.size(), 0u);
    for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
        const size_t targetCount = std::min(primitive.morphTargets.size(), weights.size());
        for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex) {
            const float weight = weights[targetIndex];
            if (std::abs(weight) <= 1.0e-6f) {
                continue;
            }
            const MeshPrimitiveAsset::MorphTarget& target = primitive.morphTargets[targetIndex];
            const size_t vertexCount = static_cast<size_t>(primitive.vertexCount);
            const size_t positionCount = std::min(vertexCount, target.positionDeltas.size());
            for (size_t i = 0; i < positionCount; ++i) {
                const size_t vertexIndex = static_cast<size_t>(primitive.firstVertex) + i;
                if (vertexIndex < mesh.vertices.size()) {
                    mesh.vertices[vertexIndex].position += target.positionDeltas[i] * weight;
                }
            }
            const size_t normalCount = std::min(vertexCount, target.normalDeltas.size());
            for (size_t i = 0; i < normalCount; ++i) {
                const size_t vertexIndex = static_cast<size_t>(primitive.firstVertex) + i;
                if (vertexIndex < mesh.vertices.size()) {
                    mesh.vertices[vertexIndex].normal += target.normalDeltas[i] * weight;
                    touchedNormals[vertexIndex] = 1u;
                }
            }
            const size_t tangentCount = std::min(vertexCount, target.tangentDeltas.size());
            for (size_t i = 0; i < tangentCount; ++i) {
                const size_t vertexIndex = static_cast<size_t>(primitive.firstVertex) + i;
                if (vertexIndex < mesh.vertices.size()) {
                    mesh.vertices[vertexIndex].tangent.x += target.tangentDeltas[i].x * weight;
                    mesh.vertices[vertexIndex].tangent.y += target.tangentDeltas[i].y * weight;
                    mesh.vertices[vertexIndex].tangent.z += target.tangentDeltas[i].z * weight;
                    touchedTangents[vertexIndex] = 1u;
                }
            }
        }
    }

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        MeshVertex& vertex = mesh.vertices[i];
        if (touchedNormals[i] != 0u) {
            const float len2 = glm::dot(vertex.normal, vertex.normal);
            vertex.normal = len2 > 1.0e-10f ? vertex.normal / std::sqrt(len2) : glm::vec3{0.0f, 1.0f, 0.0f};
        }
        if (touchedTangents[i] != 0u) {
            glm::vec3 tangent = glm::vec3(vertex.tangent);
            tangent -= vertex.normal * glm::dot(vertex.normal, tangent);
            const float len2 = glm::dot(tangent, tangent);
            tangent = len2 > 1.0e-10f ? tangent / std::sqrt(len2) : glm::vec3{1.0f, 0.0f, 0.0f};
            vertex.tangent = glm::vec4{tangent, vertex.tangent.w < 0.0f ? -1.0f : 1.0f};
        }
    }
}

struct SceneNodeAsset {
    std::string name;
    glm::mat4 transform{1.0f};
    MeshAssetHandle mesh{};
    std::vector<float> morphWeights;
    int32_t skinIndex = -1;
    bool visible = true;
    bool castShadow = true;
    bool visibleToCamera = true;
    bool hasCamera = false;
    uint32_t cameraProjection = 0;
    float cameraYfov = 60.0f * 0.017453292519943295f;
    float cameraAspectRatio = 0.0f;
    float cameraOrthoXmag = 1.0f;
    float cameraOrthoYmag = 1.0f;
    float cameraNear = 0.01f;
    float cameraFar = 1000.0f;
    int32_t parent = -1;
    std::vector<uint32_t> children;
};

struct SceneSkinAsset {
    std::string name;
    int32_t skeletonRoot = -1;
    std::vector<uint32_t> joints;
    std::vector<glm::mat4> inverseBindMatrices;
};

struct SceneLightAsset {
    uint32_t type = 1;
    glm::mat4 transform{1.0f};
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float sizeOrRadius = 1.0f;
    float innerConeRadians = 0.35f;
    float outerConeRadians = 0.70f;
    bool enabled = true;
    int32_t nodeIndex = -1;
};

struct SceneAsset {
    std::string name;
    std::filesystem::path sourcePath;
    std::vector<TextureAssetHandle> textures;
    std::vector<MaterialAssetHandle> materials;
    std::vector<MeshAssetHandle> meshes;
    std::vector<std::string> materialVariants;
    std::vector<SceneNodeAsset> nodes;
    std::vector<SceneSkinAsset> skins;
    std::vector<SceneLightAsset> lights;
    std::vector<uint32_t> rootNodes;
};

} // namespace rtv
