#include "rtv/GpuScene.h"

#include "rtv/BatchUploader.h"
#include "rtv/BvhBuilder.h"
#include "rtv/ParallelBvhBuilder.h"
#include "rtv/AssetManager.h"
#include "rtv/BufferUploader.h"
#include "rtv/Check.h"
#include "rtv/EnvironmentImportanceSampler.h"
#include "rtv/LightBvh.h"
#include "rtv/HdrLoader.h"
#include "rtv/ResourceAllocator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <glm/gtc/matrix_inverse.hpp>

namespace rtv {

namespace {

constexpr uint32_t instanceFlagVisible = 1u << 0u;
constexpr uint32_t instanceFlagVisibleToCamera = 1u << 1u;
constexpr uint32_t instanceFlagCastShadow = 1u << 2u;

uint32_t nodeInstanceFlags(const SceneNodeAsset& node) {
    uint32_t flags = 0u;
    if (node.visible) {
        flags |= instanceFlagVisible;
    }
    if (node.visibleToCamera) {
        flags |= instanceFlagVisibleToCamera;
    }
    if (node.castShadow) {
        flags |= instanceFlagCastShadow;
    }
    return flags;
}

constexpr uint32_t maxMaterialTextures = 1024;
constexpr uint32_t sceneLightTypeDirectional = 0u;
constexpr uint32_t sceneLightTypePoint = 1u;
constexpr uint32_t sceneLightTypeArea = 2u;
constexpr uint32_t sceneLightTypeSpot = 3u;
constexpr uint32_t gpuLightTypeEmissiveTriangle = 0u;
constexpr uint32_t gpuLightTypeEmissiveSphere = 1u;
constexpr uint32_t gpuLightTypeDirectional = 2u;
constexpr uint32_t gpuLightTypePoint = 3u;
constexpr uint32_t gpuLightTypeArea = 4u;
constexpr uint32_t gpuLightTypeSpot = 5u;
constexpr uint64_t fastImportedBvhTriangleThreshold = 1'000'000ull;
constexpr uint32_t materialFlagManualBaseColorSrgb = 1u << 0u;
constexpr uint32_t materialFlagManualEmissiveSrgb = 1u << 1u;
constexpr uint32_t materialFlagNormalMapDirectX = 1u << 2u;
constexpr uint32_t materialFlagSpecularGlossinessWorkflow = 1u << 3u;
constexpr uint32_t materialFlagSpecularAlphaGlossiness = 1u << 4u;
constexpr uint32_t materialParameterVec4Stride = 18u;
constexpr uint32_t materialTextureTransformCount = 17u;
constexpr uint32_t materialVec4Stride = materialParameterVec4Stride + materialTextureTransformCount * 2u;
constexpr uint32_t materialTypeImportedPbr = 3u;
constexpr uint32_t materialTypeDielectric = 2u;
constexpr uint32_t materialTypeUnlit = 5u;
constexpr float materialTransmissionDielectricThreshold = 0.001f;
constexpr float legacyGlassTransmissionFactor = 0.92f;
constexpr float legacyGlassTintLuminanceFloor = 0.02f;
constexpr float emissiveTextureAverageFallback = 1.0f;

static_assert(sizeof(GpuMeshRecord) == 64);
static_assert(sizeof(GpuPrimitiveRecord) == 32);
static_assert(sizeof(GpuInstanceRecord) == 272);
static_assert(sizeof(GpuLocalVertex) == 80);
static_assert(sizeof(GpuInstanceBoundsRecord) == 32);
static_assert(sizeof(GpuLightRecord) == 96);
static_assert(sizeof(MeshParamsUniform) == 80);

template <typename Fn>
void parallelFor(size_t count, const Fn& fn, size_t minItemsPerTask = 64) {
    if (count == 0) {
        return;
    }
    const size_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const size_t taskCount = std::min(hardwareThreads, std::max<size_t>(1, (count + minItemsPerTask - 1) / minItemsPerTask));
    if (taskCount <= 1) {
        fn(0, count);
        return;
    }

    std::vector<std::future<void>> tasks;
    tasks.reserve(taskCount - 1);
    const size_t blockSize = (count + taskCount - 1) / taskCount;
    size_t begin = 0;
    for (size_t task = 0; task + 1 < taskCount; ++task) {
        const size_t end = std::min(count, begin + blockSize);
        tasks.push_back(std::async(std::launch::async, [begin, end, &fn]() {
            fn(begin, end);
        }));
        begin = end;
    }
    fn(begin, count);
    for (auto& task : tasks) {
        task.get();
    }
}

struct MaterialTextureUploadPayload {
    std::vector<uint8_t> bytes;
    std::vector<TextureMipLevel> mipData;
    uint32_t width = 1;
    uint32_t height = 1;
    bool capped = false;
};

bool validMipRange(const TextureAsset& texture, const TextureMipLevel& mip) {
    return mip.size != 0 &&
        mip.offset <= texture.rgba8.size() &&
        mip.size <= texture.rgba8.size() - mip.offset;
}

uint32_t previewBaseMipFor(const TextureAsset& texture, uint32_t maxDimension) {
    if (maxDimension == 0 || texture.mipData.empty()) {
        return 0;
    }
    for (uint32_t mip = 0; mip < static_cast<uint32_t>(texture.mipData.size()); ++mip) {
        const TextureMipLevel& level = texture.mipData[mip];
        if (level.size == 0) {
            continue;
        }
        const uint32_t width = std::max(level.width, 1u);
        const uint32_t height = std::max(level.height, 1u);
        if (std::max(width, height) <= maxDimension) {
            return mip;
        }
    }
    return static_cast<uint32_t>(texture.mipData.size() - 1u);
}

MaterialTextureUploadPayload makeMaterialTextureUploadPayload(const TextureAsset& texture, uint32_t maxDimension) {
    MaterialTextureUploadPayload payload;
    payload.width = std::max(texture.width, 1u);
    payload.height = std::max(texture.height, 1u);
    if (texture.rgba8.empty()) {
        return payload;
    }

    const uint32_t firstMip = previewBaseMipFor(texture, maxDimension);
    if (firstMip == 0 || texture.mipData.empty()) {
        payload.bytes = texture.rgba8;
        payload.mipData = texture.mipData;
        return payload;
    }

    for (uint32_t mip = firstMip; mip < static_cast<uint32_t>(texture.mipData.size()); ++mip) {
        const TextureMipLevel& sourceMip = texture.mipData[mip];
        if (!validMipRange(texture, sourceMip)) {
            continue;
        }
        const uint64_t destinationOffset = static_cast<uint64_t>(payload.bytes.size());
        const auto begin = texture.rgba8.begin() + static_cast<std::ptrdiff_t>(sourceMip.offset);
        const auto end = begin + static_cast<std::ptrdiff_t>(sourceMip.size);
        payload.bytes.insert(payload.bytes.end(), begin, end);
        payload.mipData.push_back(TextureMipLevel{
            .offset = destinationOffset,
            .size = sourceMip.size,
            .width = std::max(sourceMip.width, 1u),
            .height = std::max(sourceMip.height, 1u),
        });
    }

    if (payload.bytes.empty() || payload.mipData.empty()) {
        payload.bytes = texture.rgba8;
        payload.mipData = texture.mipData;
        return payload;
    }

    payload.width = payload.mipData.front().width;
    payload.height = payload.mipData.front().height;
    payload.capped = true;
    return payload;
}

bool materialNameImpliesLegacyGlass(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower.find("glass") != std::string::npos ||
        lower.find("transparent") != std::string::npos ||
        lower.find("translucent") != std::string::npos;
}

bool materialPathImpliesLegacyGlass(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    return materialNameImpliesLegacyGlass(path.stem().string()) ||
        materialNameImpliesLegacyGlass(path.filename().string()) ||
        materialNameImpliesLegacyGlass(path.generic_string());
}

float rgbLuminance(glm::vec3 value) {
    return value.x * 0.2126f + value.y * 0.7152f + value.z * 0.0722f;
}

bool materialHasExplicitTransmission(const MaterialAsset& material) {
    return material.hasTransmission != 0u ||
        material.transmissionFactor > materialTransmissionDielectricThreshold ||
        material.transmissionTexture.valid();
}

bool materialHasExplicitTransmission(const CachedMaterialData& material) {
    return material.hasTransmission != 0u ||
        material.transmissionFactor > materialTransmissionDielectricThreshold ||
        material.transmissionTextureIndex >= 0;
}

bool materialUsesLegacyGlassTransmission(const MaterialAsset* material) {
    const bool identityImpliesGlass = material != nullptr &&
        (materialNameImpliesLegacyGlass(material->name) ||
         materialPathImpliesLegacyGlass(material->nativePath) ||
         materialNameImpliesLegacyGlass(material->nativeSource));
    return material != nullptr &&
        !materialHasExplicitTransmission(*material) &&
        material->metallicFactor <= 0.05f &&
        identityImpliesGlass;
}

bool materialUsesLegacyGlassTransmission(const CachedMaterialData& material) {
    return !materialHasExplicitTransmission(material) &&
        material.metallicFactor <= 0.05f &&
        materialNameImpliesLegacyGlass(material.name);
}

float effectiveMaterialTransmissionFactor(const MaterialAsset* material) {
    if (material == nullptr) {
        return 0.0f;
    }
    if (materialUsesLegacyGlassTransmission(material)) {
        return legacyGlassTransmissionFactor;
    }
    return material->transmissionFactor;
}

float effectiveMaterialTransmissionFactor(const CachedMaterialData& material) {
    if (materialUsesLegacyGlassTransmission(material)) {
        return legacyGlassTransmissionFactor;
    }
    return material.transmissionFactor;
}

glm::vec4 effectiveMaterialBaseColor(const MaterialAsset* material, glm::vec4 fallback) {
    if (material == nullptr) {
        return fallback;
    }
    glm::vec4 base = material->baseColorFactor;
    if (materialUsesLegacyGlassTransmission(material) &&
        !material->baseColorTexture.valid() &&
        rgbLuminance(glm::vec3(base)) < legacyGlassTintLuminanceFloor) {
        base = glm::vec4(glm::vec3(1.0f), base.a);
    }
    return base;
}

glm::vec4 effectiveMaterialBaseColor(const CachedMaterialData& material) {
    glm::vec4 base = material.baseColorFactor;
    if (materialUsesLegacyGlassTransmission(material) &&
        material.baseColorTextureIndex < 0 &&
        rgbLuminance(glm::vec3(base)) < legacyGlassTintLuminanceFloor) {
        base = glm::vec4(glm::vec3(1.0f), base.a);
    }
    return base;
}

uint32_t importedMaterialType(const MaterialAsset* material) {
    if (material != nullptr && (material->shaderCompatibilityMask & kMaterialClosureFlagUnlit) != 0u) {
        return materialTypeUnlit;
    }
    const float transmissionFactor = effectiveMaterialTransmissionFactor(material);
    if (material != nullptr &&
        transmissionFactor > materialTransmissionDielectricThreshold &&
        material->metallicFactor <= 0.05f) {
        return materialTypeDielectric;
    }
    return materialTypeImportedPbr;
}

float srgbToLinear(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 averageTextureRgb(const TextureAsset* texture) {
    if (texture == nullptr || texture->rgba8.empty() || texture->isCompressed) {
        return glm::vec3(emissiveTextureAverageFallback);
    }

    const uint8_t* texelBytes = texture->rgba8.data();
    size_t texelByteCount = texture->rgba8.size();
    uint32_t width = std::max(texture->width, 1u);
    uint32_t height = std::max(texture->height, 1u);
    if (!texture->mipData.empty()) {
        const TextureMipLevel& baseMip = texture->mipData.front();
        if (baseMip.offset > texture->rgba8.size() || baseMip.size > texture->rgba8.size() - baseMip.offset) {
            return glm::vec3(emissiveTextureAverageFallback);
        }
        texelBytes = texture->rgba8.data() + baseMip.offset;
        texelByteCount = static_cast<size_t>(baseMip.size);
        width = std::max(baseMip.width, 1u);
        height = std::max(baseMip.height, 1u);
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixelCount == 0) {
        return glm::vec3(emissiveTextureAverageFallback);
    }

    glm::dvec3 sum{0.0};
    size_t sampledPixels = 0;
    if (texture->format == VK_FORMAT_R32G32B32A32_SFLOAT && texelByteCount >= pixelCount * 4u * sizeof(float)) {
        for (size_t i = 0; i < pixelCount; ++i) {
            float values[4]{};
            std::memcpy(values, texelBytes + i * 4u * sizeof(float), sizeof(values));
            sum += glm::dvec3(
                std::max(values[0], 0.0f),
                std::max(values[1], 0.0f),
                std::max(values[2], 0.0f));
        }
        sampledPixels = pixelCount;
    } else if (texture->format == VK_FORMAT_R16G16B16A16_UNORM &&
               texelByteCount >= pixelCount * 4u * sizeof(uint16_t)) {
        for (size_t i = 0; i < pixelCount; ++i) {
            uint16_t values[4]{};
            std::memcpy(values, texelBytes + i * 4u * sizeof(uint16_t), sizeof(values));
            sum += glm::dvec3(
                static_cast<double>(values[0]) / 65535.0,
                static_cast<double>(values[1]) / 65535.0,
                static_cast<double>(values[2]) / 65535.0);
        }
        sampledPixels = pixelCount;
    } else if (texelByteCount >= pixelCount * 4u) {
        for (size_t i = 0; i < pixelCount; ++i) {
            const float r = static_cast<float>(texelBytes[i * 4u + 0u]) / 255.0f;
            const float g = static_cast<float>(texelBytes[i * 4u + 1u]) / 255.0f;
            const float b = static_cast<float>(texelBytes[i * 4u + 2u]) / 255.0f;
            sum += texture->srgb && !texture->linearColorSpace
                ? glm::dvec3(srgbToLinear(r), srgbToLinear(g), srgbToLinear(b))
                : glm::dvec3(r, g, b);
        }
        sampledPixels = pixelCount;
    }

    if (sampledPixels == 0) {
        return glm::vec3(emissiveTextureAverageFallback);
    }
    return glm::max(glm::vec3(sum / static_cast<double>(sampledPixels)), glm::vec3(0.0f));
}

glm::vec3 materialEmissiveLightEstimate(const MaterialAsset* material, const AssetManager& assets) {
    if (material == nullptr) {
        return glm::vec3(0.0f);
    }
    glm::vec3 emissive = material->emissiveFactor;
    if (material->emissiveTexture.valid()) {
        emissive *= averageTextureRgb(assets.texture(material->emissiveTexture));
    }
    return emissive;
}

uint32_t materialSemanticFlags(const MaterialAsset* material) {
    uint32_t flags = 0;
    if (material != nullptr && material->normalMapConvention == kMaterialNormalMapDirectX) {
        flags |= materialFlagNormalMapDirectX;
    }
    if (material != nullptr && material->materialWorkflow == kMaterialWorkflowSpecularGlossiness) {
        flags |= materialFlagSpecularGlossinessWorkflow;
    }
    if (material != nullptr && material->specularTextureAlphaMode == kMaterialSpecularTextureAlphaGlossiness) {
        flags |= materialFlagSpecularAlphaGlossiness;
    }
    return flags;
}

uint32_t materialSemanticFlags(const CachedMaterialData& material) {
    uint32_t flags = 0;
    if (material.normalMapConvention == kMaterialNormalMapDirectX) {
        flags |= materialFlagNormalMapDirectX;
    }
    if (material.materialWorkflow == kMaterialWorkflowSpecularGlossiness) {
        flags |= materialFlagSpecularGlossinessWorkflow;
    }
    if (material.specularTextureAlphaMode == kMaterialSpecularTextureAlphaGlossiness) {
        flags |= materialFlagSpecularAlphaGlossiness;
    }
    return flags;
}

uint32_t importedMaterialType(const CachedMaterialData& material) {
    if ((material.shaderCompatibilityMask & kMaterialClosureFlagUnlit) != 0u) {
        return materialTypeUnlit;
    }
    const float transmissionFactor = effectiveMaterialTransmissionFactor(material);
    if (transmissionFactor > materialTransmissionDielectricThreshold &&
        material.metallicFactor <= 0.05f) {
        return materialTypeDielectric;
    }
    return materialTypeImportedPbr;
}

bool hasValidGpuCache(const CachedScene& cached, const SceneAsset& scene) {
    if (cached.meshGpuRecords.empty() || cached.meshParams.meshCount == 0) {
        return false;
    }
    if (cached.textures.size() != scene.textures.size() ||
        cached.materials.size() != scene.materials.size()) {
        return false;
    }
    if (cached.meshGpuRecords.size() != cached.meshParams.meshCount) {
        return false;
    }
    if (cached.primitiveRecords.empty() || cached.instanceRecords.empty()) {
        return false;
    }
    if (cached.tlasNodes.empty() || cached.tlasInstanceIndices.empty()) {
        return false;
    }
    if (cached.meshParams.localBvhNodeCount == 0 ||
        cached.meshParams.localTriangleCount == 0) {
        return false;
    }
    for (const auto& rec : cached.meshGpuRecords) {
        if (rec.localBvh.packedNodes.empty() || rec.localBvh.triangleData.empty()) {
            return false;
        }
    }
    return true;
}

bool hasValidGpuGeometryCache(const CachedScene& cached, const SceneAsset& scene) {
    if (cached.meshGpuRecords.empty() || cached.meshParams.meshCount == 0) {
        return false;
    }
    if (cached.textures.size() != scene.textures.size() ||
        cached.materials.size() != scene.materials.size() ||
        cached.meshes.size() != scene.meshes.size()) {
        return false;
    }
    if (cached.meshGpuRecords.size() != cached.meshParams.meshCount ||
        cached.meshGpuRecords.size() != cached.meshes.size()) {
        return false;
    }
    if (cached.primitiveRecords.empty() ||
        cached.meshParams.localBvhNodeCount == 0 ||
        cached.meshParams.localTriangleCount == 0) {
        return false;
    }
    for (size_t i = 0; i < cached.meshGpuRecords.size(); ++i) {
        const CachedMeshGpuRecord& rec = cached.meshGpuRecords[i];
        const CachedMeshData& mesh = cached.meshes[i];
        if (rec.localBvh.packedNodes.empty() ||
            rec.localBvh.triangleData.empty() ||
            mesh.vertices.empty() ||
            mesh.indices.empty()) {
            return false;
        }
    }
    return true;
}

CachedMeshParams toCachedMeshParams(const MeshParamsUniform& params) {
    return CachedMeshParams{
        .vertexCount = params.vertexCount,
        .triangleCount = params.triangleCount,
        .bvhNodeCount = params.bvhNodeCount,
        .materialCount = params.materialCount,
        .enabled = params.enabled,
        .sphereCount = params.sphereCount,
        .primitiveCount = params.primitiveCount,
        .instanceCount = params.instanceCount,
        .lightCount = params.lightCount,
        .emissiveTotalArea = params.emissiveTotalArea,
        .meshCount = params.meshCount,
        .localVertexCount = params.localVertexCount,
        .localIndexCount = params.localIndexCount,
        .localBvhNodeCount = params.localBvhNodeCount,
        .localTriangleCount = params.localTriangleCount,
        .tlasNodeCount = params.tlasNodeCount,
        .tlasInstanceIndexCount = params.tlasInstanceIndexCount,
    };
}

MeshParamsUniform fromCachedMeshParams(const CachedMeshParams& params) {
    return MeshParamsUniform{
        .vertexCount = params.vertexCount,
        .triangleCount = params.triangleCount,
        .bvhNodeCount = params.bvhNodeCount,
        .materialCount = params.materialCount,
        .enabled = params.enabled,
        .sphereCount = params.sphereCount,
        .primitiveCount = params.primitiveCount,
        .instanceCount = params.instanceCount,
        .lightCount = params.lightCount,
        .emissiveTotalArea = params.emissiveTotalArea,
        .meshCount = params.meshCount,
        .localVertexCount = params.localVertexCount,
        .localIndexCount = params.localIndexCount,
        .localBvhNodeCount = params.localBvhNodeCount,
        .localTriangleCount = params.localTriangleCount,
        .tlasNodeCount = params.tlasNodeCount,
        .tlasInstanceIndexCount = params.tlasInstanceIndexCount,
    };
}

uint32_t cachedMaterialTextureFlag(const CachedScene& cached, int32_t textureIndex, uint32_t flag) {
    if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= cached.textures.size()) {
        return 0;
    }
    const CachedTextureData& texture = cached.textures[static_cast<size_t>(textureIndex)];
    if (texture.linearColorSpace) {
        return 0;
    }
    const VkFormat format = texture.isCompressed
        ? static_cast<VkFormat>(texture.compressedFormat)
        : (texture.srgb && texture.format == VK_FORMAT_R8G8B8A8_UNORM
            ? VK_FORMAT_R8G8B8A8_SRGB
            : static_cast<VkFormat>(texture.format));
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return 0;
    default:
        return flag;
    }
}

glm::vec3 nonnegativeRgb(const glm::vec3& value) {
    return glm::vec3{
        std::max(value.x, 0.0f),
        std::max(value.y, 0.0f),
        std::max(value.z, 0.0f),
    };
}

glm::vec3 approximateConductorKFromF0(const glm::vec3& f0) {
    const glm::vec3 clamped{
        std::clamp(f0.x, 0.02f, 0.98f),
        std::clamp(f0.y, 0.02f, 0.98f),
        std::clamp(f0.z, 0.02f, 0.98f),
    };
    return 2.0f * glm::sqrt(clamped / (glm::vec3{1.0f} - clamped));
}

void appendConductorOptics(
    std::vector<glm::vec4>& materialData,
    const MaterialAsset* material,
    float occlusionTexture = -1.0f,
    float sheenColorTexture = -1.0f,
    float sheenRoughnessTexture = -1.0f,
    float iridescenceTexture = -1.0f,
    float iridescenceThicknessTexture = -1.0f) {
    const bool enabled = material != nullptr && material->useConductorOptics != 0u;
    glm::vec3 eta = enabled ? nonnegativeRgb(material->conductorEta) : glm::vec3{0.0f};
    glm::vec3 k = enabled ? nonnegativeRgb(material->conductorK) : glm::vec3{0.0f};
    if (enabled && eta.x + eta.y + eta.z + k.x + k.y + k.z <= 1.0e-4f) {
        eta = glm::vec3{1.0f};
        k = approximateConductorKFromF0(glm::vec3{material->baseColorFactor});
    }
    materialData.push_back({eta, enabled ? 1.0f : 0.0f});
    materialData.push_back({k, 0.0f});
    materialData.push_back({
        material != nullptr ? material->anisotropyStrength : 0.0f,
        material != nullptr ? material->anisotropyRotation : 0.0f,
        occlusionTexture,
        material != nullptr ? material->occlusionStrength : 1.0f});
    materialData.push_back({
        material != nullptr ? material->sheenColorFactor : glm::vec3{0.0f},
        material != nullptr ? material->sheenRoughnessFactor : 0.0f});
    materialData.push_back({
        sheenColorTexture,
        sheenRoughnessTexture,
        material != nullptr ? material->iridescenceFactor : 0.0f,
        material != nullptr ? material->iridescenceIor : 1.3f});
    materialData.push_back({
        material != nullptr ? material->iridescenceThicknessMinimum : 100.0f,
        material != nullptr ? material->iridescenceThicknessMaximum : 400.0f,
        iridescenceTexture,
        iridescenceThicknessTexture});
}

void appendGltfMaterialExtensionData(
    std::vector<glm::vec4>& materialData,
    const MaterialAsset* material,
    float clearcoatTexture = -1.0f,
    float clearcoatRoughnessTexture = -1.0f,
    float clearcoatNormalTexture = -1.0f,
    float transmissionTexture = -1.0f,
    float volumeThicknessTexture = -1.0f,
    float specularTexture = -1.0f,
    float specularColorTexture = -1.0f,
    float anisotropyTexture = -1.0f,
    float transmissionFactor = -1.0f) {
    const float effectiveTransmission = transmissionFactor >= 0.0f
        ? transmissionFactor
        : (material != nullptr ? effectiveMaterialTransmissionFactor(material) : 0.0f);
    materialData.push_back({
        material != nullptr ? material->clearcoatFactor : 0.0f,
        material != nullptr ? material->clearcoatRoughnessFactor : 0.0f,
        clearcoatTexture,
        clearcoatRoughnessTexture});
    materialData.push_back({
        clearcoatNormalTexture,
        effectiveTransmission,
        transmissionTexture,
        material != nullptr ? material->specularFactor : 1.0f});
    materialData.push_back({
        material != nullptr ? material->specularColorFactor : glm::vec3{1.0f},
        specularTexture});
    materialData.push_back({
        specularColorTexture,
        anisotropyTexture,
        material != nullptr ? material->volumeThicknessFactor : 0.0f,
        volumeThicknessTexture});
    materialData.push_back({
        material != nullptr ? material->volumeAttenuationColor : glm::vec3{1.0f},
        material != nullptr ? material->volumeAttenuationDistance : 0.0f});
    materialData.push_back({
        material != nullptr ? material->dispersionFactor : 0.0f,
        0.0f,
        0.0f,
        0.0f});
}

void appendGltfMaterialExtensionData(std::vector<glm::vec4>& materialData, const CachedMaterialData& material) {
    const float effectiveTransmission = effectiveMaterialTransmissionFactor(material);
    materialData.push_back({
        material.clearcoatFactor,
        material.clearcoatRoughnessFactor,
        material.clearcoatTextureIndex >= 0 ? static_cast<float>(material.clearcoatTextureIndex) : -1.0f,
        material.clearcoatRoughnessTextureIndex >= 0 ? static_cast<float>(material.clearcoatRoughnessTextureIndex) : -1.0f});
    materialData.push_back({
        material.clearcoatNormalTextureIndex >= 0 ? static_cast<float>(material.clearcoatNormalTextureIndex) : -1.0f,
        effectiveTransmission,
        material.transmissionTextureIndex >= 0 ? static_cast<float>(material.transmissionTextureIndex) : -1.0f,
        material.specularFactor});
    materialData.push_back({
        material.specularColorFactor,
        material.specularTextureIndex >= 0 ? static_cast<float>(material.specularTextureIndex) : -1.0f});
    materialData.push_back({
        material.specularColorTextureIndex >= 0 ? static_cast<float>(material.specularColorTextureIndex) : -1.0f,
        material.anisotropyTextureIndex >= 0 ? static_cast<float>(material.anisotropyTextureIndex) : -1.0f,
        material.volumeThicknessFactor,
        material.volumeThicknessTextureIndex >= 0 ? static_cast<float>(material.volumeThicknessTextureIndex) : -1.0f});
    materialData.push_back({material.volumeAttenuationColor, material.volumeAttenuationDistance});
    materialData.push_back({material.dispersionFactor, 0.0f, 0.0f, 0.0f});
}

void appendTextureTransformData(std::vector<glm::vec4>& materialData, const TextureTransformAsset& transform) {
    materialData.push_back({transform.offset, transform.scale});
    materialData.push_back({
        transform.rotation,
        transform.enabled != 0u ? 1.0f : 0.0f,
        static_cast<float>(transform.texCoord),
        0.0f});
}

void appendMaterialTextureTransforms(std::vector<glm::vec4>& materialData, const MaterialAsset* material) {
    const TextureTransformAsset identity{};
    const MaterialAsset fallback{};
    const MaterialAsset& m = material != nullptr ? *material : fallback;
    appendTextureTransformData(materialData, material != nullptr ? m.baseColorTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.normalTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.metallicRoughnessTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.emissiveTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.occlusionTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.sheenColorTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.sheenRoughnessTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.iridescenceTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.iridescenceThicknessTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.clearcoatTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.clearcoatRoughnessTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.clearcoatNormalTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.transmissionTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.volumeThicknessTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.specularTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.specularColorTextureTransform : identity);
    appendTextureTransformData(materialData, material != nullptr ? m.anisotropyTextureTransform : identity);
}

void appendMaterialTextureTransforms(std::vector<glm::vec4>& materialData, const CachedMaterialData& material) {
    appendTextureTransformData(materialData, material.baseColorTextureTransform);
    appendTextureTransformData(materialData, material.normalTextureTransform);
    appendTextureTransformData(materialData, material.metallicRoughnessTextureTransform);
    appendTextureTransformData(materialData, material.emissiveTextureTransform);
    appendTextureTransformData(materialData, material.occlusionTextureTransform);
    appendTextureTransformData(materialData, material.sheenColorTextureTransform);
    appendTextureTransformData(materialData, material.sheenRoughnessTextureTransform);
    appendTextureTransformData(materialData, material.iridescenceTextureTransform);
    appendTextureTransformData(materialData, material.iridescenceThicknessTextureTransform);
    appendTextureTransformData(materialData, material.clearcoatTextureTransform);
    appendTextureTransformData(materialData, material.clearcoatRoughnessTextureTransform);
    appendTextureTransformData(materialData, material.clearcoatNormalTextureTransform);
    appendTextureTransformData(materialData, material.transmissionTextureTransform);
    appendTextureTransformData(materialData, material.volumeThicknessTextureTransform);
    appendTextureTransformData(materialData, material.specularTextureTransform);
    appendTextureTransformData(materialData, material.specularColorTextureTransform);
    appendTextureTransformData(materialData, material.anisotropyTextureTransform);
}

void appendOpacityHeightTextureIndices(std::vector<glm::vec4>& materialData, float opacityTexture = -1.0f, float heightTexture = -1.0f, float heightScale = 0.025f) {
    materialData.push_back({opacityTexture, heightTexture, heightScale, 0.0f});
}

void appendConductorOptics(std::vector<glm::vec4>& materialData, const CachedMaterialData& material) {
    const bool enabled = material.useConductorOptics != 0u;
    glm::vec3 eta = enabled ? nonnegativeRgb(material.conductorEta) : glm::vec3{0.0f};
    glm::vec3 k = enabled ? nonnegativeRgb(material.conductorK) : glm::vec3{0.0f};
    if (enabled && eta.x + eta.y + eta.z + k.x + k.y + k.z <= 1.0e-4f) {
        eta = glm::vec3{1.0f};
        k = approximateConductorKFromF0(glm::vec3{material.baseColorFactor});
    }
    materialData.push_back({eta, enabled ? 1.0f : 0.0f});
    materialData.push_back({k, 0.0f});
    materialData.push_back({
        material.anisotropyStrength,
        material.anisotropyRotation,
        material.occlusionTextureIndex >= 0 ? static_cast<float>(material.occlusionTextureIndex) : -1.0f,
        material.occlusionStrength});
    materialData.push_back({material.sheenColorFactor, material.sheenRoughnessFactor});
    materialData.push_back({
        material.sheenColorTextureIndex >= 0 ? static_cast<float>(material.sheenColorTextureIndex) : -1.0f,
        material.sheenRoughnessTextureIndex >= 0 ? static_cast<float>(material.sheenRoughnessTextureIndex) : -1.0f,
        material.iridescenceFactor,
        material.iridescenceIor});
    materialData.push_back({
        material.iridescenceThicknessMinimum,
        material.iridescenceThicknessMaximum,
        material.iridescenceTextureIndex >= 0 ? static_cast<float>(material.iridescenceTextureIndex) : -1.0f,
        material.iridescenceThicknessTextureIndex >= 0 ? static_cast<float>(material.iridescenceThicknessTextureIndex) : -1.0f});
}

std::vector<glm::vec4> buildCachedMaterialData(const CachedScene& cached) {
    std::vector<glm::vec4> materialData;
    materialData.reserve(std::max<size_t>(1, cached.materials.size()) * materialVec4Stride);
    for (const CachedMaterialData& material : cached.materials) {
        const glm::vec4 base = effectiveMaterialBaseColor(material);
        const uint32_t flags =
            cachedMaterialTextureFlag(cached, material.baseColorTextureIndex, materialFlagManualBaseColorSrgb) |
            cachedMaterialTextureFlag(cached, material.emissiveTextureIndex, materialFlagManualEmissiveSrgb) |
            materialSemanticFlags(material);
        const float type = static_cast<float>(importedMaterialType(material));
        materialData.push_back({glm::vec3(base), material.roughnessFactor});
        materialData.push_back({material.iorFactor, type, material.metallicFactor, static_cast<float>(flags)});
        materialData.push_back({material.emissiveFactor, base.a});
        materialData.push_back({
            material.baseColorTextureIndex >= 0 ? static_cast<float>(material.baseColorTextureIndex) : -1.0f,
            material.normalTextureIndex >= 0 ? static_cast<float>(material.normalTextureIndex) : -1.0f,
            material.metallicRoughnessTextureIndex >= 0 ? static_cast<float>(material.metallicRoughnessTextureIndex) : -1.0f,
            material.emissiveTextureIndex >= 0 ? static_cast<float>(material.emissiveTextureIndex) : -1.0f});
        materialData.push_back({
            material.alphaCutoff,
            static_cast<float>(material.alphaMode),
            static_cast<float>(material.doubleSided),
            0.0f});
        appendConductorOptics(materialData, material);
        appendGltfMaterialExtensionData(materialData, material);
        appendOpacityHeightTextureIndices(
            materialData,
            material.opacityTextureIndex >= 0 ? static_cast<float>(material.opacityTextureIndex) : -1.0f,
            material.heightTextureIndex >= 0 ? static_cast<float>(material.heightTextureIndex) : -1.0f,
            material.heightScale);
        appendMaterialTextureTransforms(materialData, material);
    }
    if (materialData.empty()) {
        materialData.push_back({0.8f, 0.8f, 0.8f, 1.0f});
        materialData.push_back({1.5f, 0.0f, 0.0f, 0.0f});
        materialData.push_back({0.0f, 0.0f, 0.0f, 1.0f});
        materialData.push_back({-1.0f, -1.0f, -1.0f, -1.0f});
        materialData.push_back({0.5f, 0.0f, 0.0f, 0.0f});
        appendConductorOptics(materialData, nullptr);
        appendGltfMaterialExtensionData(materialData, nullptr);
        appendOpacityHeightTextureIndices(materialData);
        appendMaterialTextureTransforms(materialData, nullptr);
    }
    return materialData;
}

bool materialDataContainsTransmission(const std::vector<glm::vec4>& materialData) {
    const size_t materialCount = materialData.size() / materialVec4Stride;
    for (size_t i = 0; i < materialCount; ++i) {
        const size_t transmissionSlot = i * materialVec4Stride + 12u;
        if (transmissionSlot < materialData.size() &&
            materialData[transmissionSlot].y > materialTransmissionDielectricThreshold) {
            return true;
        }
    }
    return false;
}

void addFileDependency(CachedScene& cached, const std::filesystem::path& path, std::unordered_set<std::string>& seen) {
    if (path.empty() || !std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
        return;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    const std::string key = normalized.string();
    if (!seen.insert(key).second) {
        return;
    }
    cached.dependencies.push_back(FileDependency{
        .path = key,
        .size = static_cast<uint64_t>(std::filesystem::file_size(normalized)),
        .mtime = SceneCache::fileMtime(normalized),
    });
}

struct TextureColorUsage {
    bool baseColor = false;
    bool emissive = false;
    bool metallicRoughness = false;
    bool normal = false;
    bool occlusion = false;

    [[nodiscard]] bool color() const { return baseColor || emissive; }
    [[nodiscard]] bool data() const { return metallicRoughness || normal || occlusion; }
};

struct MaterialCpu {
    glm::vec3 color{};
    float roughness = 1.0f;
    float ior = 1.0f;
    uint32_t type = 0;
    float metallic = 0.0f;
    glm::vec3 emissive{};
    float transmission = 0.0f;
};

struct TriBuild {
    glm::vec3 v0{};
    glm::vec3 v1{};
    glm::vec3 v2{};
    glm::vec3 normal{};
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    glm::vec3 center{};
    uint32_t material = 0;
};

struct PackedNode {
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    bool leaf = false;
    uint32_t ropePlusOne = 0;
    uint32_t child0 = 0;
    uint32_t child1 = 0;
    uint32_t triOffset = 0;
    uint32_t triCount = 0;
    uint32_t childCount = 0;
};

struct BuildNode {
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    int left = -1;
    int right = -1;
    uint32_t triOffset = 0;
    uint32_t triCount = 0;
};

struct CpuBounds {
    glm::vec3 bmin{std::numeric_limits<float>::max()};
    glm::vec3 bmax{-std::numeric_limits<float>::max()};
};

struct CpuTlasNode {
    CpuBounds bounds{};
    bool leaf = false;
    int rope = -1;
    uint32_t child0 = 0;
    uint32_t child1 = 0;
    uint32_t instanceOffset = 0;
    uint32_t instanceCount = 0;
};

glm::vec3 minVec(glm::vec3 a, glm::vec3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

glm::vec3 maxVec(glm::vec3 a, glm::vec3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

void includePoint(CpuBounds& bounds, glm::vec3 point) {
    bounds.bmin = minVec(bounds.bmin, point);
    bounds.bmax = maxVec(bounds.bmax, point);
}

void includeBounds(CpuBounds& bounds, const CpuBounds& other) {
    includePoint(bounds, other.bmin);
    includePoint(bounds, other.bmax);
}

glm::vec3 boundsCenter(const CpuBounds& bounds) {
    return (bounds.bmin + bounds.bmax) * 0.5f;
}

CpuBounds boundsFromPositions(const std::vector<glm::vec3>& positions) {
    CpuBounds bounds;
    for (glm::vec3 point : positions) {
        includePoint(bounds, point);
    }
    return bounds;
}

CpuBounds transformBounds(const CpuBounds& bounds, const glm::mat4& transform) {
    CpuBounds result;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const glm::vec3 p{
            (corner & 1u) != 0u ? bounds.bmax.x : bounds.bmin.x,
            (corner & 2u) != 0u ? bounds.bmax.y : bounds.bmin.y,
            (corner & 4u) != 0u ? bounds.bmax.z : bounds.bmin.z,
        };
        includePoint(result, glm::vec3(transform * glm::vec4(p, 1.0f)));
    }
    return result;
}

GpuInstanceBoundsRecord makeInstanceBoundsRecord(const CpuBounds& bounds, uint32_t instanceIndex, uint32_t meshIndex) {
    return GpuInstanceBoundsRecord{
        .bmin = {bounds.bmin, static_cast<float>(instanceIndex)},
        .bmax = {bounds.bmax, static_cast<float>(meshIndex)},
    };
}

GpuLocalVertex makeLocalVertex(
    glm::vec3 position,
    glm::vec3 normal,
    glm::vec2 uv = glm::vec2{0.0f},
    glm::vec4 tangent = glm::vec4{1.0f, 0.0f, 0.0f, 1.0f},
    glm::vec4 color = glm::vec4{1.0f},
    glm::vec2 uv1 = glm::vec2{0.0f}) {
    return GpuLocalVertex{
        .positionUvX = {position, uv.x},
        .normalUvY = {normal, uv.y},
        .tangent = tangent,
        .color = color,
        .texcoord1 = {uv1, 0.0f, 0.0f},
    };
}

struct CachedMeshCpuPayload {
    const CachedMeshData* mesh = nullptr;
    std::vector<GpuLocalVertex> vertices;
    CpuBounds bounds;
};

CachedMeshCpuPayload buildCachedMeshCpuPayload(const CachedMeshData& mesh) {
    CachedMeshCpuPayload payload;
    payload.mesh = &mesh;
    std::vector<MeshVertex> vertices = mesh.vertices;
    if (!mesh.defaultMorphWeights.empty()) {
        MeshAsset morphMesh;
        morphMesh.vertices = vertices;
        morphMesh.defaultMorphWeights = mesh.defaultMorphWeights;
        morphMesh.primitives.reserve(mesh.primitives.size());
        for (const CachedPrimitiveData& cachedPrimitive : mesh.primitives) {
            MeshPrimitiveAsset primitive;
            primitive.firstVertex = cachedPrimitive.firstVertex;
            primitive.vertexCount = cachedPrimitive.vertexCount;
            primitive.morphTargets = cachedPrimitive.morphTargets;
            morphMesh.primitives.push_back(std::move(primitive));
        }
        applyMorphTargetWeights(morphMesh, mesh.defaultMorphWeights);
        vertices = std::move(morphMesh.vertices);
    }

    payload.vertices.reserve(vertices.size());
    for (const MeshVertex& vertex : vertices) {
        const float normalLen2 = glm::dot(vertex.normal, vertex.normal);
        const glm::vec3 normal = normalLen2 > 1.0e-10f ? glm::normalize(vertex.normal) : glm::vec3{0.0f, 1.0f, 0.0f};
        glm::vec3 tangent = glm::vec3(vertex.tangent);
        const float tangentLen2 = glm::dot(tangent, tangent);
        tangent = tangentLen2 > 1.0e-10f ? glm::normalize(tangent) : glm::vec3{1.0f, 0.0f, 0.0f};
        payload.vertices.push_back(makeLocalVertex(
            vertex.position,
            normal,
            vertex.texcoord,
            glm::vec4{tangent, vertex.tangent.w < 0.0f ? -1.0f : 1.0f},
            vertex.color,
            vertex.texcoord1));
        includePoint(payload.bounds, vertex.position);
    }
    return payload;
}

std::vector<CachedMeshCpuPayload> buildCachedMeshCpuPayloads(const std::vector<CachedMeshData>& meshes) {
    std::vector<CachedMeshCpuPayload> payloads(meshes.size());
    parallelFor(meshes.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            payloads[i] = buildCachedMeshCpuPayload(meshes[i]);
        }
    }, 1);
    return payloads;
}

void appendCachedMeshCpuPayloads(
    const std::vector<CachedMeshCpuPayload>& payloads,
    std::vector<GpuLocalVertex>& localVertexData,
    std::vector<uint32_t>& localIndices,
    std::vector<CpuBounds>* localMeshBounds = nullptr) {
    size_t vertexCount = 0;
    size_t indexCount = 0;
    for (const CachedMeshCpuPayload& payload : payloads) {
        vertexCount += payload.vertices.size();
        indexCount += payload.mesh != nullptr ? payload.mesh->indices.size() : 0;
    }

    localVertexData.reserve(localVertexData.size() + vertexCount);
    localIndices.reserve(localIndices.size() + indexCount);
    if (localMeshBounds != nullptr) {
        localMeshBounds->reserve(localMeshBounds->size() + payloads.size());
    }

    for (const CachedMeshCpuPayload& payload : payloads) {
        const uint32_t vertexBase = static_cast<uint32_t>(localVertexData.size());
        localVertexData.insert(localVertexData.end(), payload.vertices.begin(), payload.vertices.end());
        if (payload.mesh != nullptr) {
            for (uint32_t index : payload.mesh->indices) {
                localIndices.push_back(vertexBase + index);
            }
        }
        if (localMeshBounds != nullptr) {
            localMeshBounds->push_back(payload.bounds);
        }
    }
}

void threadTlasRopes(std::vector<CpuTlasNode>& nodes, uint32_t nodeIndex, int rope) {
    if (nodeIndex >= nodes.size()) {
        return;
    }
    CpuTlasNode& node = nodes[nodeIndex];
    node.rope = rope;
    if (!node.leaf) {
        threadTlasRopes(nodes, node.child0, static_cast<int>(node.child1));
        threadTlasRopes(nodes, node.child1, rope);
    }
}

uint32_t buildTlasRecursive(
    const std::vector<GpuInstanceBoundsRecord>& instanceBounds,
    std::vector<uint32_t>& refs,
    uint32_t begin,
    uint32_t end,
    std::vector<CpuTlasNode>& nodes,
    std::vector<uint32_t>& orderedInstances) {
    CpuBounds nodeBounds;
    CpuBounds centroidBounds;
    for (uint32_t i = begin; i < end; ++i) {
        const GpuInstanceBoundsRecord& record = instanceBounds[refs[i]];
        CpuBounds bounds{glm::vec3(record.bmin), glm::vec3(record.bmax)};
        includeBounds(nodeBounds, bounds);
        includePoint(centroidBounds, boundsCenter(bounds));
    }

    const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
    nodes.push_back(CpuTlasNode{.bounds = nodeBounds});
    const uint32_t count = end - begin;
    if (count <= 4u) {
        CpuTlasNode& node = nodes[nodeIndex];
        node.leaf = true;
        node.instanceOffset = static_cast<uint32_t>(orderedInstances.size());
        node.instanceCount = count;
        orderedInstances.insert(orderedInstances.end(), refs.begin() + begin, refs.begin() + end);
        return nodeIndex;
    }

    const glm::vec3 extent = centroidBounds.bmax - centroidBounds.bmin;
    uint32_t axis = 0;
    if (extent.y > extent.x && extent.y >= extent.z) {
        axis = 1;
    } else if (extent.z > extent.x && extent.z > extent.y) {
        axis = 2;
    }
    const uint32_t mid = begin + count / 2u;
    std::nth_element(refs.begin() + begin, refs.begin() + mid, refs.begin() + end, [&](uint32_t a, uint32_t b) {
        const CpuBounds boundsA{glm::vec3(instanceBounds[a].bmin), glm::vec3(instanceBounds[a].bmax)};
        const CpuBounds boundsB{glm::vec3(instanceBounds[b].bmin), glm::vec3(instanceBounds[b].bmax)};
        return boundsCenter(boundsA)[axis] < boundsCenter(boundsB)[axis];
    });

    const uint32_t child0 = buildTlasRecursive(instanceBounds, refs, begin, mid, nodes, orderedInstances);
    const uint32_t child1 = buildTlasRecursive(instanceBounds, refs, mid, end, nodes, orderedInstances);
    nodes[nodeIndex].child0 = child0;
    nodes[nodeIndex].child1 = child1;
    return nodeIndex;
}

void buildTlas(
    const std::vector<GpuInstanceBoundsRecord>& instanceBounds,
    std::vector<glm::vec4>& packedNodes,
    std::vector<uint32_t>& orderedInstances) {
    packedNodes.clear();
    orderedInstances.clear();
    if (instanceBounds.empty()) {
        return;
    }

    std::vector<uint32_t> refs(instanceBounds.size());
    std::iota(refs.begin(), refs.end(), 0u);
    std::vector<CpuTlasNode> nodes;
    nodes.reserve(instanceBounds.size() * 2u);
    const uint32_t root = buildTlasRecursive(instanceBounds, refs, 0u, static_cast<uint32_t>(refs.size()), nodes, orderedInstances);
    threadTlasRopes(nodes, root, -1);

    packedNodes.reserve(nodes.size() * 4u);
    for (const CpuTlasNode& node : nodes) {
        packedNodes.push_back({node.bounds.bmin, node.leaf ? 1.0f : 0.0f});
        packedNodes.push_back({node.bounds.bmax, node.rope >= 0 ? static_cast<float>(node.rope + 1) : 0.0f});
        if (node.leaf) {
            packedNodes.push_back({static_cast<float>(node.instanceOffset), static_cast<float>(node.instanceCount), 0.0f, 0.0f});
        } else {
            packedNodes.push_back({static_cast<float>(node.child0), static_cast<float>(node.child1), 0.0f, 0.0f});
        }
        packedNodes.push_back({node.leaf ? 0.0f : 2.0f, 0.0f, 0.0f, 0.0f});
    }
}

std::vector<TextureColorUsage> classifyTextureUsage(const SceneAsset& scene, const AssetManager& assets) {
    std::vector<TextureColorUsage> usage(scene.textures.size());
    auto slotFor = [&](TextureAssetHandle texture) -> uint32_t {
        if (!texture.valid()) {
            return UINT32_MAX;
        }
        for (uint32_t slot = 0; slot < scene.textures.size(); ++slot) {
            if (scene.textures[slot].index == texture.index) {
                return slot;
            }
        }
        return UINT32_MAX;
    };

    for (MaterialAssetHandle handle : scene.materials) {
        const MaterialAsset* material = assets.material(handle);
        if (material == nullptr) {
            continue;
        }
        uint32_t slot = slotFor(material->baseColorTexture);
        if (slot < usage.size()) {
            usage[slot].baseColor = true;
        }
        slot = slotFor(material->specularColorTexture);
        if (slot < usage.size()) {
            usage[slot].baseColor = true;
        }
        slot = slotFor(material->sheenColorTexture);
        if (slot < usage.size()) {
            usage[slot].baseColor = true;
        }
        slot = slotFor(material->iridescenceTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->iridescenceThicknessTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->emissiveTexture);
        if (slot < usage.size()) {
            usage[slot].emissive = true;
        }
        slot = slotFor(material->metallicRoughnessTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->clearcoatTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->clearcoatRoughnessTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->transmissionTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->specularTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->sheenRoughnessTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->anisotropyTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->occlusionTexture);
        if (slot < usage.size()) {
            usage[slot].occlusion = true;
        }
        slot = slotFor(material->normalTexture);
        if (slot < usage.size()) {
            usage[slot].normal = true;
        }
        slot = slotFor(material->clearcoatNormalTexture);
        if (slot < usage.size()) {
            usage[slot].normal = true;
        }
    }
    return usage;
}

bool uploadTextureAsSrgb(const std::vector<TextureColorUsage>& usage, uint32_t slot) {
    return slot < usage.size() && usage[slot].color() && !usage[slot].data();
}

bool isSrgbTextureFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

VkFormat importedMaterialTextureFormat(const TextureAsset* texture, const std::vector<TextureColorUsage>& usage, uint32_t slot) {
    if (texture == nullptr || texture->fallback) {
        return uploadTextureAsSrgb(usage, slot) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    if (texture->isCompressed) {
        return texture->compressedFormat != VK_FORMAT_UNDEFINED ? texture->compressedFormat : texture->format;
    }
    if (texture->format == VK_FORMAT_R8G8B8A8_UNORM && uploadTextureAsSrgb(usage, slot)) {
        return VK_FORMAT_R8G8B8A8_SRGB;
    }
    return texture->format != VK_FORMAT_UNDEFINED ? texture->format : VK_FORMAT_R8G8B8A8_UNORM;
}

VkFormat cachedMaterialTextureFormat(const CachedTextureData* texture) {
    if (texture == nullptr || texture->fallback) {
        return texture != nullptr && texture->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    if (texture->isCompressed) {
        return texture->compressedFormat != 0u ? static_cast<VkFormat>(texture->compressedFormat) : static_cast<VkFormat>(texture->format);
    }
    if (texture->format == VK_FORMAT_R8G8B8A8_UNORM && texture->srgb) {
        return VK_FORMAT_R8G8B8A8_SRGB;
    }
    return texture->format != 0u ? static_cast<VkFormat>(texture->format) : VK_FORMAT_R8G8B8A8_UNORM;
}

bool importedMaterialTextureNeedsManualSrgb(const TextureAsset* texture, const std::vector<TextureColorUsage>& usage, uint32_t slot) {
    if (slot >= usage.size() || !usage[slot].color() || usage[slot].data()) {
        return false;
    }
    if (texture != nullptr && texture->linearColorSpace) {
        return false;
    }
    return !isSrgbTextureFormat(importedMaterialTextureFormat(texture, usage, slot));
}

bool isHighPrecisionTextureFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16B16_UNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return true;
    default:
        return false;
    }
}

uint64_t estimatedTextureBytes(size_t uploadedBytes, uint32_t mipLevels, bool hasExplicitMips) {
    uint64_t bytes = static_cast<uint64_t>(uploadedBytes);
    if (!hasExplicitMips && mipLevels > 1) {
        bytes = (bytes * 4u) / 3u;
    }
    return bytes;
}

void warnHighPrecisionTextureMemory(
    const char* label,
    const std::string& name,
    VkFormat format,
    uint32_t width,
    uint32_t height,
    uint64_t bytes) {
    constexpr uint64_t oneMiB = 1024ull * 1024ull;
    if (!isHighPrecisionTextureFormat(format) || bytes < 64ull * oneMiB) {
        return;
    }
    std::cerr << label << " high-precision texture memory warning: "
              << (name.empty() ? std::string("<unnamed>") : name)
              << " " << width << "x" << height
              << " format=" << static_cast<uint32_t>(format)
              << " estimated=" << (bytes / oneMiB) << " MiB\n";
}

void warnHighPrecisionTextureBudget(const char* label, uint64_t bytes) {
    constexpr uint64_t oneMiB = 1024ull * 1024ull;
    if (bytes < 256ull * oneMiB) {
        return;
    }
    std::cerr << label << " high-precision texture budget warning: estimated "
              << (bytes / oneMiB) << " MiB across resident material textures\n";
}

std::vector<uint8_t> fallbackTexturePixels(const std::vector<TextureColorUsage>& usage, uint32_t slot) {
    if (slot < usage.size() && usage[slot].normal) {
        return {128, 128, 255, 255};
    }
    if (slot < usage.size() && usage[slot].metallicRoughness) {
        return {255, 255, 0, 255};
    }
    if (slot < usage.size() && usage[slot].occlusion) {
        return {255, 255, 255, 255};
    }
    return {255, 255, 255, 255};
}

std::unique_ptr<Buffer> uploadBuffer(ResourceAllocator& allocator, BufferUploader& uploader, std::unique_ptr<Buffer>& buffer, VkBufferUsageFlags usage, const void* data, VkDeviceSize bytes, const char* name) {
    if (allocator.supportsDeviceAddress() && (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const VkDeviceSize requiredSize = std::max<VkDeviceSize>(bytes, 4);
    const VkBufferUsageFlags requiredUsage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (buffer != nullptr && buffer->size() >= requiredSize && (buffer->usage() & requiredUsage) == requiredUsage) {
        if (bytes > 0) {
            uploader.uploadToBuffer(*buffer, data, bytes);
        }
        return {};
    }
    std::unique_ptr<Buffer> replacedBuffer;
    if (buffer != nullptr && buffer->handle() != VK_NULL_HANDLE) {
        replacedBuffer = std::move(buffer);
    }
    buffer = std::make_unique<Buffer>(allocator, BufferDesc{
        .size = requiredSize,
        .usage = requiredUsage,
        .memory = BufferMemory::GpuOnly,
        .debugName = name,
    });
    if (bytes > 0) {
        uploader.uploadToBuffer(*buffer, data, bytes);
    }
    return replacedBuffer;
}

void uploadBufferBatched(BatchUploader& batch, std::unique_ptr<Buffer>& buffer, VkBufferUsageFlags usage, const void* data, VkDeviceSize bytes, const char* name) {
    if (batch.allocator().supportsDeviceAddress() && (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const VkDeviceSize requiredSize = std::max<VkDeviceSize>(bytes, 4);
    const VkBufferUsageFlags requiredUsage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer = std::make_unique<Buffer>(batch.allocator(), BufferDesc{
        .size = requiredSize,
        .usage = requiredUsage,
        .memory = BufferMemory::GpuOnly,
        .debugName = name,
    });
    if (bytes > 0) {
        batch.enqueueBufferUpload(*buffer, data, bytes);
    }
}

template <typename T>
void uploadVectorBatched(BatchUploader& batch, std::unique_ptr<Buffer>& buffer, VkBufferUsageFlags usage, const std::vector<T>& data, const char* name) {
    uploadBufferBatched(batch, buffer, usage, data.data(), sizeof(T) * data.size(), name);
}

template <typename T>
std::unique_ptr<Buffer> uploadVector(ResourceAllocator& allocator, BufferUploader& uploader, std::unique_ptr<Buffer>& buffer, VkBufferUsageFlags usage, const std::vector<T>& data, const char* name) {
    return uploadBuffer(allocator, uploader, buffer, usage, data.data(), sizeof(T) * data.size(), name);
}

VkFilter toVkFilter(TextureFilter filter) {
    return filter == TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode toVkAddressMode(TextureWrap wrap) {
    switch (wrap) {
    case TextureWrap::ClampToEdge:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::MirroredRepeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case TextureWrap::Repeat:
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

bool sameSampler(const TextureSamplerDesc& a, const TextureSamplerDesc& b) {
    return a.minFilter == b.minFilter &&
        a.magFilter == b.magFilter &&
        a.wrapS == b.wrapS &&
        a.wrapT == b.wrapT;
}

VkSampler createMaterialSampler(ResourceAllocator& allocator, const TextureSamplerDesc& desc, float requestedAnisotropy) {
    VkDevice device = allocator.device();
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = toVkFilter(desc.magFilter);
    samplerInfo.minFilter = toVkFilter(desc.minFilter);
    samplerInfo.addressModeU = toVkAddressMode(desc.wrapS);
    samplerInfo.addressModeV = toVkAddressMode(desc.wrapT);
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipmapMode = desc.minFilter == TextureFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (allocator.supportsSamplerAnisotropy() &&
        requestedAnisotropy > 1.0f &&
        (desc.minFilter != TextureFilter::Nearest || desc.magFilter != TextureFilter::Nearest)) {
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = std::clamp(requestedAnisotropy, 1.0f, allocator.maxSamplerAnisotropy());
    }
    VkSampler sampler = VK_NULL_HANDLE;
    checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "vkCreateSampler(material textures)");
    return sampler;
}

TextureSamplerDesc selectMaterialSampler(const SceneAsset& scene, const AssetManager& assets, bool& mixedSamplers) {
    TextureSamplerDesc selected{};
    bool hasSelected = false;
    mixedSamplers = false;
    for (TextureAssetHandle handle : scene.textures) {
        const TextureAsset* texture = assets.texture(handle);
        if (texture == nullptr) {
            continue;
        }
        if (!hasSelected) {
            selected = texture->sampler;
            hasSelected = true;
            continue;
        }
        if (!sameSampler(selected, texture->sampler)) {
            mixedSamplers = true;
        }
    }
    return selected;
}

GpuPrimitiveRecord makePrimitiveRecord(
    uint32_t firstIndex,
    uint32_t indexCount,
    uint32_t firstVertex,
    uint32_t materialIndex,
    uint32_t firstTriangle,
    uint32_t triangleCount,
    uint32_t alphaClass = kPrimitiveAlphaClassOpaque,
    bool opaqueTraversalSafe = false) {
    return GpuPrimitiveRecord{
        .indexData = {firstIndex, indexCount, firstVertex, materialIndex},
        .metadata = {firstTriangle, triangleCount, alphaClass, opaqueTraversalSafe ? 1u : 0u},
    };
}

GpuMeshRecord makeMeshRecord(
    uint32_t firstVertex,
    uint32_t vertexCount,
    uint32_t firstIndex,
    uint32_t indexCount,
    uint32_t primitiveOffset,
    uint32_t primitiveCount,
    uint32_t bvhNodeOffset = 0,
    uint32_t bvhNodeCount = 0,
    uint32_t triangleOffset = 0,
    uint32_t triangleCount = 0) {
    return GpuMeshRecord{
        .vertexIndexData = {firstVertex, vertexCount, firstIndex, indexCount},
        .primitiveData = {primitiveOffset, primitiveCount, 0u, 0u},
        .bvhData = {bvhNodeOffset, bvhNodeCount, triangleOffset, triangleCount},
        .flags = {0u, 0u, 0u, 0u},
    };
}

GpuInstanceRecord makeInstanceRecord(
    const glm::mat4& transform,
    uint32_t meshIndex,
    uint32_t primitiveOffset,
    uint32_t primitiveCount,
    uint32_t flags = instanceFlagVisible | instanceFlagVisibleToCamera | instanceFlagCastShadow,
    const glm::mat4* prevTransform = nullptr) {
    const glm::mat4 inverseTransform = glm::inverse(transform);
    return GpuInstanceRecord{
        .transform = transform,
        .inverseTransform = inverseTransform,
        .normalTransform = glm::transpose(inverseTransform),
        .prevTransform = prevTransform != nullptr ? *prevTransform : transform,
        .metadata = {meshIndex, primitiveOffset, primitiveCount, flags},
    };
}

std::vector<uint32_t> buildRtTriangleMaterialIds(const std::vector<GpuPrimitiveRecord>& primitiveRecords, uint32_t rawTriangleCount) {
    std::vector<uint32_t> materialIds(std::max(rawTriangleCount, 1u), 0u);
    for (const GpuPrimitiveRecord& primitive : primitiveRecords) {
        const uint32_t firstTriangle = primitive.indexData.x / 3u;
        const uint32_t triangleCount = primitive.indexData.y / 3u;
        if (firstTriangle >= materialIds.size()) {
            continue;
        }
        const uint32_t end = std::min(firstTriangle + triangleCount, static_cast<uint32_t>(materialIds.size()));
        std::fill(materialIds.begin() + firstTriangle, materialIds.begin() + end, primitive.indexData.w);
    }
    return materialIds;
}

bool primitivesAreOpaqueTraversalSafe(
    const std::vector<GpuPrimitiveRecord>& primitiveRecords,
    uint32_t primitiveOffset,
    uint32_t primitiveCount,
    const std::vector<bool>& materialOpaqueTraversalSafe) {
    if (primitiveCount == 0) {
        return false;
    }
    for (uint32_t i = 0; i < primitiveCount; ++i) {
        const uint32_t primitiveIndex = primitiveOffset + i;
        if (primitiveIndex >= primitiveRecords.size()) {
            return false;
        }
        const uint32_t materialIndex = primitiveRecords[primitiveIndex].indexData.w;
        if (materialIndex >= materialOpaqueTraversalSafe.size() || !materialOpaqueTraversalSafe[materialIndex]) {
            return false;
        }
    }
    return true;
}

float triangleArea(glm::vec3 v0, glm::vec3 v1, glm::vec3 v2) {
    return 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
}

float luminance(glm::vec3 value) {
    return glm::dot(value, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

glm::vec3 lightRecordCentroid(const GpuLightRecord& record) {
    const uint32_t type = record.metadata.x;
    if (type == gpuLightTypeEmissiveTriangle ||
        type == gpuLightTypeEmissiveSphere ||
        type == gpuLightTypePoint ||
        type == gpuLightTypeArea ||
        type == gpuLightTypeSpot) {
        return glm::vec3(record.data1);
    }
    return glm::vec3(0.0f);
}

bool isEmissiveGpuLightRecord(const GpuLightRecord& record) {
    return record.metadata.x == gpuLightTypeEmissiveTriangle ||
        record.metadata.x == gpuLightTypeEmissiveSphere;
}

bool isAuthoredGpuLightRecord(const GpuLightRecord& record) {
    return record.metadata.x == gpuLightTypeDirectional ||
        record.metadata.x == gpuLightTypePoint ||
        record.metadata.x == gpuLightTypeArea ||
        record.metadata.x == gpuLightTypeSpot;
}

void applyLightRecordMetadataToMeshParams(MeshParamsUniform& params, const std::vector<GpuLightRecord>& records, float totalWeight) {
    params.lightCount = static_cast<uint32_t>(records.size());
    params.emissiveTotalArea = totalWeight;
    params.authoredLightOffset = params.lightCount;
    params.authoredLightCount = 0u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(records.size()); ++i) {
        if (!isAuthoredGpuLightRecord(records[i])) {
            continue;
        }
        if (params.authoredLightCount == 0u) {
            params.authoredLightOffset = i;
        }
        ++params.authoredLightCount;
    }
}

LightBvhPrimitive makeLightBvhPrimitive(const GpuLightRecord& record, uint32_t lightIndex) {
    const uint32_t type = record.metadata.x;
    const float power = std::max(record.data0.x, 0.0f);
    glm::vec3 center = lightRecordCentroid(record);
    glm::vec3 boundsMin = center;
    glm::vec3 boundsMax = center;

    if (type == gpuLightTypeEmissiveTriangle && glm::all(glm::greaterThanEqual(glm::vec3(record.data3), glm::vec3(record.data2)))) {
        boundsMin = glm::vec3(record.data2);
        boundsMax = glm::vec3(record.data3);
        center = (boundsMin + boundsMax) * 0.5f;
    } else if (type == gpuLightTypeEmissiveSphere || type == gpuLightTypePoint || type == gpuLightTypeSpot) {
        const float radius = std::max(record.data0.z, 0.001f);
        boundsMin = center - glm::vec3(radius);
        boundsMax = center + glm::vec3(radius);
    } else if (type == gpuLightTypeArea) {
        const float halfSize = std::max(record.data0.z * 0.5f, 0.001f);
        boundsMin = center - glm::vec3(halfSize);
        boundsMax = center + glm::vec3(halfSize);
    } else {
        const float extent = std::sqrt(std::max(record.data0.w, 0.0f)) * 0.5f;
        boundsMin = center - glm::vec3(std::max(extent, 0.001f));
        boundsMax = center + glm::vec3(std::max(extent, 0.001f));
    }

    return LightBvhPrimitive{
        .boundsMin = boundsMin,
        .boundsMax = boundsMax,
        .centroid = center,
        .power = power,
        .lightIndex = lightIndex,
    };
}

std::vector<GpuLightRecord> buildLightRecords(
    const std::vector<GpuMeshRecord>& meshRecords,
    const std::vector<GpuInstanceRecord>& instanceRecords,
    const std::vector<glm::vec4>& localTriangleData,
    const std::vector<glm::vec3>& materialEmissive,
    const std::vector<glm::vec4>& sphereData,
    float& totalArea) {
    totalArea = 0.0f;
    std::vector<GpuLightRecord> lights;
    for (uint32_t instanceIndex = 0; instanceIndex < instanceRecords.size(); ++instanceIndex) {
        const GpuInstanceRecord& instance = instanceRecords[instanceIndex];
        if ((instance.metadata.w & instanceFlagVisible) == 0u) {
            continue;
        }
        const uint32_t meshIndex = instance.metadata.x;
        if (meshIndex >= meshRecords.size()) {
            continue;
        }
        const GpuMeshRecord& mesh = meshRecords[meshIndex];
        const uint32_t firstTriangle = mesh.bvhData.z;
        const uint32_t triangleCount = mesh.bvhData.w;
        for (uint32_t localTriangle = 0; localTriangle < triangleCount; ++localTriangle) {
            const uint32_t packedIndex = firstTriangle + localTriangle;
            const uint32_t triBase = packedIndex * 12u;
            if (triBase + 3u >= localTriangleData.size()) {
                continue;
            }
            const uint32_t material = static_cast<uint32_t>(std::max(localTriangleData[triBase + 3u].w, 0.0f) + 0.5f);
            if (material >= materialEmissive.size() || glm::length(materialEmissive[material]) <= 0.0f) {
                continue;
            }
            const glm::vec3 v0 = glm::vec3(instance.transform * glm::vec4(glm::vec3(localTriangleData[triBase + 0u]), 1.0f));
            const glm::vec3 v1 = glm::vec3(instance.transform * glm::vec4(glm::vec3(localTriangleData[triBase + 1u]), 1.0f));
            const glm::vec3 v2 = glm::vec3(instance.transform * glm::vec4(glm::vec3(localTriangleData[triBase + 2u]), 1.0f));
            const glm::vec3 emissive = materialEmissive[material];
            const float area = triangleArea(v0, v1, v2);
            if (area <= 0.0f) {
                continue;
            }
            const glm::vec3 boundsMin = glm::min(v0, glm::min(v1, v2));
            const glm::vec3 boundsMax = glm::max(v0, glm::max(v1, v2));
            const glm::vec3 centroid = (v0 + v1 + v2) / 3.0f;
            const float weight = area * std::max(luminance(emissive), 0.0001f);
            uint32_t generation = glm::floatBitsToUint(area) ^ glm::floatBitsToUint(luminance(emissive));
            auto hashPosition = [&](const glm::vec3& value) {
                for (uint32_t component = 0; component < 3u; ++component) {
                    generation = (generation ^ glm::floatBitsToUint(value[component])) * 16777619u;
                }
            };
            hashPosition(v0);
            hashPosition(v1);
            hashPosition(v2);
            totalArea += weight;
            lights.push_back(GpuLightRecord{
                .metadata = {gpuLightTypeEmissiveTriangle, packedIndex, material, instanceIndex},
                .identity = {
                    packedIndex * 747796405u ^ instanceIndex * 2891336453u,
                    0x454d4954u,
                    generation,
                    0u},
                .data0 = {weight, totalArea, 0.0f, area},
                .data1 = {centroid, emissive.r},
                .data2 = {boundsMin, emissive.g},
                .data3 = {boundsMax, emissive.b},
            });
        }
    }

    const uint32_t sphereCount = static_cast<uint32_t>(sphereData.size() / 4u);
    for (uint32_t sphereIndex = 0; sphereIndex < sphereCount; ++sphereIndex) {
        const glm::vec4 sphere = sphereData[sphereIndex * 4u];
        const glm::vec3 emissive = glm::vec3(sphereData[sphereIndex * 4u + 3u]);
        if (glm::length(emissive) <= 0.0f || sphere.w <= 0.0f) {
            continue;
        }
        const float area = 4.0f * 3.14159265358979323846f * sphere.w * sphere.w;
        const float weight = area * std::max(luminance(emissive), 0.0001f);
        uint32_t generation = glm::floatBitsToUint(sphere.w) ^ glm::floatBitsToUint(luminance(emissive));
        for (uint32_t component = 0; component < 3u; ++component) {
            generation = (generation ^ glm::floatBitsToUint(sphere[component])) * 16777619u;
        }
        totalArea += weight;
        lights.push_back(GpuLightRecord{
            .metadata = {gpuLightTypeEmissiveSphere, sphereIndex, 0u, static_cast<uint32_t>(lights.size())},
            .identity = {
                sphereIndex * 747796405u,
                0x53504852u,
                generation,
                0u},
            .data0 = {weight, totalArea, sphere.w, area},
            .data1 = {glm::vec3(sphere), 0.0f},
            .data2 = {emissive, 0.0f},
        });
    }
    return lights;
}

glm::vec3 safeNormalize(glm::vec3 value, glm::vec3 fallback) {
    const float len2 = glm::dot(value, value);
    return len2 > 0.000001f ? value * glm::inversesqrt(len2) : fallback;
}

std::vector<GpuLightRecord> buildAuthoredLightRecords(const std::vector<SceneLightAsset>& lights, float startWeight, float& totalWeight) {
    totalWeight = startWeight;
    std::vector<GpuLightRecord> records;
    records.reserve(lights.size());
    for (uint32_t i = 0; i < lights.size(); ++i) {
        const SceneLightAsset& light = lights[i];
        if (!light.enabled || light.intensity <= 0.0f || luminance(light.color) <= 0.0f) {
            continue;
        }

        uint32_t type = gpuLightTypePoint;
        switch (light.type) {
        case sceneLightTypeDirectional:
            type = gpuLightTypeDirectional;
            break;
        case sceneLightTypePoint:
            type = gpuLightTypePoint;
            break;
        case sceneLightTypeArea:
            type = gpuLightTypeArea;
            break;
        case sceneLightTypeSpot:
            type = gpuLightTypeSpot;
            break;
        default:
            type = gpuLightTypePoint;
            break;
        }

        const float rawSize = light.sizeOrRadius;
        float size = std::max(rawSize, 0.0001f);
        if (type == gpuLightTypeDirectional) {
            size = rawSize > 0.0f ? std::clamp(rawSize, 0.0001f, 0.08f) : 0.00465f;
        }

        glm::vec3 radiance = light.color * light.intensity;
        if (type == gpuLightTypeDirectional) {
            const float solidAngle = std::max(2.0f * 3.14159265358979323846f * (1.0f - std::cos(size)), 1.0e-8f);
            radiance /= solidAngle;
        }

        float weight = std::max(luminance(radiance), 0.0001f);
        const float area = type == gpuLightTypeArea ? size * size : 0.0f;
        if (type == gpuLightTypeArea) {
            weight *= area;
        }
        totalWeight += weight;

        const glm::vec3 position = glm::vec3(light.transform[3]);
        const glm::vec3 forward = safeNormalize(glm::mat3(light.transform) * glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
        const glm::vec3 toLightDirection = -forward;
        const glm::vec3 normal = forward;

        GpuLightRecord record{};
        record.metadata = {type, i, 0u, 0u};
        uint32_t generation = 2166136261u;
        auto hashWord = [&](uint32_t word) {
            generation = (generation ^ word) * 16777619u;
        };
        hashWord(type);
        hashWord(glm::floatBitsToUint(light.intensity));
        hashWord(glm::floatBitsToUint(light.sizeOrRadius));
        for (uint32_t component = 0; component < 3u; ++component) hashWord(glm::floatBitsToUint(light.color[component]));
        for (uint32_t column = 0; column < 4u; ++column) {
            for (uint32_t row = 0; row < 4u; ++row) hashWord(glm::floatBitsToUint(light.transform[column][row]));
        }
        const uint64_t persistentId = light.persistentId != 0u
            ? light.persistentId
            : (0x4155544800000000ull | static_cast<uint64_t>(static_cast<uint32_t>(std::max(light.nodeIndex, 0)) + 1u));
        record.identity = {
            static_cast<uint32_t>(persistentId),
            static_cast<uint32_t>(persistentId >> 32u),
            generation,
            0u};
        record.data0 = {weight, totalWeight, size, area};
        record.data1 = type == gpuLightTypeDirectional ? glm::vec4(toLightDirection, normal.x) : glm::vec4(position, normal.x);
        record.data2 = {radiance, normal.y};
        record.data3 = {
            normal.z,
            std::clamp(light.innerConeRadians, 0.0f, 3.14159265358979323846f),
            std::clamp(light.outerConeRadians, 0.0f, 3.14159265358979323846f),
            size};
        records.push_back(record);
    }
    return records;
}

std::vector<GpuLightRecord> combineLightRecords(const std::vector<GpuLightRecord>& emissiveRecords, const std::vector<SceneLightAsset>& authoredLights, float emissiveWeight, float& totalWeight) {
    std::vector<GpuLightRecord> records = emissiveRecords;
    float runningWeight = emissiveWeight;
    std::vector<GpuLightRecord> authored = buildAuthoredLightRecords(authoredLights, runningWeight, totalWeight);
    records.insert(records.end(), authored.begin(), authored.end());
    if (authored.empty()) {
        totalWeight = runningWeight;
    }
    return records;
}

std::vector<glm::vec3> extractMaterialEmissive(const std::vector<glm::vec4>& materialData) {
    std::vector<glm::vec3> emissive;
    const size_t materialCount = materialData.size() / materialVec4Stride;
    emissive.reserve(materialCount);
    for (size_t i = 0; i < materialCount; ++i) {
        emissive.push_back(glm::vec3(materialData[i * materialVec4Stride + 2u]));
    }
    return emissive;
}

CachedSceneLightData toCachedSceneLightData(const SceneLightAsset& light) {
    CachedSceneLightData cachedLight;
    cachedLight.type = light.type;
    cachedLight.transform = light.transform;
    cachedLight.color = light.color;
    cachedLight.intensity = light.intensity;
    cachedLight.sizeOrRadius = light.sizeOrRadius;
    cachedLight.innerConeRadians = light.innerConeRadians;
    cachedLight.outerConeRadians = light.outerConeRadians;
    cachedLight.enabled = light.enabled ? 1u : 0u;
    cachedLight.nodeIndex = light.nodeIndex;
    return cachedLight;
}

void appendCachedSceneLights(CachedScene& cached, const std::vector<SceneLightAsset>& lights) {
    cached.sceneLights.reserve(cached.sceneLights.size() + lights.size());
    for (const SceneLightAsset& light : lights) {
        cached.sceneLights.push_back(toCachedSceneLightData(light));
    }
}

} // namespace

GpuScene::GpuScene(
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    const SceneAsset* importedScene,
    const AssetManager* assets,
    std::optional<std::filesystem::path> environmentPath,
    SceneCachePolicy sceneCachePolicy,
    uint32_t opacityMicromapSubdivisionLevel,
    bool opacityMicromapsEnabled,
    uint32_t materialTextureMaxDimension)
    : allocator_(allocator),
      environmentPath_(std::move(environmentPath)),
      sceneCachePolicy_(std::move(sceneCachePolicy)),
      opacityMicromapSubdivisionLevel_(opacityMicromapSubdivisionLevel),
      opacityMicromapsEnabled_(opacityMicromapsEnabled),
      materialTextureMaxDimension_(materialTextureMaxDimension) {
    materialTextureAnisotropy_ = allocator_.supportsSamplerAnisotropy()
        ? std::clamp(8.0f, 1.0f, allocator_.maxSamplerAnisotropy())
        : 1.0f;
    bool usedGpuCache = false;
    if (importedScene != nullptr && assets != nullptr && !importedScene->meshes.empty()) {
        if (sceneCachePolicy_.canRead()) {
            auto cached = SceneCache::load(*sceneCachePolicy_.path);
            if (cached.has_value()) {
                if (sceneCachePolicy_.mode == SceneCacheMode::FullReadWrite && importedScene->lights.empty()) {
                    if (hasValidGpuCache(*cached, *importedScene)) {
                        createImportedSceneFromCache(uploader, *cached, importedScene->lights);
                        usedGpuCache = true;
                    } else {
                        std::cout << "GPU cache miss: full cache rejected for active scene signature.\n";
                    }
                } else if (sceneCachePolicy_.mode == SceneCacheMode::GeometryReadOnly) {
                    if (hasValidGpuGeometryCache(*cached, *importedScene)) {
                        createImportedSceneGeometryFromCache(uploader, *cached, *importedScene);
                        usedGpuCache = true;
                    } else {
                        std::cout << "GPU geometry cache miss: geometry cache rejected for active scene signature.\n";
                    }
                }
            } else {
                std::cout << "GPU cache miss: failed to load " << sceneCachePolicy_.path->string() << ".\n";
            }
        } else if (sceneCachePolicy_.mode == SceneCacheMode::Disabled) {
            std::cout << "GPU cache disabled for renderer startup.\n";
        }
        if (!usedGpuCache) {
            createImportedScene(uploader, *importedScene, *assets);
        }
    } else {
        createCornellBox(uploader);
    }
    createEnvironment(uploader);
}

GpuScene::~GpuScene() {
    destroyMaterialTextureSamplers();
    for (RetiredMaterialSampler retired : retiredMaterialSamplers_) {
        if (retired.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(allocator_.device(), retired.sampler, nullptr);
        }
    }
    retiredMaterialSamplers_.clear();
    if (environmentSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(allocator_.device(), environmentSampler_, nullptr);
    }
    if (materialSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(allocator_.device(), materialSampler_, nullptr);
    }
}

void GpuScene::destroyMaterialTextureSamplers() {
    for (VkSampler sampler : materialTextureSamplers_) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(allocator_.device(), sampler, nullptr);
        }
    }
    materialTextureSamplers_.clear();
    materialTextureSamplerDescs_.clear();
}

void GpuScene::retireMaterialTextureSampler(VkSampler sampler, uint64_t retireFrame) {
    if (sampler == VK_NULL_HANDLE) {
        return;
    }
    retiredMaterialSamplers_.push_back(RetiredMaterialSampler{sampler, retireFrame});
}

void GpuScene::retireBuffer(std::unique_ptr<Buffer> buffer, uint64_t retireFrame) {
    if (buffer == nullptr || buffer->handle() == VK_NULL_HANDLE) {
        return;
    }
    retiredBuffers_.push_back(RetiredBuffer{std::move(buffer), retireFrame});
}

void GpuScene::retireImage(std::unique_ptr<Image> image, uint64_t retireFrame) {
    if (image == nullptr || image->handle() == VK_NULL_HANDLE) {
        return;
    }
    retiredImages_.push_back(RetiredImage{std::move(image), retireFrame});
}

void GpuScene::retireEnvironmentResources(uint64_t retireFrame) {
    retireImage(std::move(environmentImage_), retireFrame);
    retireBuffer(std::move(envRows_), retireFrame);
    retireBuffer(std::move(envCols_), retireFrame);
    retireBuffer(std::move(envParamsBuffer_), retireFrame);
}

void GpuScene::releaseRetiredMaterialSamplers(uint64_t completedFrame) {
    auto firstLive = std::remove_if(
        retiredMaterialSamplers_.begin(),
        retiredMaterialSamplers_.end(),
        [&](const RetiredMaterialSampler& retired) {
            if (retired.retireFrame > completedFrame) {
                return false;
            }
            if (retired.sampler != VK_NULL_HANDLE) {
                vkDestroySampler(allocator_.device(), retired.sampler, nullptr);
            }
            return true;
        });
    retiredMaterialSamplers_.erase(firstLive, retiredMaterialSamplers_.end());

    auto firstLiveBuffer = std::remove_if(
        retiredBuffers_.begin(),
        retiredBuffers_.end(),
        [&](const RetiredBuffer& retired) {
            return retired.retireFrame <= completedFrame;
        });
    retiredBuffers_.erase(firstLiveBuffer, retiredBuffers_.end());

    auto firstLiveImage = std::remove_if(
        retiredImages_.begin(),
        retiredImages_.end(),
        [&](const RetiredImage& retired) {
            return retired.retireFrame <= completedFrame;
        });
    retiredImages_.erase(firstLiveImage, retiredImages_.end());
}

void GpuScene::recreateMaterialTextureSamplers(uint64_t retireFrame) {
    VkSampler replacement = createMaterialSampler(allocator_, materialSamplerDesc_, materialTextureAnisotropy_);
    retireMaterialTextureSampler(materialSampler_, retireFrame);
    materialSampler_ = replacement;

    std::vector<VkSampler> replacements;
    replacements.reserve(materialTextureSamplerDescs_.size());
    for (const TextureSamplerDesc& desc : materialTextureSamplerDescs_) {
        replacements.push_back(createMaterialSampler(allocator_, desc, materialTextureAnisotropy_));
    }
    for (VkSampler sampler : materialTextureSamplers_) {
        retireMaterialTextureSampler(sampler, retireFrame);
    }
    materialTextureSamplers_ = std::move(replacements);
}

bool GpuScene::setMaterialTextureAnisotropy(float anisotropy, uint64_t retireFrame) {
    const float supportedMax = allocator_.supportsSamplerAnisotropy() ? allocator_.maxSamplerAnisotropy() : 1.0f;
    const float clamped = std::clamp(std::isfinite(anisotropy) ? anisotropy : 1.0f, 1.0f, supportedMax);
    if (std::abs(clamped - materialTextureAnisotropy_) <= 0.0001f) {
        return false;
    }

    materialTextureAnisotropy_ = clamped;
    if (materialSampler_ != VK_NULL_HANDLE) {
        recreateMaterialTextureSamplers(retireFrame);
    }
    return true;
}

uint32_t meshPrimitiveAlphaClass(
    const std::vector<GpuPrimitiveRecord>& primitiveRecords,
    uint32_t primitiveOffset,
    uint32_t primitiveCount,
    uint32_t alphaClass) {
    for (uint32_t i = 0; i < primitiveCount; ++i) {
        const uint32_t primitiveIndex = primitiveOffset + i;
        if (primitiveIndex < primitiveRecords.size() && primitiveRecords[primitiveIndex].metadata.z == alphaClass) {
            return 1u;
        }
    }
    return 0u;
}

void annotatePrimitiveAlphaClasses(
    std::vector<GpuPrimitiveRecord>& primitiveRecords,
    const std::vector<uint32_t>& materialAlphaClasses,
    const std::vector<bool>* materialOpaqueTraversalSafe = nullptr) {
    for (GpuPrimitiveRecord& primitive : primitiveRecords) {
        const uint32_t materialIndex = primitive.indexData.w;
        primitive.metadata.z = materialIndex < materialAlphaClasses.size()
            ? materialAlphaClasses[materialIndex]
            : kPrimitiveAlphaClassOpaque;
        primitive.metadata.w = materialOpaqueTraversalSafe != nullptr && materialIndex < materialOpaqueTraversalSafe->size() && (*materialOpaqueTraversalSafe)[materialIndex]
            ? 1u
            : 0u;
    }
}

RayTracingGeometryStats computeRayTracingGeometryStats(
    const std::vector<GpuMeshRecord>& meshRecords,
    const std::vector<GpuPrimitiveRecord>& primitiveRecords) {
    RayTracingGeometryStats stats{};
    for (const GpuPrimitiveRecord& primitive : primitiveRecords) {
        const uint32_t triangleCount = primitive.indexData.y / 3u;
        switch (primitive.metadata.z) {
        case kPrimitiveAlphaClassAlphaTested:
            ++stats.alphaTestedPrimitiveCount;
            stats.alphaTestedTriangleCount += triangleCount;
            break;
        case kPrimitiveAlphaClassBlended:
            ++stats.blendedPrimitiveCount;
            stats.blendedTriangleCount += triangleCount;
            break;
        default:
            ++stats.opaquePrimitiveCount;
            stats.opaqueTriangleCount += triangleCount;
            break;
        }
    }

    for (const GpuMeshRecord& mesh : meshRecords) {
        const uint32_t primitiveOffset = mesh.primitiveData.x;
        const uint32_t primitiveCount = mesh.primitiveData.y;
        const bool hasAlphaTested = meshPrimitiveAlphaClass(
            primitiveRecords,
            primitiveOffset,
            primitiveCount,
            kPrimitiveAlphaClassAlphaTested) != 0u;
        const bool hasBlended = meshPrimitiveAlphaClass(
            primitiveRecords,
            primitiveOffset,
            primitiveCount,
            kPrimitiveAlphaClassBlended) != 0u;
        stats.meshCountWithAlphaTestedGeometry += hasAlphaTested ? 1u : 0u;
        stats.meshCountWithBlendedGeometry += hasBlended ? 1u : 0u;
        stats.meshCountWithOnlyOpaqueGeometry += (!hasAlphaTested && !hasBlended) ? 1u : 0u;
    }
    return stats;
}

void logRayTracingGeometryStats(const char* label, const RayTracingGeometryStats& stats) {
    std::cout << label
              << " RT geometry: opaque_primitives=" << stats.opaquePrimitiveCount
              << " alpha_tested_primitives=" << stats.alphaTestedPrimitiveCount
              << " blended_primitives=" << stats.blendedPrimitiveCount
              << " opaque_triangles=" << stats.opaqueTriangleCount
              << " alpha_tested_triangles=" << stats.alphaTestedTriangleCount
              << " blended_triangles=" << stats.blendedTriangleCount << '\n';
}

void logOpacityMicromapPreprocessStats(const char* label, const OpacityMicromapPreprocessStats& stats) {
    std::cout << label
              << " OMM preprocess: eligible_primitives=" << stats.eligiblePrimitiveCount
              << " generated_primitives=" << stats.generatedPrimitiveCount
              << " micro_triangles=" << stats.microTriangleCount
              << " opaque=" << stats.opaqueCount
              << " transparent=" << stats.transparentCount
              << " mixed=" << stats.mixedCount
              << " unknown=" << stats.unknownCount
              << " bytes=" << stats.dataBytes
              << " time_ms=" << stats.preprocessingMs << '\n';
}

std::vector<VkDescriptorImageInfo> GpuScene::materialCombinedDescriptors() const {
    const auto& texDescs = materialTextureTable_.descriptors();
    std::vector<VkDescriptorImageInfo> result;
    result.reserve(texDescs.size());
    for (const auto& tex : texDescs) {
        const size_t index = result.size();
        VkDescriptorImageInfo combined{};
        combined.imageView = tex.imageView;
        combined.imageLayout = tex.imageLayout;
        combined.sampler = index < materialTextureSamplers_.size() ? materialTextureSamplers_[index] : materialSampler_;
        result.push_back(combined);
    }
    return result;
}

bool GpuScene::setEnvironmentControls(bool enabled, float intensity, float rotation, float backgroundIntensity) {
    const uint32_t enabledValue = enabled ? 1u : 0u;
    const bool changed =
        envParams_.enabled != enabledValue ||
        std::abs(envParams_.intensity - intensity) > 0.0001f ||
        std::abs(envParams_.rotation - rotation) > 0.0001f ||
        std::abs(envParams_.backgroundIntensity - backgroundIntensity) > 0.0001f;
    if (!changed) {
        return false;
    }

    envParams_.enabled = enabledValue;
    envParams_.intensity = std::max(0.0f, intensity);
    envParams_.rotation = rotation;
    envParams_.backgroundIntensity = std::max(0.0f, backgroundIntensity);
    uploadEnvironmentParams();
    return true;
}

bool GpuScene::setSkyCdfDimensions(uint32_t width, uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (envParams_.skyCdfWidth == width && envParams_.skyCdfHeight == height) {
        return false;
    }

    envParams_.skyCdfWidth = width;
    envParams_.skyCdfHeight = height;
    uploadEnvironmentParams();
    return true;
}

void GpuScene::createDefaultMaterialTexture(BufferUploader& uploader) {
    if (materialSampler_ == VK_NULL_HANDLE) {
        materialSamplerDesc_ = TextureSamplerDesc{};
        materialSampler_ = createMaterialSampler(allocator_, materialSamplerDesc_, materialTextureAnisotropy_);
    }

    if (materialTextureTable_.residentCount() > 0) {
        return;
    }
    destroyMaterialTextureSamplers();

    const std::array<uint8_t, 4> white = {255, 255, 255, 255};
    std::vector<std::unique_ptr<Image>> images;
    auto image = std::make_unique<Image>(allocator_, ImageDesc{
        .width = 1,
        .height = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = "default material texture",
    });
    uploader.uploadToImage2D(*image, white.data(), white.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    images.push_back(std::move(image));
    materialTextureTable_.setImages(std::move(images), maxMaterialTextures);
    materialTextureTable_.setRegistrationInfo(std::vector<BindlessTextureRegistrationInfo>{BindlessTextureRegistrationInfo{
        .slot = 0,
        .nativeSource = "default",
        .resident = true,
        .fallback = true,
    }});
}

void GpuScene::createImportedMaterialTextures(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets) {
    materialTextureTable_.clear();
    destroyMaterialTextureSamplers();
    bool mixedSamplers = false;
    const TextureSamplerDesc materialSamplerDesc = selectMaterialSampler(importedScene, assets, mixedSamplers);
    materialSamplerDesc_ = materialSamplerDesc;
    materialSampler_ = createMaterialSampler(allocator_, materialSamplerDesc_, materialTextureAnisotropy_);
    if (mixedSamplers) {
        std::cout << "glTF scene uses mixed texture samplers; using the first sampler until per-texture bindless samplers are enabled.\n";
    }

    const std::vector<TextureColorUsage> usage = classifyTextureUsage(importedScene, assets);
    const uint32_t textureCount = std::min<uint32_t>(static_cast<uint32_t>(importedScene.textures.size()), maxMaterialTextures);

    struct PendingTexture {
        std::unique_ptr<Image> image;
        std::vector<uint8_t> pixels;
        std::vector<TextureMipLevel> mipData;
        TextureSamplerDesc sampler;
        BindlessTextureRegistrationInfo registration;
    };
    std::vector<PendingTexture> pendingTextures;
    pendingTextures.reserve(std::max(1u, textureCount));
    uint64_t highPrecisionBytes = 0;
    uint64_t sourceTexturePayloadBytes = 0;
    uint64_t uploadedTexturePayloadBytes = 0;
    uint32_t cappedPreviewTextures = 0;

    for (uint32_t slot = 0; slot < std::max(1u, textureCount); ++slot) {
        const TextureAsset* texture = slot < textureCount ? assets.texture(importedScene.textures[slot]) : nullptr;
        std::vector<uint8_t> pixels;
        std::vector<TextureMipLevel> uploadMipData;
        uint32_t width = 1;
        uint32_t height = 1;
        const char* name = "imported material texture";
        if (texture != nullptr && !texture->rgba8.empty() && texture->width > 0 && texture->height > 0) {
            if (texture->fallback) {
                pixels = fallbackTexturePixels(usage, slot);
                width = texture->width;
                height = texture->height;
            } else {
                MaterialTextureUploadPayload uploadPayload = makeMaterialTextureUploadPayload(*texture, materialTextureMaxDimension_);
                pixels = std::move(uploadPayload.bytes);
                uploadMipData = std::move(uploadPayload.mipData);
                width = uploadPayload.width;
                height = uploadPayload.height;
                if (uploadPayload.capped) {
                    ++cappedPreviewTextures;
                }
            }
            name = texture->fallback ? "fallback material texture" : name;
        } else {
            pixels = fallbackTexturePixels(usage, slot);
            name = "default material texture";
        }
        sourceTexturePayloadBytes += texture != nullptr ? static_cast<uint64_t>(texture->rgba8.size()) : static_cast<uint64_t>(pixels.size());
        uploadedTexturePayloadBytes += static_cast<uint64_t>(pixels.size());

        const bool hasUploadedMips = !uploadMipData.empty();
        const uint32_t textureMipLevels = hasUploadedMips
            ? static_cast<uint32_t>(uploadMipData.size())
            : (texture != nullptr && texture->isCompressed
                ? std::max(1u, static_cast<uint32_t>(texture->mipLevels))
                : std::max(1u, static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1u)));
        const VkFormat textureFormat = importedMaterialTextureFormat(texture, usage, slot);
        const uint64_t textureBytes = estimatedTextureBytes(pixels.size(), textureMipLevels, hasUploadedMips);
        if (isHighPrecisionTextureFormat(textureFormat)) {
            highPrecisionBytes += textureBytes;
            warnHighPrecisionTextureMemory(
                "Material",
                texture != nullptr ? texture->name : std::string{},
                textureFormat,
                width,
                height,
                textureBytes);
        }

        auto image = std::make_unique<Image>(allocator_, ImageDesc{
            .width = width,
            .height = height,
            .mipLevels = textureMipLevels,
            .format = textureFormat,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .debugName = name,
        });

        const TextureSamplerDesc samplerDesc = texture != nullptr ? texture->sampler : materialSamplerDesc;
        BindlessTextureRegistrationInfo registration;
        registration.slot = slot;
        registration.resident = true;
        registration.fallback = texture == nullptr || texture->fallback || texture->rgba8.empty() || texture->width == 0 || texture->height == 0;
        registration.missing = texture == nullptr;
        if (texture != nullptr) {
            registration.guid = texture->nativeGuid;
            registration.nativeSource = texture->nativeSource;
            registration.nativePath = texture->nativePath;
        }
        pendingTextures.push_back({
            std::move(image),
            std::move(pixels),
            std::move(uploadMipData),
            samplerDesc,
            std::move(registration),
        });
    }
    warnHighPrecisionTextureBudget("Material", highPrecisionBytes);
    if (materialTextureMaxDimension_ > 0 && cappedPreviewTextures > 0) {
        constexpr double oneMiB = 1024.0 * 1024.0;
        std::cout << "Material texture preview cap: max_dim=" << materialTextureMaxDimension_
                  << " capped=" << cappedPreviewTextures << " / " << textureCount
                  << " upload=" << (static_cast<double>(uploadedTexturePayloadBytes) / oneMiB)
                  << " MiB source=" << (static_cast<double>(sourceTexturePayloadBytes) / oneMiB)
                  << " MiB\n";
    }

    BatchUploader batch(uploader);
    batch.begin();
    for (auto& pending : pendingTextures) {
        batch.enqueueImageUpload(*pending.image, pending.pixels.data(), pending.pixels.size(), pending.mipData, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    batch.submit();

    std::vector<std::unique_ptr<Image>> images;
    std::vector<BindlessTextureRegistrationInfo> registrations;
    images.reserve(pendingTextures.size());
    registrations.reserve(pendingTextures.size());
    materialTextureSamplerDescs_.reserve(pendingTextures.size());
    for (auto& pending : pendingTextures) {
        materialTextureSamplerDescs_.push_back(pending.sampler);
        materialTextureSamplers_.push_back(createMaterialSampler(allocator_, pending.sampler, materialTextureAnisotropy_));
        registrations.push_back(std::move(pending.registration));
        images.push_back(std::move(pending.image));
    }

    materialTextureTable_.setImages(std::move(images), maxMaterialTextures);
    materialTextureTable_.setRegistrationInfo(std::move(registrations));
    std::cout << "Material textures resident: " << materialTextureTable_.residentCount() << " / " << materialTextureTable_.slotCount() << " slots\n";
}

void GpuScene::createCachedMaterialTextures(BufferUploader& uploader, const CachedScene& cached) {
    materialTextureTable_.clear();
    destroyMaterialTextureSamplers();

    TextureSamplerDesc materialSamplerDesc{};
    if (!cached.textures.empty()) {
        const CachedTextureData& first = cached.textures.front();
        materialSamplerDesc.minFilter = static_cast<TextureFilter>(first.minFilter);
        materialSamplerDesc.magFilter = static_cast<TextureFilter>(first.magFilter);
        materialSamplerDesc.wrapS = static_cast<TextureWrap>(first.wrapS);
        materialSamplerDesc.wrapT = static_cast<TextureWrap>(first.wrapT);
    }
    materialSamplerDesc_ = materialSamplerDesc;
    materialSampler_ = createMaterialSampler(allocator_, materialSamplerDesc_, materialTextureAnisotropy_);

    const uint32_t textureCount = std::min<uint32_t>(static_cast<uint32_t>(cached.textures.size()), maxMaterialTextures);

    struct PendingTexture {
        std::unique_ptr<Image> image;
        std::vector<uint8_t> pixels;
        std::vector<TextureMipLevel> mipData;
        TextureSamplerDesc sampler;
        BindlessTextureRegistrationInfo registration;
    };
    std::vector<PendingTexture> pendingTextures;
    pendingTextures.reserve(std::max(1u, textureCount));
    uint64_t highPrecisionBytes = 0;

    for (uint32_t slot = 0; slot < std::max(1u, textureCount); ++slot) {
        const CachedTextureData* texture = slot < textureCount ? &cached.textures[slot] : nullptr;
        std::vector<uint8_t> pixels;
        uint32_t width = 1;
        uint32_t height = 1;
        const char* name = "cached material texture";
        TextureSamplerDesc samplerDesc = materialSamplerDesc;
        if (texture != nullptr && !texture->rgba8.empty() && texture->width > 0 && texture->height > 0) {
            pixels = texture->rgba8;
            width = texture->width;
            height = texture->height;
            samplerDesc.minFilter = static_cast<TextureFilter>(texture->minFilter);
            samplerDesc.magFilter = static_cast<TextureFilter>(texture->magFilter);
            samplerDesc.wrapS = static_cast<TextureWrap>(texture->wrapS);
            samplerDesc.wrapT = static_cast<TextureWrap>(texture->wrapT);
        } else {
            pixels = {255, 255, 255, 255};
            name = "default cached material texture";
        }

        const bool textureCompressed = texture != nullptr && texture->isCompressed;
        const bool hasUploadedMips = texture != nullptr && !texture->mipData.empty();
        const uint32_t textureMipLevels = hasUploadedMips
            ? static_cast<uint32_t>(texture->mipData.size())
            : (textureCompressed
                ? std::max(1u, static_cast<uint32_t>(texture->mipLevels))
                : std::max(1u, static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1u)));
        const VkFormat textureFormat = cachedMaterialTextureFormat(texture);
        const uint64_t textureBytes = estimatedTextureBytes(pixels.size(), textureMipLevels, hasUploadedMips);
        if (isHighPrecisionTextureFormat(textureFormat)) {
            highPrecisionBytes += textureBytes;
            warnHighPrecisionTextureMemory(
                "Cached material",
                texture != nullptr ? texture->name : std::string{},
                textureFormat,
                width,
                height,
                textureBytes);
        }
        auto image = std::make_unique<Image>(allocator_, ImageDesc{
            .width = width,
            .height = height,
            .mipLevels = textureMipLevels,
            .format = textureFormat,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .debugName = name,
        });
        pendingTextures.push_back({
            std::move(image),
            std::move(pixels),
            texture != nullptr ? texture->mipData : std::vector<TextureMipLevel>{},
            samplerDesc,
            BindlessTextureRegistrationInfo{.slot = slot, .nativeSource = "cache", .resident = true, .fallback = texture == nullptr || texture->fallback, .missing = texture == nullptr},
        });
    }
    warnHighPrecisionTextureBudget("Cached material", highPrecisionBytes);

    BatchUploader batch(uploader);
    batch.begin();
    for (auto& pending : pendingTextures) {
        batch.enqueueImageUpload(*pending.image, pending.pixels.data(), pending.pixels.size(), pending.mipData, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    batch.submit();

    std::vector<std::unique_ptr<Image>> images;
    std::vector<BindlessTextureRegistrationInfo> registrations;
    images.reserve(pendingTextures.size());
    registrations.reserve(pendingTextures.size());
    materialTextureSamplerDescs_.reserve(pendingTextures.size());
    for (auto& pending : pendingTextures) {
        materialTextureSamplerDescs_.push_back(pending.sampler);
        materialTextureSamplers_.push_back(createMaterialSampler(allocator_, pending.sampler, materialTextureAnisotropy_));
        registrations.push_back(std::move(pending.registration));
        images.push_back(std::move(pending.image));
    }

    materialTextureTable_.setImages(std::move(images), maxMaterialTextures);
    materialTextureTable_.setRegistrationInfo(std::move(registrations));
    std::cout << "Cached material textures resident: " << materialTextureTable_.residentCount() << " / " << materialTextureTable_.slotCount() << " slots\n";
}

void GpuScene::loadEnvironment(BufferUploader& uploader, const std::filesystem::path& path, uint64_t retireFrame) {
    environmentPath_ = path;
    retireEnvironmentResources(retireFrame);
    createEnvironment(uploader);
}

bool GpuScene::updateImportedMaterials(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets) {
    if (materials_ == nullptr || importedScene.materials.empty()) {
        return false;
    }

    const std::vector<TextureColorUsage> textureUsage = classifyTextureUsage(importedScene, assets);
    auto textureSlotFor = [&](TextureAssetHandle texture) {
        const uint32_t slot = GpuScene::textureSlotIndexFor(importedScene, texture, maxMaterialTextures);
        return slot == UINT32_MAX ? -1.0f : static_cast<float>(slot);
    };

    std::vector<glm::vec4> materialData;
    materialData.reserve(importedScene.materials.size() * materialVec4Stride);
    for (MaterialAssetHandle handle : importedScene.materials) {
        const MaterialAsset* material = assets.material(handle);
        const glm::vec4 base = effectiveMaterialBaseColor(material, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
        const glm::vec3 emissive = material != nullptr ? material->emissiveFactor : glm::vec3(0.0f);
        const float roughness = material != nullptr ? material->roughnessFactor : 1.0f;
        const float metallic = material != nullptr ? material->metallicFactor : 0.0f;
        const uint32_t type = importedMaterialType(material);
        uint32_t flags = materialSemanticFlags(material);
        if (material != nullptr) {
            uint32_t slot = GpuScene::textureSlotIndexFor(importedScene, material->baseColorTexture, maxMaterialTextures);
            if (slot != UINT32_MAX && importedMaterialTextureNeedsManualSrgb(assets.texture(material->baseColorTexture), textureUsage, slot)) {
                flags |= materialFlagManualBaseColorSrgb;
            }
            slot = GpuScene::textureSlotIndexFor(importedScene, material->emissiveTexture, maxMaterialTextures);
            if (slot != UINT32_MAX && importedMaterialTextureNeedsManualSrgb(assets.texture(material->emissiveTexture), textureUsage, slot)) {
                flags |= materialFlagManualEmissiveSrgb;
            }
        }

        materialData.push_back({glm::vec3(base), roughness});
        materialData.push_back({material != nullptr ? material->iorFactor : 1.5f, static_cast<float>(type), metallic, static_cast<float>(flags)});
        materialData.push_back({emissive, base.a});
        materialData.push_back({
            material != nullptr ? textureSlotFor(material->baseColorTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->normalTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->metallicRoughnessTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->emissiveTexture) : -1.0f});
        materialData.push_back({
            material != nullptr ? material->alphaCutoff : 0.5f,
            material != nullptr ? static_cast<float>(material->alphaMode) : 0.0f,
            material != nullptr ? static_cast<float>(material->doubleSided) : 0.0f,
            0.0f});
        appendConductorOptics(
            materialData,
            material,
            material != nullptr ? textureSlotFor(material->occlusionTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->sheenColorTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->sheenRoughnessTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->iridescenceTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->iridescenceThicknessTexture) : -1.0f);
        appendGltfMaterialExtensionData(
            materialData,
            material,
            material != nullptr ? textureSlotFor(material->clearcoatTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->clearcoatRoughnessTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->clearcoatNormalTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->transmissionTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->volumeThicknessTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->specularTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->specularColorTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->anisotropyTexture) : -1.0f,
            effectiveMaterialTransmissionFactor(material));
        appendOpacityHeightTextureIndices(
            materialData,
            material != nullptr ? textureSlotFor(material->opacityTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->heightTexture) : -1.0f,
            material != nullptr ? material->heightScale : 0.025f);
        appendMaterialTextureTransforms(materialData, material);
    }

    const VkDeviceSize byteSize = sizeof(glm::vec4) * materialData.size();
    if (byteSize == 0 || byteSize > materials_->size()) {
        return false;
    }

    std::unordered_map<uint32_t, uint32_t> materialIndexForAsset;
    materialIndexForAsset.reserve(importedScene.materials.size());
    for (uint32_t i = 0; i < importedScene.materials.size(); ++i) {
        materialIndexForAsset.emplace(importedScene.materials[i].index, i);
    }

    uint32_t primitiveCursor = 0;
    for (MeshAssetHandle meshHandle : importedScene.meshes) {
        const MeshAsset* mesh = assets.mesh(meshHandle);
        if (mesh == nullptr) {
            return false;
        }
        for (const MeshPrimitiveAsset& primitive : mesh->primitives) {
            if (primitiveCursor >= primitiveRecordCpu_.size()) {
                return false;
            }
            const auto materialIt = materialIndexForAsset.find(primitive.material.index);
            const uint32_t materialIndex = materialIt != materialIndexForAsset.end() ? materialIt->second : 0u;
            const MaterialAsset* material = assets.material(primitive.material);
            const uint32_t alphaClass = primitiveAlphaClassForMaterial(material);
            const bool opaqueTraversalSafe = material != nullptr && material->alphaMode == 0u && material->doubleSided != 0u;
            const GpuPrimitiveRecord& current = primitiveRecordCpu_[primitiveCursor];
            if (current.indexData.w != materialIndex ||
                current.metadata.z != alphaClass ||
                current.metadata.w != (opaqueTraversalSafe ? 1u : 0u)) {
                return false;
            }
            ++primitiveCursor;
        }
    }
    if (primitiveCursor != primitiveRecordCpu_.size()) {
        return false;
    }

    uploader.uploadToBuffer(*materials_, materialData.data(), byteSize);
    meshParams_.materialCount = static_cast<uint32_t>(materialData.size() / materialVec4Stride);
    hasTransmissiveMaterials_ = materialDataContainsTransmission(materialData);
    if (meshParamsBuffer_ != nullptr) {
        meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
        meshParamsBuffer_->flush(sizeof(meshParams_));
    }

    materialEmissiveCpu_.clear();
    materialEmissiveCpu_.reserve(importedScene.materials.size());
    for (MaterialAssetHandle handle : importedScene.materials) {
        materialEmissiveCpu_.push_back(materialEmissiveLightEstimate(assets.material(handle), assets));
    }
    float emissiveTotalWeight = 0.0f;
    if (rebuildEmissiveLightRecords(importedScene, emissiveTotalWeight)) {
        float lightSelectionWeight = emissiveTotalWeight;
        std::vector<GpuLightRecord> records = combineLightRecords(
            emissiveLightRecords_,
            importedScene.lights,
            emissiveTotalWeight,
            lightSelectionWeight);
        uploadLightRecords(uploader, std::move(records), lightSelectionWeight);
    }
    return true;
}

bool GpuScene::rebuildEmissiveLightRecords(const SceneAsset& scene, float& emissiveTotalWeight) {
    emissiveTotalWeight = 0.0f;
    if (meshRecordCpu_.empty() || instanceRecordCpu_.empty() || localTriangleDataCpu_.empty()) {
        emissiveLightRecords_.clear();
        return false;
    }
    emissiveLightRecords_ = buildLightRecords(
        meshRecordCpu_,
        instanceRecordCpu_,
        localTriangleDataCpu_,
        materialEmissiveCpu_,
        sphereDataCpu_,
        emissiveTotalWeight);
    return !emissiveLightRecords_.empty() || !scene.lights.empty();
}

bool GpuScene::updateSceneLights(
    BufferUploader& uploader,
    const SceneAsset& scene,
    uint64_t retireFrame,
    bool logLightBvhStats,
    bool rebuildLightBvh) {
    if (lightRecords_ == nullptr || meshParamsBuffer_ == nullptr) {
        return false;
    }

    float totalWeight = emissiveLightRecords_.empty() ? 0.0f : emissiveLightRecords_.back().data0.y;
    std::vector<GpuLightRecord> records = combineLightRecords(emissiveLightRecords_, scene.lights, totalWeight, totalWeight);
    uploadLightRecords(uploader, std::move(records), totalWeight, retireFrame, logLightBvhStats, rebuildLightBvh);
    return true;
}

bool GpuScene::updateInstanceTransforms(BufferUploader& uploader, const SceneAsset& scene, const AssetManager& assets, uint64_t retireFrame) {
    if (instanceRecords_ == nullptr || instanceBounds_ == nullptr || tlasNodes_ == nullptr || tlasInstanceIndices_ == nullptr || meshParams_.meshCount == 0) {
        return false;
    }

    std::unordered_map<uint32_t, uint32_t> meshRecordIndexForAsset;
    std::vector<CpuBounds> localMeshBounds;
    std::vector<uint32_t> primitiveOffsets;
    std::vector<uint32_t> primitiveCounts;
    localMeshBounds.reserve(scene.meshes.size());
    primitiveOffsets.reserve(scene.meshes.size());
    primitiveCounts.reserve(scene.meshes.size());

    uint32_t primitiveOffset = 0;
    for (MeshAssetHandle handle : scene.meshes) {
        const MeshAsset* mesh = assets.mesh(handle);
        if (mesh == nullptr || mesh->vertices.empty() || mesh->indices.empty()) {
            continue;
        }
        CpuBounds bounds;
        for (const MeshVertex& vertex : mesh->vertices) {
            includePoint(bounds, vertex.position);
        }
        const uint32_t meshRecordIndex = static_cast<uint32_t>(localMeshBounds.size());
        meshRecordIndexForAsset.emplace(handle.index, meshRecordIndex);
        localMeshBounds.push_back(bounds);
        primitiveOffsets.push_back(primitiveOffset);
        primitiveCounts.push_back(static_cast<uint32_t>(mesh->primitives.size()));
        primitiveOffset += static_cast<uint32_t>(mesh->primitives.size());
    }

    std::vector<GpuInstanceRecord> instanceRecords;
    std::vector<GpuInstanceBoundsRecord> instanceBounds;
    std::vector<RayTracingInstanceBuildInput> rayTracingInstances;
    const std::vector<GpuInstanceRecord> previousInstanceRecords = instanceRecordCpu_;

    auto appendInstance = [&](const glm::mat4& transform, MeshAssetHandle meshHandle, uint32_t flags) {
        const auto recordIt = meshRecordIndexForAsset.find(meshHandle.index);
        if (recordIt == meshRecordIndexForAsset.end()) {
            return;
        }
        const uint32_t meshRecordIndex = recordIt->second;
        const uint32_t instanceIndex = static_cast<uint32_t>(instanceRecords.size());
        const glm::mat4 prevTransform =
            instanceIndex < previousInstanceRecords.size()
                ? previousInstanceRecords[instanceIndex].transform
                : transform;
        instanceRecords.push_back(makeInstanceRecord(
            transform,
            meshRecordIndex,
            primitiveOffsets[meshRecordIndex],
            primitiveCounts[meshRecordIndex],
            flags,
            &prevTransform));
        instanceBounds.push_back(makeInstanceBoundsRecord(transformBounds(localMeshBounds[meshRecordIndex], transform), instanceIndex, meshRecordIndex));
        rayTracingInstances.push_back(RayTracingInstanceBuildInput{
            .instanceIndex = instanceIndex,
            .meshIndex = meshRecordIndex,
            .transform = transform,
            .previousTransform = prevTransform,
            .flags = flags,
            .visible = (flags & instanceFlagVisible) != 0u,
        });
    };

    auto visitNode = [&](auto&& self, uint32_t nodeIndex, glm::mat4 parent) -> void {
        if (nodeIndex >= scene.nodes.size()) {
            return;
        }
        const SceneNodeAsset& node = scene.nodes[nodeIndex];
        const glm::mat4 world = parent * node.transform;
        if (node.mesh.valid()) {
            appendInstance(world, node.mesh, nodeInstanceFlags(node));
        }
        for (uint32_t child : node.children) {
            self(self, child, world);
        }
    };

    if (!scene.rootNodes.empty()) {
        for (uint32_t root : scene.rootNodes) {
            visitNode(visitNode, root, glm::mat4{1.0f});
        }
    } else {
        for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
            if (scene.nodes[i].parent < 0) {
                visitNode(visitNode, i, glm::mat4{1.0f});
            }
        }
    }

    if (instanceRecords.empty() || instanceRecords.size() != meshParams_.instanceCount) {
        return false;
    }

    std::vector<glm::vec4> tlasData;
    std::vector<uint32_t> tlasInstanceIndices;
    buildTlas(instanceBounds, tlasData, tlasInstanceIndices);
    if (tlasData.empty() || tlasInstanceIndices.empty()) {
        return false;
    }

    retireBuffer(uploadVector(allocator_, uploader, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "updated instance records"), retireFrame);
    retireBuffer(uploadVector(allocator_, uploader, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "updated instance bounds"), retireFrame);
    retireBuffer(uploadVector(allocator_, uploader, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasData, "updated tlas nodes"), retireFrame);
    retireBuffer(uploadVector(allocator_, uploader, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasInstanceIndices, "updated tlas instance indices"), retireFrame);

    instanceRecordCpu_ = instanceRecords;
    rayTracingInstances_ = std::move(rayTracingInstances);
    meshParams_.tlasNodeCount = static_cast<uint32_t>(tlasData.size() / 4u);
    meshParams_.tlasInstanceIndexCount = static_cast<uint32_t>(tlasInstanceIndices.size());
    if (meshParamsBuffer_) {
        meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
        meshParamsBuffer_->flush(sizeof(meshParams_));
    }

    float emissiveTotalWeight = 0.0f;
    if (rebuildEmissiveLightRecords(scene, emissiveTotalWeight)) {
        float lightSelectionWeight = emissiveTotalWeight;
        std::vector<GpuLightRecord> records = combineLightRecords(
            emissiveLightRecords_,
            scene.lights,
            emissiveTotalWeight,
            lightSelectionWeight);
        uploadLightRecords(uploader, std::move(records), lightSelectionWeight, retireFrame);
    }
    return true;
}

void GpuScene::createCornellBox(BufferUploader& uploader) {
    createDefaultMaterialTexture(uploader);

    const float s = 1.5f;
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> faceMaterials;
    std::vector<GpuLocalVertex> localVertexData;

    auto pushQuad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, uint32_t material) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
        const glm::vec3 tangent = glm::normalize(b - a);
        vertices.insert(vertices.end(), {a, b, c, d});
        localVertexData.insert(localVertexData.end(), {
            makeLocalVertex(a, normal, {0.0f, 0.0f}, glm::vec4{tangent, 1.0f}),
            makeLocalVertex(b, normal, {1.0f, 0.0f}, glm::vec4{tangent, 1.0f}),
            makeLocalVertex(c, normal, {1.0f, 1.0f}, glm::vec4{tangent, 1.0f}),
            makeLocalVertex(d, normal, {0.0f, 1.0f}, glm::vec4{tangent, 1.0f}),
        });
        indices.insert(indices.end(), {base + 0u, base + 1u, base + 2u, base + 0u, base + 2u, base + 3u});
        faceMaterials.insert(faceMaterials.end(), {material, material});
    };

    // Five inward-facing Cornell-box walls. The front is intentionally open for the camera.
    pushQuad({-s, -s, -s}, { s, -s, -s}, { s,  s, -s}, {-s,  s, -s}, 0); // back
    pushQuad({-s, -s,  s}, {-s, -s, -s}, {-s,  s, -s}, {-s,  s,  s}, 1); // left
    pushQuad({ s, -s, -s}, { s, -s,  s}, { s,  s,  s}, { s,  s, -s}, 2); // right
    pushQuad({-s, -s,  s}, { s, -s,  s}, { s, -s, -s}, {-s, -s, -s}, 0); // floor
    pushQuad({-s,  s, -s}, { s,  s, -s}, { s,  s,  s}, {-s,  s,  s}, 0); // ceiling

    const float lightSize = 0.6f;
    const float lightY = s - 0.01f;
    pushQuad(
        {-lightSize, lightY, -lightSize},
        { lightSize, lightY, -lightSize},
        { lightSize, lightY,  lightSize},
        {-lightSize, lightY,  lightSize},
        3);

    const std::vector<MaterialCpu> mats = {
        {{0.73f, 0.73f, 0.73f}, 1.0f, 1.0f, 0, 0.0f, {0.0f, 0.0f, 0.0f}},
        {{0.63f, 0.06f, 0.05f}, 1.0f, 1.0f, 0, 0.0f, {0.0f, 0.0f, 0.0f}},
        {{0.14f, 0.45f, 0.09f}, 1.0f, 1.0f, 0, 0.0f, {0.0f, 0.0f, 0.0f}},
        {{1.0f, 1.0f, 1.0f}, 1.0f, 1.0f, 0, 0.0f, {15.0f, 13.0f, 10.0f}},
        {{0.86f, 0.95f, 1.0f}, 0.0f, 1.33f, 2, 0.0f, {}, 1.0f},
        {{0.95f, 0.96f, 0.97f}, 0.08f, 1.5f, 3, 1.0f, {}},
        {{0.95f, 0.93f, 0.88f}, 0.12f, 1.5f, 4, 0.0f, {}},
    };
    const std::vector<bool> materialOpaqueTraversalSafe(mats.size(), true);

    auto pushSphereMesh = [&](glm::vec3 center, float radius, uint32_t material) {
        constexpr uint32_t longitude = 160;
        constexpr uint32_t latitude = 80;
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (uint32_t y = 0; y <= latitude; ++y) {
            const float v = static_cast<float>(y) / static_cast<float>(latitude);
            const float phi = v * 3.14159265358979323846f;
            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);
            for (uint32_t x = 0; x <= longitude; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(longitude);
                const float theta = u * 2.0f * 3.14159265358979323846f;
                const glm::vec3 normal{
                    sinPhi * std::cos(theta),
                    cosPhi,
                    sinPhi * std::sin(theta),
                };
                const glm::vec3 tangent = glm::normalize(glm::vec3{-std::sin(theta), 0.0f, std::cos(theta)});
                const glm::vec3 position = center + normal * radius;
                vertices.push_back(position);
                localVertexData.push_back(makeLocalVertex(position, normal, {u, v}, glm::vec4{tangent, 1.0f}));
            }
        }

        const uint32_t rowStride = longitude + 1u;
        for (uint32_t y = 0; y < latitude; ++y) {
            for (uint32_t x = 0; x < longitude; ++x) {
                const uint32_t i0 = base + y * rowStride + x;
                const uint32_t i1 = i0 + 1u;
                const uint32_t i2 = i0 + rowStride;
                const uint32_t i3 = i2 + 1u;
                indices.insert(indices.end(), {i0, i1, i2, i1, i3, i2});
                faceMaterials.insert(faceMaterials.end(), {material, material});
            }
        }
    };

    pushSphereMesh({-0.6f, -1.2f, -0.5f}, 0.3f, 4u);
    pushSphereMesh({ 0.0f, -1.2f, -0.8f}, 0.3f, 5u);
    pushSphereMesh({ 0.6f, -1.2f, -0.5f}, 0.3f, 6u);

    std::vector<glm::vec4> vertexData;
    vertexData.reserve(vertices.size());
    for (glm::vec3 v : vertices) {
        vertexData.push_back({v, 0.0f});
    }
    const std::vector<uint32_t> localIndices = indices;

    const BvhBuildResult bvh = buildBvh(vertices, indices, faceMaterials);
    const std::vector<glm::vec4> bvhData = packBvhNodesForGpu(bvh.packedNodes);
    const std::vector<glm::vec4> triangleData = packTrianglesForGpu(bvh);
    const std::vector<glm::vec4> localBvhData = bvhData;
    const std::vector<glm::vec4> localTriangleData = triangleData;
    std::vector<GpuPrimitiveRecord> primitiveRecords;
    primitiveRecords.reserve(faceMaterials.size());
    for (uint32_t triangle = 0; triangle < static_cast<uint32_t>(faceMaterials.size()); ++triangle) {
        primitiveRecords.push_back(makePrimitiveRecord(
            triangle * 3u,
            3u,
            0u,
            faceMaterials[triangle],
            triangle,
            1u,
            kPrimitiveAlphaClassOpaque,
            faceMaterials[triangle] < materialOpaqueTraversalSafe.size() && materialOpaqueTraversalSafe[faceMaterials[triangle]]));
    }
    const std::vector<GpuMeshRecord> meshRecords = {
        makeMeshRecord(
            0u,
            static_cast<uint32_t>(vertices.size()),
            0u,
            static_cast<uint32_t>(indices.size()),
            0u,
            static_cast<uint32_t>(primitiveRecords.size()),
            0u,
            static_cast<uint32_t>(bvh.packedNodes.size()),
            0u,
            static_cast<uint32_t>(bvh.leafTriangleIndices.size())),
    };
    const std::vector<GpuInstanceRecord> instanceRecords = {
        makeInstanceRecord(glm::mat4{1.0f}, 0u, 0u, static_cast<uint32_t>(primitiveRecords.size())),
    };
    const std::vector<uint32_t> rtTriangleMaterialIds =
        buildRtTriangleMaterialIds(primitiveRecords, static_cast<uint32_t>(localIndices.size() / 3u));
    rayTracingMeshes_.clear();
    rayTracingMeshes_.push_back(RayTracingMeshBuildInput{
        .meshIndex = 0u,
        .firstVertex = 0u,
        .vertexCount = static_cast<uint32_t>(localVertexData.size()),
        .firstIndex = 0u,
        .indexCount = static_cast<uint32_t>(localIndices.size()),
        .primitiveOffset = 0u,
        .primitiveCount = static_cast<uint32_t>(primitiveRecords.size()),
        .containsAlphaTestedGeometry = false,
        .containsBlendedGeometry = false,
        .opaqueTraversalSafe = primitivesAreOpaqueTraversalSafe(
            primitiveRecords,
            0u,
            static_cast<uint32_t>(primitiveRecords.size()),
            materialOpaqueTraversalSafe),
        .updateMode = AccelUpdateMode::Static,
    });
    rayTracingInstances_.clear();
    rayTracingInstances_.push_back(RayTracingInstanceBuildInput{
        .instanceIndex = 0u,
        .meshIndex = 0u,
        .transform = glm::mat4{1.0f},
        .previousTransform = glm::mat4{1.0f},
        .flags = instanceFlagVisible | instanceFlagVisibleToCamera | instanceFlagCastShadow,
        .visible = true,
    });
    const bool fallbackCameraVisible = std::any_of(
        rayTracingInstances_.begin(),
        rayTracingInstances_.end(),
        [](const RayTracingInstanceBuildInput& instance) {
            return instance.visible &&
                (instance.flags & instanceFlagVisible) != 0u &&
                (instance.flags & instanceFlagVisibleToCamera) != 0u;
        });
    if (!fallbackCameraVisible) {
        throw std::runtime_error("Cornell fallback has no camera-visible ray tracing instance");
    }
    const CpuBounds sceneBounds = boundsFromPositions(vertices);
    const std::vector<GpuInstanceBoundsRecord> instanceBounds = {
        makeInstanceBoundsRecord(sceneBounds, 0u, 0u),
    };
    std::vector<glm::vec4> tlasData;
    std::vector<uint32_t> tlasInstanceIndices;
    buildTlas(instanceBounds, tlasData, tlasInstanceIndices);

    std::vector<glm::vec4> materialData;
    std::vector<glm::vec3> materialEmissive;
    materialEmissive.reserve(mats.size());
    for (const auto& m : mats) {
        materialData.push_back({m.color, m.roughness});
        materialData.push_back({m.ior, static_cast<float>(m.type), m.metallic, 0.0f});
        materialData.push_back({m.emissive, 1.0f});
        materialData.push_back({-1.0f, -1.0f, -1.0f, -1.0f});
        materialData.push_back({0.5f, 0.0f, 1.0f, 0.0f});
        appendConductorOptics(materialData, nullptr);
        appendGltfMaterialExtensionData(
            materialData,
            nullptr,
            -1.0f,
            -1.0f,
            -1.0f,
            -1.0f,
            -1.0f,
            -1.0f,
            -1.0f,
            -1.0f,
            m.transmission);
        appendOpacityHeightTextureIndices(materialData);
        appendMaterialTextureTransforms(materialData, nullptr);
        materialEmissive.push_back(m.emissive);
    }
    hasTransmissiveMaterials_ = materialDataContainsTransmission(materialData);

    std::vector<glm::vec4> sphereData;

    float emissiveTotalArea = 0.0f;
    emissiveLightRecords_ = buildLightRecords(meshRecords, instanceRecords, localTriangleData, materialEmissive, sphereData, emissiveTotalArea);
    const std::vector<GpuLightRecord> lightRecords = emissiveLightRecords_;
    lightRecordCpu_ = lightRecords;

    meshParams_ = {
        .vertexCount = static_cast<uint32_t>(vertices.size()),
        .triangleCount = static_cast<uint32_t>(bvh.triangles.size()),
        .bvhNodeCount = static_cast<uint32_t>(bvh.packedNodes.size()),
        .materialCount = static_cast<uint32_t>(mats.size()),
        .enabled = 1,
        .sphereCount = 0,
        .primitiveCount = static_cast<uint32_t>(primitiveRecords.size()),
        .instanceCount = static_cast<uint32_t>(instanceRecords.size()),
        .lightCount = static_cast<uint32_t>(lightRecords.size()),
        .emissiveTotalArea = emissiveTotalArea,
        .meshCount = static_cast<uint32_t>(meshRecords.size()),
        .localVertexCount = static_cast<uint32_t>(localVertexData.size()),
        .localIndexCount = static_cast<uint32_t>(localIndices.size()),
        .localBvhNodeCount = static_cast<uint32_t>(bvh.packedNodes.size()),
        .localTriangleCount = static_cast<uint32_t>(bvh.leafTriangleIndices.size()),
        .tlasNodeCount = static_cast<uint32_t>(tlasData.size() / 4u),
        .tlasInstanceIndexCount = static_cast<uint32_t>(tlasInstanceIndices.size()),
    };
    applyLightRecordMetadataToMeshParams(meshParams_, lightRecords, emissiveTotalArea);
    std::cout << "Cornell scene: vertices=" << meshParams_.vertexCount
              << " triangles=" << meshParams_.triangleCount
              << " bvh_nodes=" << meshParams_.bvhNodeCount
              << " materials=" << meshParams_.materialCount << '\n';
    rayTracingGeometryStats_ = computeRayTracingGeometryStats(meshRecords, primitiveRecords);
    primitiveRecordCpu_ = primitiveRecords;
    localVertexCpu_ = localVertexData;
    meshRecordCpu_ = meshRecords;
    localTriangleDataCpu_ = localTriangleData;
    materialEmissiveCpu_ = materialEmissive;
    sphereDataCpu_ = sphereData;
    logRayTracingGeometryStats("Cornell scene", rayTracingGeometryStats_);
    opacityMicromapData_ = {};

    {
        BatchUploader batch(uploader);
        batch.begin();
        uploadVectorBatched(batch, vertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vertexData, "scene vertices");
        uploadVectorBatched(batch, indices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, indices, "scene indices");
        uploadVectorBatched(batch, bvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bvhData, "scene bvh nodes");
        uploadVectorBatched(batch, triangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triangleData, "scene triangles");
        uploadVectorBatched(batch, materials_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, materialData, "scene materials");
        uploadVectorBatched(batch, spheres_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sphereData, "scene spheres");
        uploadVectorBatched(batch, meshRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, meshRecords, "scene mesh records");
        uploadVectorBatched(batch, primitiveRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, primitiveRecords, "scene primitive records");
        uploadVectorBatched(batch, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "scene instance records");
        uploadVectorBatched(batch, rtTriangleMaterialIds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rtTriangleMaterialIds, "scene rt triangle material ids");
        uploadVectorBatched(batch, lightRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lightRecords, "scene emissive light records");
        uploadVectorBatched(batch, localVertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localVertexData, "scene local mesh vertices");
        uploadVectorBatched(batch, localIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localIndices, "scene local mesh indices");
        uploadVectorBatched(batch, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "scene instance bounds");
        uploadVectorBatched(batch, localBvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localBvhData, "scene local bvh nodes");
        uploadVectorBatched(batch, localTriangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localTriangleData, "scene local bvh triangles");
        uploadVectorBatched(batch, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasData, "scene tlas nodes");
        uploadVectorBatched(batch, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasInstanceIndices, "scene tlas instance indices");
        batch.submit();
    }

    instanceRecordCpu_ = instanceRecords;
    meshParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(MeshParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "mesh params",
    });
    meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
    meshParamsBuffer_->flush(sizeof(meshParams_));
    uploadLightBvh(uploader, lightRecords);
}

void GpuScene::createImportedScene(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets) {
    createImportedMaterialTextures(uploader, importedScene, assets);

    std::vector<glm::vec4> vertexData;
    std::vector<uint32_t> indices;
    std::vector<glm::vec4> bvhData;
    std::vector<glm::vec4> triangleData;
    std::vector<glm::vec4> materialData;
    std::vector<glm::vec3> materialEmissive;
    std::vector<bool> materialOpaqueTraversalSafe;
    std::vector<uint32_t> materialAlphaClasses;
    std::vector<GpuMeshRecord> meshRecords;
    std::vector<GpuPrimitiveRecord> primitiveRecords;
    std::vector<GpuInstanceRecord> instanceRecords;
    std::vector<GpuInstanceBoundsRecord> instanceBounds;
    std::vector<GpuLocalVertex> localVertexData;
    std::vector<uint32_t> localIndices;
    std::vector<glm::vec4> localBvhData;
    std::vector<glm::vec4> localTriangleData;
    std::vector<glm::vec4> tlasData;
    std::vector<uint32_t> tlasInstanceIndices;
    std::vector<CpuBounds> localMeshBounds;

    std::vector<MaterialAssetHandle> materialHandles = importedScene.materials;
    if (materialHandles.empty()) {
        materialHandles.push_back(MaterialAssetHandle{0});
    }
    const std::vector<TextureColorUsage> textureUsage = classifyTextureUsage(importedScene, assets);
    auto textureSlotFor = [&](TextureAssetHandle texture) {
        const uint32_t slot = GpuScene::textureSlotIndexFor(importedScene, texture, maxMaterialTextures);
        return slot == UINT32_MAX ? -1.0f : static_cast<float>(slot);
    };

    struct PreparedMaterialGpuData {
        std::array<glm::vec4, materialVec4Stride> rows{};
        glm::vec3 emissive{0.0f};
        bool opaqueTraversalSafe = false;
        uint32_t alphaClass = kPrimitiveAlphaClassOpaque;
    };
    std::vector<PreparedMaterialGpuData> preparedMaterials(materialHandles.size());
    parallelFor(materialHandles.size(), [&](size_t begin, size_t end) {
        for (size_t materialIndex = begin; materialIndex < end; ++materialIndex) {
            const MaterialAsset* material = assets.material(materialHandles[materialIndex]);
            const glm::vec4 base = effectiveMaterialBaseColor(material, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
            const glm::vec3 emissive = material != nullptr ? material->emissiveFactor : glm::vec3(0.0f);
            const float roughness = material != nullptr ? material->roughnessFactor : 1.0f;
            const float metallic = material != nullptr ? material->metallicFactor : 0.0f;
            const uint32_t type = importedMaterialType(material);
            const float baseColorTexture = material != nullptr ? textureSlotFor(material->baseColorTexture) : -1.0f;
            const float normalTexture = material != nullptr ? textureSlotFor(material->normalTexture) : -1.0f;
            const float metallicRoughnessTexture = material != nullptr ? textureSlotFor(material->metallicRoughnessTexture) : -1.0f;
            const float emissiveTexture = material != nullptr ? textureSlotFor(material->emissiveTexture) : -1.0f;
            const float occlusionTexture = material != nullptr ? textureSlotFor(material->occlusionTexture) : -1.0f;
            const float sheenColorTexture = material != nullptr ? textureSlotFor(material->sheenColorTexture) : -1.0f;
            const float sheenRoughnessTexture = material != nullptr ? textureSlotFor(material->sheenRoughnessTexture) : -1.0f;
            const float iridescenceTexture = material != nullptr ? textureSlotFor(material->iridescenceTexture) : -1.0f;
            const float iridescenceThicknessTexture = material != nullptr ? textureSlotFor(material->iridescenceThicknessTexture) : -1.0f;
            const float clearcoatTexture = material != nullptr ? textureSlotFor(material->clearcoatTexture) : -1.0f;
            const float clearcoatRoughnessTexture = material != nullptr ? textureSlotFor(material->clearcoatRoughnessTexture) : -1.0f;
            const float clearcoatNormalTexture = material != nullptr ? textureSlotFor(material->clearcoatNormalTexture) : -1.0f;
            const float transmissionTexture = material != nullptr ? textureSlotFor(material->transmissionTexture) : -1.0f;
            const float volumeThicknessTexture = material != nullptr ? textureSlotFor(material->volumeThicknessTexture) : -1.0f;
            const float specularTexture = material != nullptr ? textureSlotFor(material->specularTexture) : -1.0f;
            const float specularColorTexture = material != nullptr ? textureSlotFor(material->specularColorTexture) : -1.0f;
            const float anisotropyTexture = material != nullptr ? textureSlotFor(material->anisotropyTexture) : -1.0f;
            const float opacityTexture = material != nullptr ? textureSlotFor(material->opacityTexture) : -1.0f;
            const float heightTexture = material != nullptr ? textureSlotFor(material->heightTexture) : -1.0f;
            uint32_t flags = materialSemanticFlags(material);
            if (material != nullptr) {
                uint32_t slot = GpuScene::textureSlotIndexFor(importedScene, material->baseColorTexture, maxMaterialTextures);
                if (slot != UINT32_MAX && importedMaterialTextureNeedsManualSrgb(assets.texture(material->baseColorTexture), textureUsage, slot)) {
                    flags |= materialFlagManualBaseColorSrgb;
                }
                slot = GpuScene::textureSlotIndexFor(importedScene, material->emissiveTexture, maxMaterialTextures);
                if (slot != UINT32_MAX && importedMaterialTextureNeedsManualSrgb(assets.texture(material->emissiveTexture), textureUsage, slot)) {
                    flags |= materialFlagManualEmissiveSrgb;
                }
            }

            std::vector<glm::vec4> rows;
            rows.reserve(materialVec4Stride);
            rows.push_back({glm::vec3(base), roughness});
            rows.push_back({material != nullptr ? material->iorFactor : 1.5f, static_cast<float>(type), metallic, static_cast<float>(flags)});
            rows.push_back({emissive, base.a});
            rows.push_back({baseColorTexture, normalTexture, metallicRoughnessTexture, emissiveTexture});
            rows.push_back({
                material != nullptr ? material->alphaCutoff : 0.5f,
                material != nullptr ? static_cast<float>(material->alphaMode) : 0.0f,
                material != nullptr ? static_cast<float>(material->doubleSided) : 0.0f,
                0.0f});
            appendConductorOptics(
                rows,
                material,
                occlusionTexture,
                sheenColorTexture,
                sheenRoughnessTexture,
                iridescenceTexture,
                iridescenceThicknessTexture);
            appendGltfMaterialExtensionData(
                rows,
                material,
                clearcoatTexture,
                clearcoatRoughnessTexture,
                clearcoatNormalTexture,
                transmissionTexture,
                volumeThicknessTexture,
                specularTexture,
                specularColorTexture,
                anisotropyTexture,
                effectiveMaterialTransmissionFactor(material));
            appendOpacityHeightTextureIndices(rows, opacityTexture, heightTexture, material != nullptr ? material->heightScale : 0.025f);
            appendMaterialTextureTransforms(rows, material);
            if (rows.size() != materialVec4Stride) {
                throw std::runtime_error("Imported material GPU layout stride mismatch");
            }

            PreparedMaterialGpuData prepared;
            std::copy(rows.begin(), rows.end(), prepared.rows.begin());
            prepared.emissive = materialEmissiveLightEstimate(material, assets);
            prepared.opaqueTraversalSafe = material != nullptr && material->alphaMode == 0u && material->doubleSided != 0u;
            prepared.alphaClass = primitiveAlphaClassForMaterial(material);
            preparedMaterials[materialIndex] = prepared;
        }
    }, 4);
    materialData.reserve(preparedMaterials.size() * materialVec4Stride);
    materialEmissive.reserve(preparedMaterials.size());
    materialOpaqueTraversalSafe.reserve(preparedMaterials.size());
    materialAlphaClasses.reserve(preparedMaterials.size());
    for (const PreparedMaterialGpuData& prepared : preparedMaterials) {
        materialData.insert(materialData.end(), prepared.rows.begin(), prepared.rows.end());
        materialEmissive.push_back(prepared.emissive);
        materialOpaqueTraversalSafe.push_back(prepared.opaqueTraversalSafe);
        materialAlphaClasses.push_back(prepared.alphaClass);
    }
    if (materialData.empty()) {
        materialData.push_back({0.8f, 0.8f, 0.8f, 1.0f});
        materialData.push_back({1.5f, 0.0f, 0.0f, 0.0f});
        materialData.push_back({0.0f, 0.0f, 0.0f, 1.0f});
        materialData.push_back({-1.0f, -1.0f, -1.0f, -1.0f});
        materialData.push_back({0.5f, 0.0f, 0.0f, 0.0f});
        appendConductorOptics(materialData, nullptr);
        appendGltfMaterialExtensionData(materialData, nullptr);
        appendOpacityHeightTextureIndices(materialData);
        appendMaterialTextureTransforms(materialData, nullptr);
        materialEmissive.push_back(glm::vec3(0.0f));
        materialOpaqueTraversalSafe.push_back(false);
        materialAlphaClasses.push_back(kPrimitiveAlphaClassOpaque);
    }
    hasTransmissiveMaterials_ = materialDataContainsTransmission(materialData);

    std::unordered_map<uint32_t, uint32_t> materialIndexForAsset;
    materialIndexForAsset.reserve(materialHandles.size());
    for (uint32_t i = 0; i < materialHandles.size(); ++i) {
        materialIndexForAsset.emplace(materialHandles[i].index, i);
    }

    std::unordered_map<uint32_t, uint32_t> meshRecordIndexForAsset;
    meshRecordIndexForAsset.reserve(importedScene.meshes.size());
    uint64_t importedTriangleCount = 0;
    for (MeshAssetHandle handle : importedScene.meshes) {
        if (const MeshAsset* mesh = assets.mesh(handle)) {
            importedTriangleCount += mesh->indices.size() / 3u;
        }
    }
    const BvhBuildQuality importedBvhQuality = importedTriangleCount >= fastImportedBvhTriangleThreshold
        ? BvhBuildQuality::MortonFast
        : BvhBuildQuality::BinnedSah;

    struct ImportedMeshJob {
        MeshAssetHandle handle;
        const MeshAsset* mesh = nullptr;
        uint32_t firstVertex = 0;
        uint32_t firstIndex = 0;
        uint32_t primitiveOffset = 0;
        uint32_t localTriangleCursor = 0;
    };

    struct MeshPrep {
        MeshAssetHandle handle;
        const MeshAsset* mesh = nullptr;
        uint32_t firstVertex = 0;
        uint32_t firstIndex = 0;
        uint32_t primitiveOffset = 0;
        uint32_t localTriangleCursor = 0;
        uint32_t meshRecordIndex = 0;
        CpuBounds localBounds;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec2> texcoords;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec4> tangents;
        std::vector<uint32_t> faceMaterials;
        std::vector<MeshVertex> morphedVertices;
    };

    std::vector<ImportedMeshJob> meshJobs;
    meshJobs.reserve(importedScene.meshes.size());
    uint32_t vertexCursor = 0;
    uint32_t indexCursor = 0;
    uint32_t primitiveCursor = 0;
    uint32_t localTriangleCursor = 0;
    for (MeshAssetHandle handle : importedScene.meshes) {
        const MeshAsset* mesh = assets.mesh(handle);
        if (mesh == nullptr || mesh->vertices.empty() || mesh->indices.empty()) {
            continue;
        }
        ImportedMeshJob job;
        job.handle = handle;
        job.mesh = mesh;
        job.firstVertex = vertexCursor;
        job.firstIndex = indexCursor;
        job.primitiveOffset = primitiveCursor;
        job.localTriangleCursor = localTriangleCursor;
        uint32_t meshTriangleCount = 0;
        for (const MeshPrimitiveAsset& primitive : mesh->primitives) {
            meshTriangleCount += primitive.indexCount / 3u;
        }
        meshJobs.push_back(job);
        vertexCursor += static_cast<uint32_t>(mesh->vertices.size());
        indexCursor += static_cast<uint32_t>(mesh->indices.size());
        primitiveCursor += static_cast<uint32_t>(mesh->primitives.size());
        localTriangleCursor += meshTriangleCount;
    }

    std::vector<MeshPrep> meshPrep(meshJobs.size());
    localVertexData.resize(vertexCursor);
    localIndices.resize(indexCursor);
    primitiveRecords.resize(primitiveCursor);
    const auto& materialIndexForAssetLookup = materialIndexForAsset;
    parallelFor(meshJobs.size(), [&](size_t begin, size_t end) {
        for (size_t meshJobIndex = begin; meshJobIndex < end; ++meshJobIndex) {
            const ImportedMeshJob& job = meshJobs[meshJobIndex];
            const MeshAsset* mesh = job.mesh;
            MeshPrep prep;
            prep.handle = job.handle;
            prep.mesh = mesh;
            prep.firstVertex = job.firstVertex;
            prep.firstIndex = job.firstIndex;
            prep.primitiveOffset = job.primitiveOffset;
            prep.localTriangleCursor = job.localTriangleCursor;
            prep.meshRecordIndex = static_cast<uint32_t>(meshJobIndex);
            const bool hasUsableCachedLocalBvh = !mesh->cachedLocalBvhNodes.empty() &&
                !mesh->cachedLocalBvhTriangles.empty() &&
                mesh->cachedLocalBvhNodes.size() % 4u == 0u &&
                mesh->cachedLocalBvhTriangles.size() % 12u == 0u;
            if (!hasUsableCachedLocalBvh) {
                prep.positions.reserve(mesh->vertices.size());
                prep.texcoords.reserve(mesh->vertices.size());
                prep.normals.reserve(mesh->vertices.size());
                prep.tangents.reserve(mesh->vertices.size());
            }

            const std::vector<MeshVertex>* sourceVertices = &mesh->vertices;
            if (hasActiveMorphTargetWeights(*mesh, mesh->defaultMorphWeights)) {
                MeshAsset morphedMesh = *mesh;
                applyMorphTargetWeights(morphedMesh, mesh->defaultMorphWeights);
                prep.morphedVertices = std::move(morphedMesh.vertices);
                sourceVertices = &prep.morphedVertices;
            }

            for (size_t vertexIndex = 0; vertexIndex < sourceVertices->size(); ++vertexIndex) {
                const MeshVertex& vertex = (*sourceVertices)[vertexIndex];
                const float normalLen2 = glm::dot(vertex.normal, vertex.normal);
                const glm::vec3 normal = normalLen2 > 1.0e-10f ? glm::normalize(vertex.normal) : glm::vec3{0.0f, 1.0f, 0.0f};
                glm::vec3 tangent = glm::vec3(vertex.tangent);
                const float tangentLen2 = glm::dot(tangent, tangent);
                tangent = tangentLen2 > 1.0e-10f ? glm::normalize(tangent) : glm::vec3{1.0f, 0.0f, 0.0f};
                const glm::vec4 packedTangent{tangent, vertex.tangent.w < 0.0f ? -1.0f : 1.0f};
                localVertexData[static_cast<size_t>(prep.firstVertex) + vertexIndex] =
                    makeLocalVertex(vertex.position, normal, vertex.texcoord, packedTangent, vertex.color, vertex.texcoord1);
                if (!hasUsableCachedLocalBvh) {
                    prep.positions.push_back(vertex.position);
                    prep.texcoords.push_back(vertex.texcoord);
                    prep.normals.push_back(normal);
                    prep.tangents.push_back(packedTangent);
                }
                includePoint(prep.localBounds, vertex.position);
            }

            for (size_t indexOffset = 0; indexOffset < mesh->indices.size(); ++indexOffset) {
                localIndices[static_cast<size_t>(prep.firstIndex) + indexOffset] = prep.firstVertex + mesh->indices[indexOffset];
            }

            uint32_t localTriangleWriteCursor = prep.localTriangleCursor;
            for (size_t primitiveIndex = 0; primitiveIndex < mesh->primitives.size(); ++primitiveIndex) {
                const MeshPrimitiveAsset& primitive = mesh->primitives[primitiveIndex];
                const uint32_t triangleCount = primitive.indexCount / 3u;
                const auto materialIt = materialIndexForAssetLookup.find(primitive.material.index);
                const uint32_t materialIndex = materialIt != materialIndexForAssetLookup.end() ? materialIt->second : 0u;
                const uint32_t alphaClass = materialIndex < materialAlphaClasses.size()
                    ? materialAlphaClasses[materialIndex]
                    : kPrimitiveAlphaClassOpaque;
                if (!hasUsableCachedLocalBvh) {
                    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
                        prep.faceMaterials.push_back(materialIndex);
                    }
                }
                primitiveRecords[static_cast<size_t>(prep.primitiveOffset) + primitiveIndex] = makePrimitiveRecord(
                    prep.firstIndex + primitive.firstIndex,
                    primitive.indexCount,
                    prep.firstVertex + primitive.firstVertex,
                    materialIndex,
                    localTriangleWriteCursor,
                    triangleCount,
                    alphaClass,
                    materialIndex < materialOpaqueTraversalSafe.size() && materialOpaqueTraversalSafe[materialIndex]);
                localTriangleWriteCursor += triangleCount;
            }
            meshPrep[meshJobIndex] = std::move(prep);
        }
    }, 1);

    std::vector<ParallelBvhBuildTask> bvhTasks;
    bvhTasks.reserve(meshPrep.size());
    std::vector<int32_t> bvhTaskIndexForMesh(meshPrep.size(), -1);
    for (const auto& prep : meshPrep) {
        const bool hasUsableCachedLocalBvh = !prep.mesh->cachedLocalBvhNodes.empty() &&
            !prep.mesh->cachedLocalBvhTriangles.empty() &&
            prep.mesh->cachedLocalBvhNodes.size() % 4u == 0u &&
            prep.mesh->cachedLocalBvhTriangles.size() % 12u == 0u;
        if (hasUsableCachedLocalBvh) {
            continue;
        }
        ParallelBvhBuildTask task;
        task.vertices = &prep.positions;
        task.indices = &prep.mesh->indices;
        task.faceMaterials = &prep.faceMaterials;
        task.texcoords = &prep.texcoords;
        task.normals = &prep.normals;
        task.tangents = &prep.tangents;
        task.quality = importedBvhQuality;
        bvhTaskIndexForMesh[&prep - meshPrep.data()] = static_cast<int32_t>(bvhTasks.size());
        bvhTasks.push_back(task);
    }

    const auto bvhResults = ParallelBvhBuilder::buildAll(bvhTasks);
    if (importedBvhQuality == BvhBuildQuality::MortonFast) {
        if (bvhTasks.empty()) {
            std::cout << "Large imported geometry detected; reusing cached local BVHs for "
                      << meshPrep.size() << " meshes (" << importedTriangleCount
                      << " triangles), no CPU local BVH rebuild needed.\n";
        } else {
            std::cout << "Large imported geometry detected; building missing local BVHs with fast Morton for "
                      << bvhTasks.size() << " / " << meshPrep.size()
                      << " meshes (" << importedTriangleCount << " triangles total).\n";
        }
    }
    if (!bvhTasks.empty() && bvhTasks.size() != meshPrep.size()) {
        std::cout << "Imported scene reused cached local BVHs for " << (meshPrep.size() - bvhTasks.size())
                  << " / " << meshPrep.size() << " meshes.\n";
    }

    for (size_t i = 0; i < meshPrep.size(); ++i) {
        const auto& prep = meshPrep[i];

        const uint32_t localBvhNodeOffset = static_cast<uint32_t>(localBvhData.size() / 4u);
        const uint32_t localTriangleOffset = static_cast<uint32_t>(localTriangleData.size() / 12u);

        uint32_t localBvhNodeCount = 0;
        uint32_t localTriangleCount = 0;
        const bool hasUsableCachedLocalBvh = !prep.mesh->cachedLocalBvhNodes.empty() &&
            !prep.mesh->cachedLocalBvhTriangles.empty() &&
            prep.mesh->cachedLocalBvhNodes.size() % 4u == 0u &&
            prep.mesh->cachedLocalBvhTriangles.size() % 12u == 0u;
        if (hasUsableCachedLocalBvh) {
            localBvhNodeCount = static_cast<uint32_t>(prep.mesh->cachedLocalBvhNodes.size() / 4u);
            localTriangleCount = static_cast<uint32_t>(prep.mesh->cachedLocalBvhTriangles.size() / 12u);
            localBvhData.insert(localBvhData.end(), prep.mesh->cachedLocalBvhNodes.begin(), prep.mesh->cachedLocalBvhNodes.end());
            localTriangleData.insert(localTriangleData.end(), prep.mesh->cachedLocalBvhTriangles.begin(), prep.mesh->cachedLocalBvhTriangles.end());
        } else {
            const int32_t bvhTaskIndex = bvhTaskIndexForMesh[i];
            const BvhBuildResult& localBvh = bvhResults[static_cast<size_t>(bvhTaskIndex)].bvh;
            localBvhNodeCount = static_cast<uint32_t>(localBvh.packedNodes.size());
            localTriangleCount = static_cast<uint32_t>(localBvh.leafTriangleIndices.size());
            appendBvhNodesForGpu(localBvh.packedNodes, localBvhData);
            appendTrianglesForGpu(localBvh, localTriangleData);
        }

        const uint32_t meshRecordIndex = static_cast<uint32_t>(meshRecords.size());
        meshRecords.push_back(makeMeshRecord(
            prep.firstVertex,
            static_cast<uint32_t>(prep.mesh->vertices.size()),
            prep.firstIndex,
            static_cast<uint32_t>(prep.mesh->indices.size()),
            prep.primitiveOffset,
            static_cast<uint32_t>(prep.mesh->primitives.size()),
            localBvhNodeOffset,
            localBvhNodeCount,
            localTriangleOffset,
            localTriangleCount));
        rayTracingMeshes_.push_back(RayTracingMeshBuildInput{
            .meshIndex = meshRecordIndex,
            .sourceMeshHandleIndex = prep.handle.index,
            .firstVertex = prep.firstVertex,
            .vertexCount = static_cast<uint32_t>(prep.mesh->vertices.size()),
            .firstIndex = prep.firstIndex,
            .indexCount = static_cast<uint32_t>(prep.mesh->indices.size()),
            .primitiveOffset = prep.primitiveOffset,
            .primitiveCount = static_cast<uint32_t>(prep.mesh->primitives.size()),
            .containsAlphaTestedGeometry = meshPrimitiveAlphaClass(
                primitiveRecords,
                prep.primitiveOffset,
                static_cast<uint32_t>(prep.mesh->primitives.size()),
                kPrimitiveAlphaClassAlphaTested) != 0u,
            .containsBlendedGeometry = meshPrimitiveAlphaClass(
                primitiveRecords,
                prep.primitiveOffset,
                static_cast<uint32_t>(prep.mesh->primitives.size()),
                kPrimitiveAlphaClassBlended) != 0u,
            .opaqueTraversalSafe = primitivesAreOpaqueTraversalSafe(
                primitiveRecords,
                prep.primitiveOffset,
                static_cast<uint32_t>(prep.mesh->primitives.size()),
                materialOpaqueTraversalSafe),
            .updateMode = AccelUpdateMode::Static,
        });
        localMeshBounds.push_back(prep.localBounds);
        meshRecordIndexForAsset.emplace(prep.handle.index, meshRecordIndex);
    }

    struct PendingImportedInstance {
        glm::mat4 transform{1.0f};
        uint32_t meshRecordIndex = 0;
        uint32_t flags = instanceFlagVisible | instanceFlagVisibleToCamera | instanceFlagCastShadow;
    };
    std::vector<PendingImportedInstance> pendingInstances;
    pendingInstances.reserve(importedScene.nodes.size());

    auto appendInstance = [&](const glm::mat4& transform, uint32_t meshIndex, uint32_t flags) {
        auto recordIt = meshRecordIndexForAsset.find(meshIndex);
        if (recordIt == meshRecordIndexForAsset.end()) {
            return;
        }
        const uint32_t meshRecordIndex = recordIt->second;
        if (meshRecordIndex >= meshRecords.size() || meshRecordIndex >= localMeshBounds.size()) {
            return;
        }
        pendingInstances.push_back(PendingImportedInstance{
            .transform = transform,
            .meshRecordIndex = meshRecordIndex,
            .flags = flags,
        });
    };

    auto visitNode = [&](auto&& self, uint32_t nodeIndex, glm::mat4 parent) -> void {
        if (nodeIndex >= importedScene.nodes.size()) {
            return;
        }
        const SceneNodeAsset& node = importedScene.nodes[nodeIndex];
        const glm::mat4 world = parent * node.transform;
        if (node.mesh.valid()) {
            appendInstance(world, node.mesh.index, nodeInstanceFlags(node));
        }
        for (uint32_t child : node.children) {
            self(self, child, world);
        }
    };
    if (!importedScene.rootNodes.empty()) {
        for (uint32_t root : importedScene.rootNodes) {
            visitNode(visitNode, root, glm::mat4{1.0f});
        }
    } else {
        for (uint32_t i = 0; i < importedScene.nodes.size(); ++i) {
            if (importedScene.nodes[i].parent < 0) {
                visitNode(visitNode, i, glm::mat4{1.0f});
            }
        }
    }

    instanceRecords.resize(pendingInstances.size());
    instanceBounds.resize(pendingInstances.size());
    rayTracingInstances_.resize(pendingInstances.size());
    parallelFor(pendingInstances.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const PendingImportedInstance& pending = pendingInstances[i];
            const GpuMeshRecord& meshRecord = meshRecords[pending.meshRecordIndex];
            const uint32_t primitiveOffset = meshRecord.primitiveData.x;
            const uint32_t primitiveCount = meshRecord.primitiveData.y;
            const uint32_t instanceIndex = static_cast<uint32_t>(i);
            instanceRecords[i] = makeInstanceRecord(
                pending.transform,
                pending.meshRecordIndex,
                primitiveOffset,
                primitiveCount,
                pending.flags);
            rayTracingInstances_[i] = RayTracingInstanceBuildInput{
                .instanceIndex = instanceIndex,
                .meshIndex = pending.meshRecordIndex,
                .transform = pending.transform,
                .previousTransform = pending.transform,
                .flags = pending.flags,
                .visible = (pending.flags & instanceFlagVisible) != 0u,
            };
            instanceBounds[i] = makeInstanceBoundsRecord(
                transformBounds(localMeshBounds[pending.meshRecordIndex], pending.transform),
                instanceIndex,
                pending.meshRecordIndex);
        }
    }, 16);

    if (instanceRecords.empty() || localVertexData.empty() || localIndices.empty() || localBvhData.empty() || localTriangleData.empty()) {
        createCornellBox(uploader);
        return;
    }

    const std::vector<uint32_t> rtTriangleMaterialIds =
        buildRtTriangleMaterialIds(primitiveRecords, static_cast<uint32_t>(localIndices.size() / 3u));
    buildTlas(instanceBounds, tlasData, tlasInstanceIndices);

    std::vector<glm::vec4> sphereData;
    float emissiveTotalArea = 0.0f;
    emissiveLightRecords_ = buildLightRecords(meshRecords, instanceRecords, localTriangleData, materialEmissive, sphereData, emissiveTotalArea);
    float lightSelectionWeight = emissiveTotalArea;
    const std::vector<GpuLightRecord> lightRecords = combineLightRecords(emissiveLightRecords_, importedScene.lights, emissiveTotalArea, lightSelectionWeight);
    lightRecordCpu_ = lightRecords;
    meshParams_ = {
        .vertexCount = static_cast<uint32_t>(localVertexData.size()),
        .triangleCount = localTriangleCursor,
        .bvhNodeCount = 0,
        .materialCount = static_cast<uint32_t>(materialData.size() / materialVec4Stride),
        .enabled = 1,
        .sphereCount = 0,
        .primitiveCount = static_cast<uint32_t>(primitiveRecords.size()),
        .instanceCount = static_cast<uint32_t>(instanceRecords.size()),
        .lightCount = static_cast<uint32_t>(lightRecords.size()),
        .emissiveTotalArea = lightSelectionWeight,
        .meshCount = static_cast<uint32_t>(meshRecords.size()),
        .localVertexCount = static_cast<uint32_t>(localVertexData.size()),
        .localIndexCount = static_cast<uint32_t>(localIndices.size()),
        .localBvhNodeCount = static_cast<uint32_t>(localBvhData.size() / 4u),
        .localTriangleCount = static_cast<uint32_t>(localTriangleData.size() / 12u),
        .tlasNodeCount = static_cast<uint32_t>(tlasData.size() / 4u),
        .tlasInstanceIndexCount = static_cast<uint32_t>(tlasInstanceIndices.size()),
    };
    applyLightRecordMetadataToMeshParams(meshParams_, lightRecords, lightSelectionWeight);
    std::cout << "Imported scene GPU data: meshes=" << meshParams_.meshCount
              << " instances=" << meshParams_.instanceCount
              << " local_triangles=" << meshParams_.localTriangleCount
              << " local_bvh_nodes=" << meshParams_.localBvhNodeCount
              << " tlas_nodes=" << meshParams_.tlasNodeCount << '\n';
    rayTracingGeometryStats_ = computeRayTracingGeometryStats(meshRecords, primitiveRecords);
    primitiveRecordCpu_ = primitiveRecords;
    localVertexCpu_ = localVertexData;
    meshRecordCpu_ = meshRecords;
    localTriangleDataCpu_ = localTriangleData;
    materialEmissiveCpu_ = materialEmissive;
    sphereDataCpu_ = sphereData;
    logRayTracingGeometryStats("Imported scene", rayTracingGeometryStats_);
    if (opacityMicromapsEnabled_) {
        opacityMicromapData_ = generateOpacityMicromapData(importedScene, assets, opacityMicromapSubdivisionLevel_);
        logOpacityMicromapPreprocessStats("Imported scene", opacityMicromapData_.stats);
    } else {
        opacityMicromapData_ = {};
        opacityMicromapData_.stats.subdivisionLevel = opacityMicromapSubdivisionLevel_;
        std::cout << "Imported scene OMM preprocess skipped: opacity micromaps disabled.\n";
    }

    {
        BatchUploader batch(uploader);
        batch.begin();
        uploadVectorBatched(batch, vertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vertexData, "imported scene vertices");
        uploadVectorBatched(batch, indices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, indices, "imported scene indices");
        uploadVectorBatched(batch, bvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bvhData, "imported scene bvh nodes");
        uploadVectorBatched(batch, triangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triangleData, "imported scene triangles");
        uploadVectorBatched(batch, materials_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, materialData, "imported scene materials");
        uploadVectorBatched(batch, spheres_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sphereData, "imported scene spheres");
        uploadVectorBatched(batch, meshRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, meshRecords, "imported mesh records");
        uploadVectorBatched(batch, primitiveRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, primitiveRecords, "imported primitive records");
        uploadVectorBatched(batch, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "imported instance records");
        uploadVectorBatched(batch, rtTriangleMaterialIds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rtTriangleMaterialIds, "imported rt triangle material ids");
        uploadVectorBatched(batch, lightRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lightRecords, "imported emissive light records");
        uploadVectorBatched(batch, localVertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localVertexData, "imported local mesh vertices");
        uploadVectorBatched(batch, localIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localIndices, "imported local mesh indices");
        uploadVectorBatched(batch, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "imported instance bounds");
        uploadVectorBatched(batch, localBvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localBvhData, "imported local bvh nodes");
        uploadVectorBatched(batch, localTriangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localTriangleData, "imported local bvh triangles");
        uploadVectorBatched(batch, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasData, "imported tlas nodes");
        uploadVectorBatched(batch, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasInstanceIndices, "imported tlas instance indices");
        batch.submit();
    }

    instanceRecordCpu_ = instanceRecords;
    meshParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(MeshParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "imported mesh params",
    });
    meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
    meshParamsBuffer_->flush(sizeof(meshParams_));
    uploadLightBvh(uploader, lightRecords);

    if (sceneCachePolicy_.canWrite() && !importedScene.sourcePath.empty() && importedScene.lights.empty()) {
        CachedScene gpuCached;
        gpuCached.name = importedScene.name;
        gpuCached.sourceMtime = SceneCache::fileMtime(importedScene.sourcePath);
        std::filesystem::path binPath = importedScene.sourcePath.parent_path() / (importedScene.sourcePath.stem().string() + ".bin");
        gpuCached.sourceBinMtime = SceneCache::fileMtime(binPath);

        auto getTextureIndex = [&](TextureAssetHandle handle) -> int32_t {
            if (!handle.valid()) return -1;
            for (int32_t i = 0; i < static_cast<int32_t>(importedScene.textures.size()); ++i) {
                if (importedScene.textures[static_cast<size_t>(i)].index == handle.index) return i;
            }
            return -1;
        };
        auto getMaterialIndex = [&](MaterialAssetHandle handle) -> int32_t {
            if (!handle.valid()) return -1;
            for (int32_t i = 0; i < static_cast<int32_t>(importedScene.materials.size()); ++i) {
                if (importedScene.materials[static_cast<size_t>(i)].index == handle.index) return i;
            }
            return -1;
        };
        auto getMeshIndex = [&](MeshAssetHandle handle) -> int32_t {
            if (!handle.valid()) return -1;
            for (int32_t i = 0; i < static_cast<int32_t>(importedScene.meshes.size()); ++i) {
                if (importedScene.meshes[static_cast<size_t>(i)].index == handle.index) return i;
            }
            return -1;
        };

        for (TextureAssetHandle handle : importedScene.textures) {
            const TextureAsset* texture = assets.texture(handle);
            if (texture == nullptr) continue;
            CachedTextureData cachedTex;
            cachedTex.name = texture->name;
            cachedTex.sourcePath = texture->sourcePath.string();
            cachedTex.width = texture->width;
            cachedTex.height = texture->height;
            cachedTex.channels = texture->channels;
            cachedTex.sourceArrayLayers = texture->sourceArrayLayers;
            cachedTex.sourceDepth = texture->sourceDepth;
            cachedTex.sourceFaceCount = texture->sourceFaceCount;
            cachedTex.sourceIsCubemap = texture->sourceIsCubemap;
            cachedTex.mipLevels = texture->mipLevels;
            cachedTex.srgb = texture->srgb;
            cachedTex.fallback = texture->fallback;
            cachedTex.isCompressed = texture->isCompressed;
            cachedTex.linearColorSpace = texture->linearColorSpace;
            cachedTex.format = static_cast<uint32_t>(texture->format);
            cachedTex.compressedFormat = static_cast<uint32_t>(texture->compressedFormat);
            cachedTex.rgba8 = texture->rgba8;
            cachedTex.mipData = texture->mipData;
            cachedTex.minFilter = static_cast<uint32_t>(texture->sampler.minFilter);
            cachedTex.magFilter = static_cast<uint32_t>(texture->sampler.magFilter);
            cachedTex.wrapS = static_cast<uint32_t>(texture->sampler.wrapS);
            cachedTex.wrapT = static_cast<uint32_t>(texture->sampler.wrapT);
            gpuCached.textures.push_back(std::move(cachedTex));
        }
        std::unordered_set<std::string> dependencyPaths;
        for (TextureAssetHandle handle : importedScene.textures) {
            const TextureAsset* texture = assets.texture(handle);
            if (texture == nullptr || texture->sourcePath.empty() || texture->sourcePath == importedScene.sourcePath) {
                continue;
            }
            addFileDependency(gpuCached, texture->sourcePath, dependencyPaths);
        }

        for (MaterialAssetHandle handle : importedScene.materials) {
            const MaterialAsset* material = assets.material(handle);
            if (material == nullptr) continue;
            CachedMaterialData cachedMat;
            cachedMat.name = material->name;
            cachedMat.baseColorFactor = material->baseColorFactor;
            cachedMat.emissiveFactor = material->emissiveFactor;
            cachedMat.metallicFactor = material->metallicFactor;
            cachedMat.roughnessFactor = material->roughnessFactor;
            cachedMat.iorFactor = material->iorFactor;
            cachedMat.alphaCutoff = material->alphaCutoff;
            cachedMat.alphaMode = material->alphaMode;
            cachedMat.doubleSided = material->doubleSided;
            cachedMat.hasIor = material->hasIor;
            cachedMat.hasClearcoat = material->hasClearcoat;
            cachedMat.clearcoatFactor = material->clearcoatFactor;
            cachedMat.clearcoatRoughnessFactor = material->clearcoatRoughnessFactor;
            cachedMat.hasTransmission = material->hasTransmission;
            cachedMat.transmissionFactor = material->transmissionFactor;
            cachedMat.hasSpecular = material->hasSpecular;
            cachedMat.specularFactor = material->specularFactor;
            cachedMat.specularColorFactor = material->specularColorFactor;
            cachedMat.hasSheen = material->hasSheen;
            cachedMat.sheenColorFactor = material->sheenColorFactor;
            cachedMat.sheenRoughnessFactor = material->sheenRoughnessFactor;
            cachedMat.hasIridescence = material->hasIridescence;
            cachedMat.iridescenceFactor = material->iridescenceFactor;
            cachedMat.iridescenceIor = material->iridescenceIor;
            cachedMat.iridescenceThicknessMinimum = material->iridescenceThicknessMinimum;
            cachedMat.iridescenceThicknessMaximum = material->iridescenceThicknessMaximum;
            cachedMat.hasEmissiveStrength = material->hasEmissiveStrength;
            cachedMat.emissiveStrength = material->emissiveStrength;
            cachedMat.hasAnisotropy = material->hasAnisotropy;
            cachedMat.anisotropyStrength = material->anisotropyStrength;
            cachedMat.anisotropyRotation = material->anisotropyRotation;
            cachedMat.hasVolume = material->hasVolume;
            cachedMat.volumeThicknessFactor = material->volumeThicknessFactor;
            cachedMat.volumeAttenuationDistance = material->volumeAttenuationDistance;
            cachedMat.volumeAttenuationColor = material->volumeAttenuationColor;
            cachedMat.nestedPriority = material->nestedPriority;
            cachedMat.hasDispersion = material->hasDispersion;
            cachedMat.dispersionFactor = material->dispersionFactor;
            cachedMat.occlusionStrength = material->occlusionStrength;
            cachedMat.useConductorOptics = material->useConductorOptics;
            cachedMat.conductorEta = material->conductorEta;
            cachedMat.conductorK = material->conductorK;
            cachedMat.baseColorTextureIndex = getTextureIndex(material->baseColorTexture);
            cachedMat.normalTextureIndex = getTextureIndex(material->normalTexture);
            cachedMat.metallicRoughnessTextureIndex = getTextureIndex(material->metallicRoughnessTexture);
            cachedMat.emissiveTextureIndex = getTextureIndex(material->emissiveTexture);
            cachedMat.clearcoatTextureIndex = getTextureIndex(material->clearcoatTexture);
            cachedMat.clearcoatRoughnessTextureIndex = getTextureIndex(material->clearcoatRoughnessTexture);
            cachedMat.clearcoatNormalTextureIndex = getTextureIndex(material->clearcoatNormalTexture);
            cachedMat.transmissionTextureIndex = getTextureIndex(material->transmissionTexture);
            cachedMat.volumeThicknessTextureIndex = getTextureIndex(material->volumeThicknessTexture);
            cachedMat.specularTextureIndex = getTextureIndex(material->specularTexture);
            cachedMat.specularColorTextureIndex = getTextureIndex(material->specularColorTexture);
            cachedMat.sheenColorTextureIndex = getTextureIndex(material->sheenColorTexture);
            cachedMat.sheenRoughnessTextureIndex = getTextureIndex(material->sheenRoughnessTexture);
            cachedMat.iridescenceTextureIndex = getTextureIndex(material->iridescenceTexture);
            cachedMat.iridescenceThicknessTextureIndex = getTextureIndex(material->iridescenceThicknessTexture);
            cachedMat.anisotropyTextureIndex = getTextureIndex(material->anisotropyTexture);
            cachedMat.occlusionTextureIndex = getTextureIndex(material->occlusionTexture);
            cachedMat.opacityTextureIndex = getTextureIndex(material->opacityTexture);
            cachedMat.heightTextureIndex = getTextureIndex(material->heightTexture);
            cachedMat.heightScale = material->heightScale;
            cachedMat.baseColorTextureTransform = material->baseColorTextureTransform;
            cachedMat.metallicRoughnessTextureTransform = material->metallicRoughnessTextureTransform;
            cachedMat.normalTextureTransform = material->normalTextureTransform;
            cachedMat.emissiveTextureTransform = material->emissiveTextureTransform;
            cachedMat.occlusionTextureTransform = material->occlusionTextureTransform;
            cachedMat.clearcoatTextureTransform = material->clearcoatTextureTransform;
            cachedMat.clearcoatRoughnessTextureTransform = material->clearcoatRoughnessTextureTransform;
            cachedMat.clearcoatNormalTextureTransform = material->clearcoatNormalTextureTransform;
            cachedMat.transmissionTextureTransform = material->transmissionTextureTransform;
            cachedMat.volumeThicknessTextureTransform = material->volumeThicknessTextureTransform;
            cachedMat.specularTextureTransform = material->specularTextureTransform;
            cachedMat.specularColorTextureTransform = material->specularColorTextureTransform;
            cachedMat.sheenColorTextureTransform = material->sheenColorTextureTransform;
            cachedMat.sheenRoughnessTextureTransform = material->sheenRoughnessTextureTransform;
            cachedMat.iridescenceTextureTransform = material->iridescenceTextureTransform;
            cachedMat.iridescenceThicknessTextureTransform = material->iridescenceThicknessTextureTransform;
            cachedMat.anisotropyTextureTransform = material->anisotropyTextureTransform;
            cachedMat.materialWorkflow = material->materialWorkflow;
            cachedMat.normalMapConvention = material->normalMapConvention;
            cachedMat.specularTextureAlphaMode = material->specularTextureAlphaMode;
            cachedMat.shaderCompatibilityMask = material->shaderCompatibilityMask;
            gpuCached.materials.push_back(std::move(cachedMat));
        }

        for (size_t meshPrepIndex = 0; meshPrepIndex < meshPrep.size(); ++meshPrepIndex) {
            const auto& prep = meshPrep[meshPrepIndex];
            CachedMeshData cachedMesh;
            cachedMesh.name = prep.mesh->name;
            cachedMesh.vertices = prep.mesh->vertices;
            cachedMesh.indices = prep.mesh->indices;
            cachedMesh.defaultMorphWeights = prep.mesh->defaultMorphWeights;
            for (const MeshPrimitiveAsset& prim : prep.mesh->primitives) {
                CachedPrimitiveData cachedPrim;
                cachedPrim.firstVertex = prim.firstVertex;
                cachedPrim.vertexCount = prim.vertexCount;
                cachedPrim.firstIndex = prim.firstIndex;
                cachedPrim.indexCount = prim.indexCount;
                cachedPrim.materialIndex = getMaterialIndex(prim.material);
                cachedPrim.morphTargets = prim.morphTargets;
                for (const auto& variant : prim.materialVariants) {
                    cachedPrim.materialVariants.push_back(CachedPrimitiveData::MaterialVariant{
                        .variantIndex = variant.variantIndex,
                        .variantName = variant.variantName,
                        .materialIndex = getMaterialIndex(variant.material),
                    });
                }
                cachedMesh.primitives.push_back(std::move(cachedPrim));
            }
            gpuCached.meshes.push_back(std::move(cachedMesh));

            CachedMeshGpuRecord gpuRec;
            const auto& rec = meshRecords[meshPrepIndex];
            gpuRec.vertexIndexData = rec.vertexIndexData;
            gpuRec.primitiveData = rec.primitiveData;
            gpuRec.bvhData = rec.bvhData;
            gpuRec.flags = rec.flags;
            const bool hasUsableCachedLocalBvh = !prep.mesh->cachedLocalBvhNodes.empty() &&
                !prep.mesh->cachedLocalBvhTriangles.empty() &&
                prep.mesh->cachedLocalBvhNodes.size() % 4u == 0u &&
                prep.mesh->cachedLocalBvhTriangles.size() % 12u == 0u;
            if (hasUsableCachedLocalBvh) {
                gpuRec.localBvh.packedNodes = prep.mesh->cachedLocalBvhNodes;
                gpuRec.localBvh.triangleData = prep.mesh->cachedLocalBvhTriangles;
                gpuRec.localBvh.triangleCount = static_cast<uint32_t>(prep.mesh->indices.size() / 3u);
                gpuRec.localBvh.leafTriangleCount = static_cast<uint32_t>(prep.mesh->cachedLocalBvhTriangles.size() / 12u);
            } else {
                const int32_t bvhTaskIndex = bvhTaskIndexForMesh[meshPrepIndex];
                if (bvhTaskIndex >= 0 && static_cast<size_t>(bvhTaskIndex) < bvhResults.size()) {
                    const BvhBuildResult& localBvh = bvhResults[static_cast<size_t>(bvhTaskIndex)].bvh;
                    gpuRec.localBvh.packedNodes = packBvhNodesForGpu(localBvh.packedNodes);
                    gpuRec.localBvh.triangleData = packTrianglesForGpu(localBvh);
                    gpuRec.localBvh.triangleCount = static_cast<uint32_t>(localBvh.triangles.size());
                    gpuRec.localBvh.leafTriangleCount = static_cast<uint32_t>(localBvh.leafTriangleIndices.size());
                }
            }
            gpuCached.meshGpuRecords.push_back(std::move(gpuRec));
        }

        for (size_t i = 0; i < importedScene.nodes.size(); ++i) {
            const SceneNodeAsset& node = importedScene.nodes[i];
            CachedNodeData cachedNode;
            cachedNode.name = node.name;
            cachedNode.transform = node.transform;
            cachedNode.meshIndex = getMeshIndex(node.mesh);
            cachedNode.morphWeights = node.morphWeights;
            cachedNode.skinIndex = node.skinIndex;
            cachedNode.hasCamera = node.hasCamera ? 1u : 0u;
            cachedNode.cameraProjection = node.cameraProjection;
            cachedNode.cameraYfov = node.cameraYfov;
            cachedNode.cameraAspectRatio = node.cameraAspectRatio;
            cachedNode.cameraOrthoXmag = node.cameraOrthoXmag;
            cachedNode.cameraOrthoYmag = node.cameraOrthoYmag;
            cachedNode.cameraNear = node.cameraNear;
            cachedNode.cameraFar = node.cameraFar;
            cachedNode.parentIndex = node.parent;
            cachedNode.children = node.children;
            gpuCached.nodes.push_back(std::move(cachedNode));
        }
        gpuCached.skins.reserve(importedScene.skins.size());
        for (const SceneSkinAsset& skin : importedScene.skins) {
            gpuCached.skins.push_back(CachedSkinData{
                .name = skin.name,
                .skeletonRoot = skin.skeletonRoot,
                .joints = skin.joints,
                .inverseBindMatrices = skin.inverseBindMatrices,
            });
        }
        if (!importedScene.lights.empty()) {
            appendCachedSceneLights(gpuCached, importedScene.lights);
        } else if (sceneCachePolicy_.path.has_value() &&
                   SceneCache::isCacheValid(importedScene.sourcePath, *sceneCachePolicy_.path)) {
            auto sourceCached = SceneCache::load(*sceneCachePolicy_.path);
            if (sourceCached.has_value() && !sourceCached->sceneLights.empty()) {
                gpuCached.sceneLights = sourceCached->sceneLights;
                std::cout << "GPU scene cache preserved " << gpuCached.sceneLights.size()
                          << " source scene light(s) from source cache metadata.\n";
            }
        }
        gpuCached.materialVariants = importedScene.materialVariants;
        gpuCached.rootNodes = importedScene.rootNodes;

        for (const auto& prim : primitiveRecords) {
            CachedPrimitiveRecord cachedPrim;
            cachedPrim.indexData = prim.indexData;
            cachedPrim.metadata = prim.metadata;
            gpuCached.primitiveRecords.push_back(cachedPrim);
        }
        for (const auto& inst : instanceRecords) {
            CachedInstanceRecord cachedInst;
            cachedInst.transform = inst.transform;
            cachedInst.inverseTransform = inst.inverseTransform;
            cachedInst.metadata = inst.metadata;
            gpuCached.instanceRecords.push_back(cachedInst);
        }
        for (const auto& bounds : instanceBounds) {
            CachedInstanceBoundsRecord cachedBounds;
            cachedBounds.bmin = bounds.bmin;
            cachedBounds.bmax = bounds.bmax;
            gpuCached.instanceBounds.push_back(cachedBounds);
        }
        gpuCached.tlasNodes = tlasData;
        gpuCached.tlasInstanceIndices = tlasInstanceIndices;
        for (const auto& light : lightRecords) {
            CachedLightRecord cachedLight;
            cachedLight.metadata = light.metadata;
            cachedLight.identity = light.identity;
            cachedLight.data0 = light.data0;
            cachedLight.data1 = light.data1;
            cachedLight.data2 = light.data2;
            cachedLight.data3 = light.data3;
            gpuCached.lightRecords.push_back(cachedLight);
        }
        gpuCached.meshParams = toCachedMeshParams(meshParams_);

        if (SceneCache::save(*sceneCachePolicy_.path, gpuCached)) {
            std::cout << "GPU scene cache saved: " << sceneCachePolicy_.path->string() << "\n";
        }
    }
}

void GpuScene::createImportedSceneFromCache(BufferUploader& uploader, const CachedScene& cached, const std::vector<SceneLightAsset>& activeSceneLights) {
    std::cout << "GPU cache hit: restoring cached BVH data for " << cached.meshGpuRecords.size() << " meshes.\n";
    createCachedMaterialTextures(uploader, cached);

    std::vector<GpuMeshRecord> meshRecords;
    meshRecords.reserve(cached.meshGpuRecords.size());
    std::vector<GpuPrimitiveRecord> primitiveRecords;
    primitiveRecords.reserve(cached.primitiveRecords.size());
    std::vector<GpuInstanceRecord> instanceRecords;
    instanceRecords.reserve(cached.instanceRecords.size());
    std::vector<GpuInstanceBoundsRecord> instanceBounds;
    instanceBounds.reserve(cached.instanceBounds.size());
    std::vector<GpuLocalVertex> localVertexData;
    std::vector<uint32_t> localIndices;
    std::vector<glm::vec4> localBvhData;
    std::vector<glm::vec4> localTriangleData;
    std::vector<GpuLightRecord> lightRecords;
    lightRecords.reserve(cached.lightRecords.size());
    rayTracingMeshes_.clear();
    rayTracingInstances_.clear();
    std::vector<bool> materialOpaqueTraversalSafe;
    materialOpaqueTraversalSafe.reserve(cached.materials.size());
    std::vector<uint32_t> materialAlphaClasses;
    materialAlphaClasses.reserve(cached.materials.size());
    for (const CachedMaterialData& material : cached.materials) {
        materialOpaqueTraversalSafe.push_back(material.alphaMode == kMaterialAlphaModeOpaque && material.doubleSided != 0u);
        if (material.alphaMode == kMaterialAlphaModeMask) {
            materialAlphaClasses.push_back(kPrimitiveAlphaClassAlphaTested);
        } else if (material.alphaMode == kMaterialAlphaModeBlend) {
            materialAlphaClasses.push_back(kPrimitiveAlphaClassBlended);
        } else {
            materialAlphaClasses.push_back(kPrimitiveAlphaClassOpaque);
        }
    }
    for (const auto& cachedPrim : cached.primitiveRecords) {
        GpuPrimitiveRecord rec{};
        rec.indexData = cachedPrim.indexData;
        rec.metadata = cachedPrim.metadata;
        primitiveRecords.push_back(rec);
    }
    annotatePrimitiveAlphaClasses(primitiveRecords, materialAlphaClasses, &materialOpaqueTraversalSafe);

    for (const auto& cachedMesh : cached.meshGpuRecords) {
        GpuMeshRecord rec{};
        rec.vertexIndexData = cachedMesh.vertexIndexData;
        rec.primitiveData = cachedMesh.primitiveData;
        rec.flags = cachedMesh.flags;

        rec.bvhData.x = static_cast<uint32_t>(localBvhData.size() / 4u);
        rec.bvhData.y = static_cast<uint32_t>(cachedMesh.localBvh.packedNodes.size());
        rec.bvhData.z = static_cast<uint32_t>(localTriangleData.size() / 12u);
        rec.bvhData.w = cachedMesh.localBvh.leafTriangleCount;
        meshRecords.push_back(rec);
        rayTracingMeshes_.push_back(RayTracingMeshBuildInput{
            .meshIndex = static_cast<uint32_t>(meshRecords.size() - 1),
            .firstVertex = rec.vertexIndexData.x,
            .vertexCount = rec.vertexIndexData.y,
            .firstIndex = rec.vertexIndexData.z,
            .indexCount = rec.vertexIndexData.w,
            .primitiveOffset = rec.primitiveData.x,
            .primitiveCount = rec.primitiveData.y,
            .containsAlphaTestedGeometry = meshPrimitiveAlphaClass(
                primitiveRecords,
                rec.primitiveData.x,
                rec.primitiveData.y,
                kPrimitiveAlphaClassAlphaTested) != 0u,
            .containsBlendedGeometry = meshPrimitiveAlphaClass(
                primitiveRecords,
                rec.primitiveData.x,
                rec.primitiveData.y,
                kPrimitiveAlphaClassBlended) != 0u,
            .opaqueTraversalSafe = primitivesAreOpaqueTraversalSafe(
                primitiveRecords,
                rec.primitiveData.x,
                rec.primitiveData.y,
                materialOpaqueTraversalSafe),
            .updateMode = AccelUpdateMode::Static,
        });

        localBvhData.insert(localBvhData.end(), cachedMesh.localBvh.packedNodes.begin(), cachedMesh.localBvh.packedNodes.end());
        localTriangleData.insert(localTriangleData.end(), cachedMesh.localBvh.triangleData.begin(), cachedMesh.localBvh.triangleData.end());
    }

    const std::vector<CachedMeshCpuPayload> cachedMeshPayloads = buildCachedMeshCpuPayloads(cached.meshes);
    appendCachedMeshCpuPayloads(cachedMeshPayloads, localVertexData, localIndices);

    instanceRecords.resize(cached.instanceRecords.size());
    rayTracingInstances_.resize(cached.instanceRecords.size());
    parallelFor(cached.instanceRecords.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const CachedInstanceRecord& cachedInst = cached.instanceRecords[i];
            GpuInstanceRecord rec{};
            rec.transform = cachedInst.transform;
            rec.inverseTransform = cachedInst.inverseTransform;
            rec.normalTransform = glm::transpose(cachedInst.inverseTransform);
            rec.prevTransform = cachedInst.transform;
            rec.metadata = cachedInst.metadata;
            instanceRecords[i] = rec;
            rayTracingInstances_[i] = RayTracingInstanceBuildInput{
                .instanceIndex = static_cast<uint32_t>(i),
                .meshIndex = rec.metadata.x,
                .transform = rec.transform,
                .previousTransform = rec.prevTransform,
                .flags = rec.metadata.w,
            };
        }
    }, 16);

