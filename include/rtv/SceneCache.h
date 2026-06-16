#pragma once

#include "rtv/MeshAsset.h"
#include "rtv/TextureAsset.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rtv {

struct CachedTextureData {
    std::string name;
    std::string sourcePath;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 4;
    uint32_t sourceArrayLayers = 1;
    uint32_t sourceDepth = 1;
    uint32_t sourceFaceCount = 1;
    bool sourceIsCubemap = false;
    int mipLevels = 1;
    bool srgb = false;
    bool fallback = false;
    bool isCompressed = false;
    bool linearColorSpace = false;
    uint32_t format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t compressedFormat = 0;
    std::vector<uint8_t> rgba8;
    std::vector<TextureMipLevel> mipData;
    uint32_t minFilter = 1;
    uint32_t magFilter = 1;
    uint32_t wrapS = 0;
    uint32_t wrapT = 0;
};

struct CachedMaterialData {
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
    int32_t nestedPriority = 0;
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
    int32_t baseColorTextureIndex = -1;
    int32_t normalTextureIndex = -1;
    int32_t metallicRoughnessTextureIndex = -1;
    int32_t emissiveTextureIndex = -1;
    int32_t clearcoatTextureIndex = -1;
    int32_t clearcoatRoughnessTextureIndex = -1;
    int32_t clearcoatNormalTextureIndex = -1;
    int32_t transmissionTextureIndex = -1;
    int32_t volumeThicknessTextureIndex = -1;
    int32_t specularTextureIndex = -1;
    int32_t specularColorTextureIndex = -1;
    int32_t sheenColorTextureIndex = -1;
    int32_t sheenRoughnessTextureIndex = -1;
    int32_t iridescenceTextureIndex = -1;
    int32_t iridescenceThicknessTextureIndex = -1;
    int32_t anisotropyTextureIndex = -1;
    int32_t occlusionTextureIndex = -1;
    int32_t opacityTextureIndex = -1;
    int32_t heightTextureIndex = -1;
    float heightScale = 0.025f;
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
    uint32_t materialWorkflow = kMaterialWorkflowMetallicRoughness;
    uint32_t normalMapConvention = kMaterialNormalMapOpenGL;
    uint32_t specularTextureAlphaMode = kMaterialSpecularTextureAlphaNone;
    uint32_t shaderCompatibilityMask = 1u;
};

struct CachedPrimitiveData {
    struct MaterialVariant {
        uint32_t variantIndex = UINT32_MAX;
        std::string variantName;
        int32_t materialIndex = -1;
    };
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t materialIndex = -1;
    std::vector<MeshPrimitiveAsset::MorphTarget> morphTargets;
    std::vector<MaterialVariant> materialVariants;
};

struct CachedMeshData {
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<CachedPrimitiveData> primitives;
    std::vector<float> defaultMorphWeights;
};

struct CachedNodeData {
    std::string name;
    glm::mat4 transform{1.0f};
    int32_t meshIndex = -1;
    std::vector<float> morphWeights;
    int32_t skinIndex = -1;
    uint32_t hasCamera = 0;
    uint32_t cameraProjection = 0;
    float cameraYfov = 60.0f * 0.017453292519943295f;
    float cameraAspectRatio = 0.0f;
    float cameraOrthoXmag = 1.0f;
    float cameraOrthoYmag = 1.0f;
    float cameraNear = 0.01f;
    float cameraFar = 1000.0f;
    int32_t parentIndex = -1;
    std::vector<uint32_t> children;
};

struct CachedSkinData {
    std::string name;
    int32_t skeletonRoot = -1;
    std::vector<uint32_t> joints;
    std::vector<glm::mat4> inverseBindMatrices;
};

struct CachedSceneLightData {
    uint32_t type = 1;
    glm::mat4 transform{1.0f};
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float sizeOrRadius = 1.0f;
    float innerConeRadians = 0.35f;
    float outerConeRadians = 0.70f;
    uint32_t enabled = 1;
    int32_t nodeIndex = -1;
};

struct CachedBvhData {
    std::vector<glm::vec4> packedNodes;
    std::vector<glm::vec4> triangleData;
    uint32_t triangleCount = 0;
    uint32_t leafTriangleCount = 0;
};

struct CachedMeshGpuRecord {
    glm::uvec4 vertexIndexData{};
    glm::uvec4 primitiveData{};
    glm::uvec4 bvhData{};
    glm::uvec4 flags{};
    CachedBvhData localBvh;
};

struct CachedPrimitiveRecord {
    glm::uvec4 indexData{};
    glm::uvec4 metadata{};
};

struct CachedInstanceRecord {
    glm::mat4 transform{1.0f};
    glm::mat4 inverseTransform{1.0f};
    glm::uvec4 metadata{};
};

struct CachedInstanceBoundsRecord {
    glm::vec4 bmin{};
    glm::vec4 bmax{};
};

struct CachedLightRecord {
    glm::uvec4 metadata{};
    glm::vec4 data0{};
    glm::vec4 data1{};
    glm::vec4 data2{};
    glm::vec4 data3{};
};

struct CachedMeshParams {
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t bvhNodeCount = 0;
    uint32_t materialCount = 0;
    uint32_t enabled = 0;
    uint32_t sphereCount = 0;
    uint32_t primitiveCount = 0;
    uint32_t instanceCount = 0;
    uint32_t lightCount = 0;
    float emissiveTotalArea = 0.0f;
    uint32_t meshCount = 0;
    uint32_t localVertexCount = 0;
    uint32_t localIndexCount = 0;
    uint32_t localBvhNodeCount = 0;
    uint32_t localTriangleCount = 0;
    uint32_t tlasNodeCount = 0;
    uint32_t tlasInstanceIndexCount = 0;
};

struct FileDependency {
    std::string path;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

struct CachedScene {
    std::string name;
    std::vector<CachedTextureData> textures;
    std::vector<CachedMaterialData> materials;
    std::vector<CachedMeshData> meshes;
    std::vector<std::string> materialVariants;
    std::vector<CachedNodeData> nodes;
    std::vector<CachedSkinData> skins;
    std::vector<CachedSceneLightData> sceneLights;
    std::vector<uint32_t> rootNodes;
    uint64_t sourceMtime = 0;
    uint64_t sourceBinMtime = 0;

    std::vector<CachedMeshGpuRecord> meshGpuRecords;
    std::vector<CachedPrimitiveRecord> primitiveRecords;
    std::vector<CachedInstanceRecord> instanceRecords;
    std::vector<CachedInstanceBoundsRecord> instanceBounds;
    std::vector<glm::vec4> tlasNodes;
    std::vector<uint32_t> tlasInstanceIndices;
    std::vector<CachedLightRecord> lightRecords;
    CachedMeshParams meshParams;

    std::vector<FileDependency> dependencies;
};

class SceneCache {
public:
    [[nodiscard]] static bool save(const std::filesystem::path& cachePath, const CachedScene& scene);
    [[nodiscard]] static std::optional<CachedScene> load(const std::filesystem::path& cachePath);
    [[nodiscard]] static std::filesystem::path cachePathFor(const std::filesystem::path& gltfPath);
    [[nodiscard]] static bool isCacheValid(const std::filesystem::path& gltfPath, const std::filesystem::path& cachePath);
    [[nodiscard]] static uint64_t fileMtime(const std::filesystem::path& path);

private:
    [[nodiscard]] static uint64_t hashPath(const std::filesystem::path& path);
    [[nodiscard]] static bool readString(std::FILE* file, std::string& out);
    static void writeString(std::FILE* file, const std::string& str);
};

} // namespace rtv
