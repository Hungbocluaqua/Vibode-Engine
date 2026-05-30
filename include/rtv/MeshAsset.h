#pragma once

#include "rtv/TextureAsset.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
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

struct MeshPrimitiveAsset {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    MaterialAssetHandle material{};
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
};

struct SceneNodeAsset {
    std::string name;
    glm::mat4 transform{1.0f};
    MeshAssetHandle mesh{};
    bool visible = true;
    bool castShadow = true;
    bool visibleToCamera = true;
    bool hasCamera = false;
    float cameraYfov = 60.0f * 0.017453292519943295f;
    float cameraNear = 0.01f;
    float cameraFar = 1000.0f;
    int32_t parent = -1;
    std::vector<uint32_t> children;
};

struct SceneLightAsset {
    uint32_t type = 1;
    glm::mat4 transform{1.0f};
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float sizeOrRadius = 1.0f;
    bool enabled = true;
    int32_t nodeIndex = -1;
};

struct SceneAsset {
    std::string name;
    std::filesystem::path sourcePath;
    std::vector<TextureAssetHandle> textures;
    std::vector<MaterialAssetHandle> materials;
    std::vector<MeshAssetHandle> meshes;
    std::vector<SceneNodeAsset> nodes;
    std::vector<SceneLightAsset> lights;
    std::vector<uint32_t> rootNodes;
};

} // namespace rtv