    instanceBounds.resize(cached.instanceBounds.size());
    parallelFor(cached.instanceBounds.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const CachedInstanceBoundsRecord& cachedBounds = cached.instanceBounds[i];
            instanceBounds[i] = GpuInstanceBoundsRecord{
                .bmin = cachedBounds.bmin,
                .bmax = cachedBounds.bmax,
            };
        }
    }, 16);

    std::vector<GpuLightRecord> cachedLightRecords;
    cachedLightRecords.reserve(cached.lightRecords.size());
    for (const auto& cachedLight : cached.lightRecords) {
        GpuLightRecord rec{};
        rec.metadata = cachedLight.metadata;
        rec.identity = cachedLight.identity;
        rec.data0 = cachedLight.data0;
        rec.data1 = cachedLight.data1;
        rec.data2 = cachedLight.data2;
        rec.data3 = cachedLight.data3;
        cachedLightRecords.push_back(rec);
    }
    emissiveLightRecords_.clear();
    emissiveLightRecords_.reserve(cachedLightRecords.size());
    for (const GpuLightRecord& record : cachedLightRecords) {
        if (isEmissiveGpuLightRecord(record)) {
            emissiveLightRecords_.push_back(record);
        }
    }
    const float emissiveTotalWeight = emissiveLightRecords_.empty() ? 0.0f : emissiveLightRecords_.back().data0.y;
    float lightSelectionWeight = emissiveTotalWeight;
    lightRecords = combineLightRecords(
        emissiveLightRecords_,
        activeSceneLights,
        emissiveTotalWeight,
        lightSelectionWeight);
    lightRecordCpu_ = lightRecords;

    const std::vector<uint32_t> rtTriangleMaterialIds =
        buildRtTriangleMaterialIds(primitiveRecords, static_cast<uint32_t>(localIndices.size() / 3u));
    meshParams_ = fromCachedMeshParams(cached.meshParams);
    meshParams_.enabled = 1;
    meshParams_.localVertexCount = static_cast<uint32_t>(localVertexData.size());
    meshParams_.localIndexCount = static_cast<uint32_t>(localIndices.size());
    const std::vector<glm::vec4> materialData = buildCachedMaterialData(cached);
    meshParams_.materialCount = static_cast<uint32_t>(materialData.size() / materialVec4Stride);
    hasTransmissiveMaterials_ = materialDataContainsTransmission(materialData);
    applyLightRecordMetadataToMeshParams(meshParams_, lightRecords, lightSelectionWeight);
    rayTracingGeometryStats_ = computeRayTracingGeometryStats(meshRecords, primitiveRecords);
    primitiveRecordCpu_ = primitiveRecords;
    localVertexCpu_ = localVertexData;
    meshRecordCpu_ = meshRecords;
    localTriangleDataCpu_ = localTriangleData;
    materialEmissiveCpu_ = extractMaterialEmissive(materialData);
    sphereDataCpu_.clear();
    logRayTracingGeometryStats("Cached scene", rayTracingGeometryStats_);
    if (opacityMicromapsEnabled_) {
        opacityMicromapData_ = generateOpacityMicromapData(cached, opacityMicromapSubdivisionLevel_);
        logOpacityMicromapPreprocessStats("Cached scene", opacityMicromapData_.stats);
    } else {
        opacityMicromapData_ = {};
        opacityMicromapData_.stats.subdivisionLevel = opacityMicromapSubdivisionLevel_;
        std::cout << "Cached scene OMM preprocess skipped: opacity micromaps disabled.\n";
    }

    {
        BatchUploader batch(uploader);
        batch.begin();

        std::vector<glm::vec4> emptyVec4;
        std::vector<uint32_t> emptyU32;
        uploadVectorBatched(batch, vertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene vertices (cached)");
        uploadVectorBatched(batch, indices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyU32, "imported scene indices (cached)");
        uploadVectorBatched(batch, bvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene bvh nodes (cached)");
        uploadVectorBatched(batch, triangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene triangles (cached)");

        std::vector<glm::vec4> emptySpheres;
        uploadVectorBatched(batch, materials_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, materialData, "imported scene materials (cached)");
        uploadVectorBatched(batch, spheres_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptySpheres, "imported scene spheres (cached)");

        uploadVectorBatched(batch, meshRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, meshRecords, "imported mesh records (cached)");
        uploadVectorBatched(batch, primitiveRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, primitiveRecords, "imported primitive records (cached)");
        uploadVectorBatched(batch, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "imported instance records (cached)");
        uploadVectorBatched(batch, rtTriangleMaterialIds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rtTriangleMaterialIds, "imported rt triangle material ids (cached)");
        uploadVectorBatched(batch, lightRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lightRecords, "imported emissive light records (cached)");

        uploadVectorBatched(batch, localVertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localVertexData, "imported local mesh vertices (cached)");
        uploadVectorBatched(batch, localIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localIndices, "imported local mesh indices (cached)");
        uploadVectorBatched(batch, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "imported instance bounds (cached)");
        uploadVectorBatched(batch, localBvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localBvhData, "imported local bvh nodes (cached)");
        uploadVectorBatched(batch, localTriangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localTriangleData, "imported local bvh triangles (cached)");
        uploadVectorBatched(batch, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cached.tlasNodes, "imported tlas nodes (cached)");
        uploadVectorBatched(batch, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cached.tlasInstanceIndices, "imported tlas instance indices (cached)");
        batch.submit();
    }

    instanceRecordCpu_ = instanceRecords;
    meshParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(MeshParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "imported mesh params (cached)",
    });
    meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
    meshParamsBuffer_->flush(sizeof(meshParams_));
    uploadLightBvh(uploader, lightRecords);
}

void GpuScene::createImportedSceneGeometryFromCache(BufferUploader& uploader, const CachedScene& cached, const SceneAsset& activeScene) {
    std::cout << "GPU geometry cache hit: restoring cached mesh/BVH data for "
              << cached.meshGpuRecords.size()
              << " meshes and rebuilding active instances/lights.\n";
    createCachedMaterialTextures(uploader, cached);

    std::vector<GpuMeshRecord> meshRecords;
    meshRecords.reserve(cached.meshGpuRecords.size());
    std::vector<GpuPrimitiveRecord> primitiveRecords;
    primitiveRecords.reserve(cached.primitiveRecords.size());
    std::vector<GpuInstanceRecord> instanceRecords;
    std::vector<GpuInstanceBoundsRecord> instanceBounds;
    std::vector<GpuLocalVertex> localVertexData;
    std::vector<uint32_t> localIndices;
    std::vector<glm::vec4> localBvhData;
    std::vector<glm::vec4> localTriangleData;
    std::vector<CpuBounds> localMeshBounds;
    localMeshBounds.reserve(cached.meshes.size());
    rayTracingMeshes_.clear();
    rayTracingInstances_.clear();

    std::vector<bool> materialOpaqueTraversalSafe;
    materialOpaqueTraversalSafe.reserve(cached.materials.size());
    std::vector<uint32_t> materialAlphaClasses;
    materialAlphaClasses.reserve(cached.materials.size());
    for (const CachedMaterialData& material : cached.materials) {
        materialOpaqueTraversalSafe.push_back(material.alphaMode == kMaterialAlphaModeOpaque && material.doubleSided != 0u);
        if (material.alphaMode == kMaterialAlphaModeMask) {
            materialAlphaClasses.push_back(kPrimitiveAlphaClassAlphaTested);
        } else if (material.alphaMode == kMaterialAlphaModeBlend) {
            materialAlphaClasses.push_back(kPrimitiveAlphaClassBlended);
        } else {
            materialAlphaClasses.push_back(kPrimitiveAlphaClassOpaque);
        }
    }

    for (const CachedPrimitiveRecord& cachedPrim : cached.primitiveRecords) {
        primitiveRecords.push_back(GpuPrimitiveRecord{
            .indexData = cachedPrim.indexData,
            .metadata = cachedPrim.metadata,
        });
    }
    annotatePrimitiveAlphaClasses(primitiveRecords, materialAlphaClasses, &materialOpaqueTraversalSafe);

    for (size_t meshIndex = 0; meshIndex < cached.meshGpuRecords.size(); ++meshIndex) {
        const CachedMeshGpuRecord& cachedMesh = cached.meshGpuRecords[meshIndex];
        GpuMeshRecord rec{};
        rec.vertexIndexData = cachedMesh.vertexIndexData;
        rec.primitiveData = cachedMesh.primitiveData;
        rec.flags = cachedMesh.flags;
        rec.bvhData.x = static_cast<uint32_t>(localBvhData.size() / 4u);
        rec.bvhData.y = static_cast<uint32_t>(cachedMesh.localBvh.packedNodes.size());
        rec.bvhData.z = static_cast<uint32_t>(localTriangleData.size() / 12u);
        rec.bvhData.w = cachedMesh.localBvh.leafTriangleCount;
        meshRecords.push_back(rec);

        const uint32_t activeMeshHandle = meshIndex < activeScene.meshes.size()
            ? activeScene.meshes[meshIndex].index
            : 0xffffffffu;
        rayTracingMeshes_.push_back(RayTracingMeshBuildInput{
            .meshIndex = static_cast<uint32_t>(meshRecords.size() - 1),
            .sourceMeshHandleIndex = activeMeshHandle,
            .firstVertex = rec.vertexIndexData.x,
            .vertexCount = rec.vertexIndexData.y,
            .firstIndex = rec.vertexIndexData.z,
            .indexCount = rec.vertexIndexData.w,
            .primitiveOffset = rec.primitiveData.x,
            .primitiveCount = rec.primitiveData.y,
            .containsAlphaTestedGeometry = meshPrimitiveAlphaClass(
                primitiveRecords,
                rec.primitiveData.x,
                rec.primitiveData.y,
                kPrimitiveAlphaClassAlphaTested) != 0u,
            .containsBlendedGeometry = meshPrimitiveAlphaClass(
                primitiveRecords,
                rec.primitiveData.x,
                rec.primitiveData.y,
                kPrimitiveAlphaClassBlended) != 0u,
            .opaqueTraversalSafe = primitivesAreOpaqueTraversalSafe(
                primitiveRecords,
                rec.primitiveData.x,
                rec.primitiveData.y,
                materialOpaqueTraversalSafe),
            .updateMode = AccelUpdateMode::Static,
        });

        localBvhData.insert(localBvhData.end(), cachedMesh.localBvh.packedNodes.begin(), cachedMesh.localBvh.packedNodes.end());
        localTriangleData.insert(localTriangleData.end(), cachedMesh.localBvh.triangleData.begin(), cachedMesh.localBvh.triangleData.end());
    }

    const std::vector<CachedMeshCpuPayload> cachedMeshPayloads = buildCachedMeshCpuPayloads(cached.meshes);
    appendCachedMeshCpuPayloads(cachedMeshPayloads, localVertexData, localIndices, &localMeshBounds);

    std::unordered_map<uint32_t, uint32_t> meshRecordIndexForAsset;
    meshRecordIndexForAsset.reserve(activeScene.meshes.size());
    const size_t mappedMeshCount = std::min(activeScene.meshes.size(), meshRecords.size());
    for (size_t i = 0; i < mappedMeshCount; ++i) {
        if (activeScene.meshes[i].valid()) {
            meshRecordIndexForAsset.emplace(activeScene.meshes[i].index, static_cast<uint32_t>(i));
        }
    }

    struct PendingGeometryCacheInstance {
        glm::mat4 transform{1.0f};
        glm::mat4 previousTransform{1.0f};
        uint32_t meshRecordIndex = 0;
        uint32_t flags = instanceFlagVisible | instanceFlagVisibleToCamera | instanceFlagCastShadow;
    };
    std::vector<PendingGeometryCacheInstance> pendingInstances;
    pendingInstances.reserve(activeScene.nodes.size());

    auto appendInstance = [&](const glm::mat4& transform, const glm::mat4& previousTransform, uint32_t meshHandleIndex, uint32_t flags) {
        auto recordIt = meshRecordIndexForAsset.find(meshHandleIndex);
        if (recordIt == meshRecordIndexForAsset.end()) {
            return;
        }
        const uint32_t meshRecordIndex = recordIt->second;
        if (meshRecordIndex >= meshRecords.size() || meshRecordIndex >= localMeshBounds.size()) {
            return;
        }
        pendingInstances.push_back(PendingGeometryCacheInstance{
            .transform = transform,
            .previousTransform = previousTransform,
            .meshRecordIndex = meshRecordIndex,
            .flags = flags,
        });
    };

    auto visitNode = [&](auto&& self, uint32_t nodeIndex, const glm::mat4& parent, const glm::mat4& previousParent) -> void {
        if (nodeIndex >= activeScene.nodes.size()) {
            return;
        }
        const SceneNodeAsset& node = activeScene.nodes[nodeIndex];
        const glm::mat4 world = parent * node.transform;
        const glm::mat4 nodePrevious = node.previousTransformValid ? node.previousTransform : node.transform;
        const glm::mat4 previousWorld = previousParent * nodePrevious;
        if (node.mesh.valid()) {
            appendInstance(world, previousWorld, node.mesh.index, nodeInstanceFlags(node));
        }
        for (uint32_t child : node.children) {
            self(self, child, world, previousWorld);
        }
    };
    if (!activeScene.rootNodes.empty()) {
        for (uint32_t root : activeScene.rootNodes) {
            visitNode(visitNode, root, glm::mat4{1.0f}, glm::mat4{1.0f});
        }
    } else {
        for (uint32_t i = 0; i < activeScene.nodes.size(); ++i) {
            if (activeScene.nodes[i].parent < 0) {
                visitNode(visitNode, i, glm::mat4{1.0f}, glm::mat4{1.0f});
            }
        }
    }

    instanceRecords.resize(pendingInstances.size());
    instanceBounds.resize(pendingInstances.size());
    rayTracingInstances_.resize(pendingInstances.size());
    parallelFor(pendingInstances.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const PendingGeometryCacheInstance& pending = pendingInstances[i];
            const GpuMeshRecord& meshRecord = meshRecords[pending.meshRecordIndex];
            const uint32_t primitiveOffset = meshRecord.primitiveData.x;
            const uint32_t primitiveCount = meshRecord.primitiveData.y;
            const uint32_t instanceIndex = static_cast<uint32_t>(i);
            instanceRecords[i] = makeInstanceRecord(
                pending.transform,
                pending.meshRecordIndex,
                primitiveOffset,
                primitiveCount,
                pending.flags,
                &pending.previousTransform);
            rayTracingInstances_[i] = RayTracingInstanceBuildInput{
                .instanceIndex = instanceIndex,
                .meshIndex = pending.meshRecordIndex,
                .transform = pending.transform,
                .previousTransform = pending.previousTransform,
                .flags = pending.flags,
                .visible = (pending.flags & instanceFlagVisible) != 0u,
            };
            instanceBounds[i] = makeInstanceBoundsRecord(
                transformBounds(localMeshBounds[pending.meshRecordIndex], pending.transform),
                instanceIndex,
                pending.meshRecordIndex);
        }
    }, 16);

    if (instanceRecords.empty() || localVertexData.empty() || localIndices.empty() || localBvhData.empty() || localTriangleData.empty()) {
        std::cout << "GPU geometry cache rejected at rebuild: active scene produced no valid instances.\n";
        createCornellBox(uploader);
        return;
    }

    const std::vector<uint32_t> rtTriangleMaterialIds =
        buildRtTriangleMaterialIds(primitiveRecords, static_cast<uint32_t>(localIndices.size() / 3u));
    std::vector<glm::vec4> tlasData;
    std::vector<uint32_t> tlasInstanceIndices;
    buildTlas(instanceBounds, tlasData, tlasInstanceIndices);

    const std::vector<glm::vec4> materialData = buildCachedMaterialData(cached);
    std::vector<glm::vec4> sphereData;
    float emissiveTotalArea = 0.0f;
    const std::vector<glm::vec3> materialEmissive = extractMaterialEmissive(materialData);
    emissiveLightRecords_ = buildLightRecords(meshRecords, instanceRecords, localTriangleData, materialEmissive, sphereData, emissiveTotalArea);
    float lightSelectionWeight = emissiveTotalArea;
    const std::vector<GpuLightRecord> lightRecords = combineLightRecords(
        emissiveLightRecords_,
        activeScene.lights,
        emissiveTotalArea,
        lightSelectionWeight);
    lightRecordCpu_ = lightRecords;

    meshParams_ = fromCachedMeshParams(cached.meshParams);
    meshParams_.enabled = 1;
    meshParams_.vertexCount = static_cast<uint32_t>(localVertexData.size());
    meshParams_.triangleCount = static_cast<uint32_t>(localIndices.size() / 3u);
    meshParams_.materialCount = static_cast<uint32_t>(materialData.size() / materialVec4Stride);
    meshParams_.primitiveCount = static_cast<uint32_t>(primitiveRecords.size());
    meshParams_.instanceCount = static_cast<uint32_t>(instanceRecords.size());
    meshParams_.lightCount = static_cast<uint32_t>(lightRecords.size());
    meshParams_.meshCount = static_cast<uint32_t>(meshRecords.size());
    meshParams_.localVertexCount = static_cast<uint32_t>(localVertexData.size());
    meshParams_.localIndexCount = static_cast<uint32_t>(localIndices.size());
    meshParams_.localBvhNodeCount = static_cast<uint32_t>(localBvhData.size() / 4u);
    meshParams_.localTriangleCount = static_cast<uint32_t>(localTriangleData.size() / 12u);
    meshParams_.tlasNodeCount = static_cast<uint32_t>(tlasData.size() / 4u);
    meshParams_.tlasInstanceIndexCount = static_cast<uint32_t>(tlasInstanceIndices.size());
    applyLightRecordMetadataToMeshParams(meshParams_, lightRecords, lightSelectionWeight);
    hasTransmissiveMaterials_ = materialDataContainsTransmission(materialData);
    rayTracingGeometryStats_ = computeRayTracingGeometryStats(meshRecords, primitiveRecords);
    primitiveRecordCpu_ = primitiveRecords;
    localVertexCpu_ = localVertexData;
    meshRecordCpu_ = meshRecords;
    localTriangleDataCpu_ = localTriangleData;
    materialEmissiveCpu_ = materialEmissive;
    sphereDataCpu_.clear();
    instanceRecordCpu_ = instanceRecords;

    std::cout << "Imported scene GPU data from geometry cache: meshes=" << meshParams_.meshCount
              << " instances=" << meshParams_.instanceCount
              << " local_triangles=" << meshParams_.localTriangleCount
              << " local_bvh_nodes=" << meshParams_.localBvhNodeCount
              << " tlas_nodes=" << meshParams_.tlasNodeCount << '\n';
    logRayTracingGeometryStats("Geometry cached scene", rayTracingGeometryStats_);
    if (opacityMicromapsEnabled_) {
        opacityMicromapData_ = generateOpacityMicromapData(cached, opacityMicromapSubdivisionLevel_);
        logOpacityMicromapPreprocessStats("Geometry cached scene", opacityMicromapData_.stats);
    } else {
        opacityMicromapData_ = {};
        opacityMicromapData_.stats.subdivisionLevel = opacityMicromapSubdivisionLevel_;
        std::cout << "Geometry cached scene OMM preprocess skipped: opacity micromaps disabled.\n";
    }

    {
        BatchUploader batch(uploader);
        batch.begin();

        std::vector<glm::vec4> emptyVec4;
        std::vector<uint32_t> emptyU32;
        uploadVectorBatched(batch, vertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene vertices (geometry cache)");
        uploadVectorBatched(batch, indices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyU32, "imported scene indices (geometry cache)");
        uploadVectorBatched(batch, bvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene bvh nodes (geometry cache)");
        uploadVectorBatched(batch, triangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene triangles (geometry cache)");

        uploadVectorBatched(batch, materials_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, materialData, "imported scene materials (geometry cache)");
        uploadVectorBatched(batch, spheres_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sphereData, "imported scene spheres (geometry cache)");
        uploadVectorBatched(batch, meshRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, meshRecords, "imported mesh records (geometry cache)");
        uploadVectorBatched(batch, primitiveRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, primitiveRecords, "imported primitive records (geometry cache)");
        uploadVectorBatched(batch, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "imported instance records (geometry cache)");
        uploadVectorBatched(batch, rtTriangleMaterialIds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rtTriangleMaterialIds, "imported rt triangle material ids (geometry cache)");
        uploadVectorBatched(batch, lightRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lightRecords, "imported emissive light records (geometry cache)");
        uploadVectorBatched(batch, localVertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localVertexData, "imported local mesh vertices (geometry cache)");
        uploadVectorBatched(batch, localIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localIndices, "imported local mesh indices (geometry cache)");
        uploadVectorBatched(batch, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "imported instance bounds (geometry cache)");
        uploadVectorBatched(batch, localBvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localBvhData, "imported local bvh nodes (geometry cache)");
        uploadVectorBatched(batch, localTriangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localTriangleData, "imported local bvh triangles (geometry cache)");
        uploadVectorBatched(batch, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasData, "imported tlas nodes (geometry cache)");
        uploadVectorBatched(batch, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasInstanceIndices, "imported tlas instance indices (geometry cache)");
        batch.submit();
    }

    meshParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(MeshParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "imported mesh params (geometry cache)",
    });
    meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
    meshParamsBuffer_->flush(sizeof(meshParams_));
    uploadLightBvh(uploader, lightRecords);
}

void GpuScene::createEnvironment(BufferUploader& uploader) {
    constexpr uint32_t width = 512;
    constexpr uint32_t height = 256;
    const bool useExternalEnvironment = environmentPath_.has_value();
    const HdrImageData environment = useExternalEnvironment
        ? HdrLoader::loadRadiance(*environmentPath_)
        : HdrLoader::createProcedural(width, height);
    const EnvironmentImportanceData importance = EnvironmentImportanceSampler::build(environment.rgba.data(), environment.width, environment.height);
    const std::vector<uint16_t> halfPixels = rgba32fToRgba16f(environment.rgba);

    std::cout << (useExternalEnvironment ? "Loaded HDR environment: " : "Using procedural environment: ")
              << (useExternalEnvironment ? environmentPath_->string() : std::string("procedural"))
              << " (" << environment.width << "x" << environment.height << ")\n";

    environmentImage_ = std::make_unique<Image>(allocator_, ImageDesc{
        .width = environment.width,
        .height = environment.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = useExternalEnvironment ? "HDR environment" : "procedural environment",
    });

    {
        BatchUploader batch(uploader);
        batch.begin();
        batch.enqueueImageUpload(*environmentImage_, halfPixels.data(), sizeof(uint16_t) * halfPixels.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        uploadVectorBatched(batch, envRows_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, importance.rowAlias, "environment row alias");
        uploadVectorBatched(batch, envCols_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, importance.columnAlias, "environment col alias");
        batch.submit();
    }

    envParams_ = {
        .enabled = 1,
        .intensity = 1.0f,
        .rotation = 0.0f,
        .width = environment.width,
        .height = environment.height,
        .backgroundIntensity = 0.35f,
        .procedural = useExternalEnvironment ? 0u : 1u,
        .skyCdfWidth = 256u,
        .invTotalLum = importance.invTotalLuminance,
        .skyCdfHeight = 144u,
    };
    envParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(EnvParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "environment params",
    });
    envParamsBuffer_->write(&envParams_, sizeof(envParams_));
    envParamsBuffer_->flush(sizeof(envParams_));

    if (environmentSampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checkVk(vkCreateSampler(allocator_.device(), &samplerInfo, nullptr, &environmentSampler_), "vkCreateSampler(environment)");
    }
}

void GpuScene::uploadEnvironmentParams() {
    if (envParamsBuffer_) {
        envParamsBuffer_->write(&envParams_, sizeof(envParams_));
        envParamsBuffer_->flush(sizeof(envParams_));
    }
}

void GpuScene::uploadLightRecords(
    BufferUploader& uploader,
    std::vector<GpuLightRecord> lightRecords,
    float totalWeight,
    uint64_t retireFrame,
    bool logLightBvhStats,
    bool rebuildLightBvh) {
    if (lightRecords.empty()) {
        lightRecords.push_back(GpuLightRecord{});
        totalWeight = 0.0f;
    }
    lightRecordCpu_ = lightRecords;

    retireBuffer(uploadBuffer(
        allocator_,
        uploader,
        lightRecords_,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        lightRecords.data(),
        sizeof(GpuLightRecord) * lightRecords.size(),
        "scene light records"), retireFrame);

    applyLightRecordMetadataToMeshParams(meshParams_, lightRecords, totalWeight);
    if (meshParamsBuffer_) {
        meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
        meshParamsBuffer_->flush(sizeof(meshParams_));
    }
    if (rebuildLightBvh) {
        uploadLightBvh(uploader, lightRecords, retireFrame, logLightBvhStats);
    }
}

void GpuScene::uploadLightBvh(BufferUploader& uploader, const std::vector<GpuLightRecord>& lightRecords, uint64_t retireFrame, bool logStats) {
    std::vector<LightBvhPrimitive> lightPrimitives;
    lightPrimitives.reserve(lightRecords.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(lightRecords.size()); ++i) {
        lightPrimitives.push_back(makeLightBvhPrimitive(lightRecords[i], i));
    }
    std::vector<LightBvhNode> bvhNodes = buildLightBvh(lightPrimitives, 1);
    const LightBvhStats stats = computeLightBvhStats(bvhNodes);
    if (logStats && stats.nodeCount > 0u) {
        std::cout << "Light BVH: nodes=" << stats.nodeCount
                  << " leaves=" << stats.leafCount
                  << " max_depth=" << stats.maxDepth
                  << " avg_traversal=" << stats.estimatedAverageTraversalSteps
                  << " power=[" << stats.minLeafPower << ", " << stats.maxLeafPower << "]\n";
    }
    std::vector<glm::vec4> packed = packLightBvhNodesForGpu(bvhNodes);
    if (packed.empty()) {
        packed.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
    }
    retireBuffer(uploadBuffer(
        allocator_,
        uploader,
        lightBvhNodes_,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        packed.data(),
        sizeof(glm::vec4) * packed.size(),
        "scene light bvh nodes"), retireFrame);
}

uint32_t GpuScene::textureSlotIndexFor(const SceneAsset& scene, TextureAssetHandle texture, uint32_t maxSlots) {
    if (!texture.valid()) {
        return UINT32_MAX;
    }
    for (uint32_t slot = 0; slot < scene.textures.size() && slot < maxSlots; ++slot) {
        if (scene.textures[slot].index == texture.index) {
            return slot;
        }
    }
    return UINT32_MAX;
}

} // namespace rtv
