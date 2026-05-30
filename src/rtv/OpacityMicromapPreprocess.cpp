#include "rtv/OpacityMicromapPreprocess.h"

#include "rtv/AssetManager.h"
#include "rtv/MeshAsset.h"
#include "rtv/SceneCache.h"
#include "rtv/TextureAsset.h"

#include <stb_image_write.h>

#include <Volk/volk.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace rtv {
namespace {

struct AlphaTextureView {
    const uint8_t* bytes = nullptr;
    size_t byteCount = 0;
    uint32_t width = 1;
    uint32_t height = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool compressed = false;
    bool fallback = false;
    TextureSamplerDesc sampler{};
};

struct AlphaMaterialView {
    uint32_t alphaMode = kMaterialAlphaModeOpaque;
    float alphaCutoff = 0.5f;
    float baseAlpha = 1.0f;
    int32_t baseColorTextureIndex = -1;
    TextureTransformAsset baseColorTextureTransform{};
};

struct AlphaPrimitiveRequest {
    uint32_t meshIndex = 0;
    uint32_t primitiveIndex = 0;
    uint32_t materialIndex = 0;
    const MeshPrimitiveAsset* primitive = nullptr;
    const MeshAsset* mesh = nullptr;
    AlphaMaterialView material{};
    AlphaTextureView texture{};
};

struct GeneratedPrimitiveStates {
    OpacityMicromapPrimitiveCpuData primitive{};
    std::vector<uint8_t> states;
};

[[nodiscard]] uint32_t clampSubdivisionLevel(uint32_t subdivisionLevel, OpacityMicromapPreprocessStats& stats) {
    constexpr uint32_t kMaxCpuSubdivisionLevel = 5u;
    if (subdivisionLevel > kMaxCpuSubdivisionLevel) {
        std::ostringstream warning;
        warning << "OMM CPU preprocessing subdivision level " << subdivisionLevel
                << " clamped to " << kMaxCpuSubdivisionLevel << " to bound diagnostic memory";
        stats.warnings.push_back(warning.str());
        return kMaxCpuSubdivisionLevel;
    }
    return subdivisionLevel;
}

[[nodiscard]] float wrapCoord(float value, TextureWrap wrap) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    switch (wrap) {
    case TextureWrap::ClampToEdge:
        return std::clamp(value, 0.0f, std::nextafter(1.0f, 0.0f));
    case TextureWrap::MirroredRepeat: {
        const float period = std::floor(value);
        const float frac = value - period;
        const int32_t whole = static_cast<int32_t>(period);
        return (whole & 1) != 0 ? 1.0f - frac : frac;
    }
    case TextureWrap::Repeat:
    default:
        return value - std::floor(value);
    }
}

[[nodiscard]] glm::vec2 applyTextureTransform(glm::vec2 uv, const TextureTransformAsset& transform) {
    if (transform.enabled == 0u) {
        return uv;
    }
    glm::vec2 value = uv * transform.scale;
    const float c = std::cos(transform.rotation);
    const float s = std::sin(transform.rotation);
    value = glm::vec2(c * value.x - s * value.y, s * value.x + c * value.y);
    return value + transform.offset;
}

[[nodiscard]] bool alphaTextureReadable(const AlphaTextureView& texture) {
    if (texture.bytes == nullptr || texture.byteCount == 0 || texture.width == 0 || texture.height == 0 || texture.compressed) {
        return false;
    }
    switch (texture.format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return texture.byteCount >= static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height) * 4u;
    case VK_FORMAT_R16G16B16A16_UNORM:
        return texture.byteCount >= static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height) * 8u;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return texture.byteCount >= static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height) * 16u;
    default:
        return false;
    }
}

[[nodiscard]] float readAlphaAt(const AlphaTextureView& texture, uint32_t x, uint32_t y) {
    x = std::min(x, texture.width - 1u);
    y = std::min(y, texture.height - 1u);
    const size_t pixel = static_cast<size_t>(y) * texture.width + x;
    switch (texture.format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return static_cast<float>(texture.bytes[pixel * 4u + 3u]) / 255.0f;
    case VK_FORMAT_R16G16B16A16_UNORM: {
        uint16_t alpha = 0;
        std::memcpy(&alpha, texture.bytes + pixel * 8u + 6u, sizeof(alpha));
        return static_cast<float>(alpha) / 65535.0f;
    }
    case VK_FORMAT_R32G32B32A32_SFLOAT: {
        float alpha = 1.0f;
        std::memcpy(&alpha, texture.bytes + pixel * 16u + 12u, sizeof(alpha));
        return std::clamp(alpha, 0.0f, 1.0f);
    }
    default:
        return 1.0f;
    }
}

[[nodiscard]] float sampleTextureAlpha(const AlphaTextureView& texture, glm::vec2 uv) {
    uv.x = wrapCoord(uv.x, texture.sampler.wrapS);
    uv.y = wrapCoord(uv.y, texture.sampler.wrapT);

    const bool nearest = texture.sampler.magFilter == TextureFilter::Nearest;
    if (nearest) {
        const uint32_t x = std::min(static_cast<uint32_t>(uv.x * static_cast<float>(texture.width)), texture.width - 1u);
        const uint32_t y = std::min(static_cast<uint32_t>(uv.y * static_cast<float>(texture.height)), texture.height - 1u);
        return readAlphaAt(texture, x, y);
    }

    const float fx = uv.x * static_cast<float>(texture.width) - 0.5f;
    const float fy = uv.y * static_cast<float>(texture.height) - 0.5f;
    const int32_t x0 = static_cast<int32_t>(std::floor(fx));
    const int32_t y0 = static_cast<int32_t>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    auto sampleWrapped = [&](int32_t x, int32_t y) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(texture.width);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(texture.height);
        const uint32_t sx = std::min(static_cast<uint32_t>(wrapCoord(u, texture.sampler.wrapS) * texture.width), texture.width - 1u);
        const uint32_t sy = std::min(static_cast<uint32_t>(wrapCoord(v, texture.sampler.wrapT) * texture.height), texture.height - 1u);
        return readAlphaAt(texture, sx, sy);
    };

    const float a00 = sampleWrapped(x0, y0);
    const float a10 = sampleWrapped(x0 + 1, y0);
    const float a01 = sampleWrapped(x0, y0 + 1);
    const float a11 = sampleWrapped(x0 + 1, y0 + 1);
    const float ax0 = a00 * (1.0f - tx) + a10 * tx;
    const float ax1 = a01 * (1.0f - tx) + a11 * tx;
    return ax0 * (1.0f - ty) + ax1 * ty;
}

[[nodiscard]] glm::vec2 interpolateUv(
    const std::array<glm::vec2, 3>& uv,
    const glm::vec3& bary,
    const TextureTransformAsset& transform) {
    return applyTextureTransform(uv[0] * bary.x + uv[1] * bary.y + uv[2] * bary.z, transform);
}

[[nodiscard]] OpacityMicromapCpuState classifySamples(const std::array<float, 4>& samples, float alphaCutoff) {
    bool allOpaque = true;
    bool allTransparent = true;
    for (float alpha : samples) {
        allOpaque = allOpaque && alpha >= alphaCutoff;
        allTransparent = allTransparent && alpha < alphaCutoff;
    }
    if (allOpaque) {
        return OpacityMicromapCpuState::Opaque;
    }
    if (allTransparent) {
        return OpacityMicromapCpuState::Transparent;
    }
    return OpacityMicromapCpuState::Mixed;
}

void accumulateState(OpacityMicromapPrimitiveCpuData& primitive, OpacityMicromapPreprocessStats& stats, OpacityMicromapCpuState state) {
    switch (state) {
    case OpacityMicromapCpuState::Opaque:
        ++primitive.opaqueCount;
        ++stats.opaqueCount;
        break;
    case OpacityMicromapCpuState::Transparent:
        ++primitive.transparentCount;
        ++stats.transparentCount;
        break;
    case OpacityMicromapCpuState::Unknown:
        ++primitive.unknownCount;
        ++stats.unknownCount;
        break;
    case OpacityMicromapCpuState::Mixed:
        ++primitive.mixedCount;
        ++stats.mixedCount;
        break;
    }
}

[[nodiscard]] GeneratedPrimitiveStates generatePrimitiveStates(
    const AlphaPrimitiveRequest& request,
    uint32_t subdivisionLevel,
    OpacityMicromapPreprocessStats& stats) {
    GeneratedPrimitiveStates generated;
    generated.primitive.meshIndex = request.meshIndex;
    generated.primitive.primitiveIndex = request.primitiveIndex;
    generated.primitive.materialIndex = request.materialIndex;
    generated.primitive.alphaTextureIndex = request.material.baseColorTextureIndex;
    generated.primitive.firstIndex = request.primitive->firstIndex;
    generated.primitive.subdivisionLevel = subdivisionLevel;
    generated.primitive.triangleCount = request.primitive->indexCount / 3u;

    const uint32_t segments = 1u << subdivisionLevel;
    const uint32_t microTrianglesPerTriangle = segments * segments;
    generated.primitive.stateCount = generated.primitive.triangleCount * microTrianglesPerTriangle;
    generated.states.reserve(generated.primitive.stateCount);

    const bool readableTexture = request.material.baseColorTextureIndex >= 0 && alphaTextureReadable(request.texture);
    if (request.material.baseColorTextureIndex >= 0) {
        ++stats.alphaTexturePrimitiveCount;
        if (!readableTexture) {
            std::ostringstream warning;
            warning << "Alpha texture for mesh " << request.meshIndex << " primitive " << request.primitiveIndex
                    << " is not CPU-readable; marking OMM micro-triangles unknown";
            stats.warnings.push_back(warning.str());
        }
    } else {
        ++stats.constantAlphaPrimitiveCount;
    }

    for (uint32_t tri = 0; tri < generated.primitive.triangleCount; ++tri) {
        const uint32_t indexBase = request.primitive->firstIndex + tri * 3u;
        if (indexBase + 2u >= request.mesh->indices.size()) {
            stats.validationErrorCount++;
            continue;
        }
        const uint32_t i0 = request.mesh->indices[indexBase + 0u];
        const uint32_t i1 = request.mesh->indices[indexBase + 1u];
        const uint32_t i2 = request.mesh->indices[indexBase + 2u];
        if (i0 >= request.mesh->vertices.size() || i1 >= request.mesh->vertices.size() || i2 >= request.mesh->vertices.size()) {
            stats.validationErrorCount++;
            continue;
        }

        const std::array<glm::vec2, 3> uv = {
            request.mesh->vertices[i0].texcoord,
            request.mesh->vertices[i1].texcoord,
            request.mesh->vertices[i2].texcoord,
        };

        auto classifyMicroTriangle = [&](glm::vec3 b0, glm::vec3 b1, glm::vec3 b2) {
            OpacityMicromapCpuState state = OpacityMicromapCpuState::Unknown;
            if (request.material.baseColorTextureIndex < 0) {
                const float alpha = request.material.baseAlpha;
                state = alpha >= request.material.alphaCutoff
                    ? OpacityMicromapCpuState::Opaque
                    : OpacityMicromapCpuState::Transparent;
            } else if (readableTexture) {
                const glm::vec3 bc = (b0 + b1 + b2) / 3.0f;
                const std::array<float, 4> samples = {
                    request.material.baseAlpha * sampleTextureAlpha(request.texture, interpolateUv(uv, b0, request.material.baseColorTextureTransform)),
                    request.material.baseAlpha * sampleTextureAlpha(request.texture, interpolateUv(uv, b1, request.material.baseColorTextureTransform)),
                    request.material.baseAlpha * sampleTextureAlpha(request.texture, interpolateUv(uv, b2, request.material.baseColorTextureTransform)),
                    request.material.baseAlpha * sampleTextureAlpha(request.texture, interpolateUv(uv, bc, request.material.baseColorTextureTransform)),
                };
                state = classifySamples(samples, request.material.alphaCutoff);
            }
            generated.states.push_back(static_cast<uint8_t>(state));
            accumulateState(generated.primitive, stats, state);
        };

        for (uint32_t y = 0; y < segments; ++y) {
            for (uint32_t x = 0; x < segments - y; ++x) {
                const auto bary = [&](uint32_t bx, uint32_t by) {
                    const float u = static_cast<float>(bx) / static_cast<float>(segments);
                    const float v = static_cast<float>(by) / static_cast<float>(segments);
                    return glm::vec3(1.0f - u - v, u, v);
                };
                classifyMicroTriangle(bary(x, y), bary(x + 1u, y), bary(x, y + 1u));
                if (x + y + 1u < segments) {
                    classifyMicroTriangle(bary(x + 1u, y), bary(x + 1u, y + 1u), bary(x, y + 1u));
                }
            }
        }
    }

    if (generated.states.size() != generated.primitive.stateCount) {
        stats.validationErrorCount++;
        generated.primitive.stateCount = static_cast<uint32_t>(generated.states.size());
    }
    return generated;
}

[[nodiscard]] std::string cacheKey(const AlphaPrimitiveRequest& request, uint32_t subdivisionLevel) {
    std::ostringstream stream;
    stream << request.meshIndex << ':' << request.primitiveIndex << ':' << request.materialIndex << ':'
           << request.material.baseColorTextureIndex << ':' << request.material.alphaCutoff << ':'
           << request.material.baseAlpha << ':' << subdivisionLevel;
    return stream.str();
}

void appendGeneratedPrimitive(OpacityMicromapCpuData& data, GeneratedPrimitiveStates generated) {
    generated.primitive.stateOffset = static_cast<uint32_t>(data.microTriangleStates.size());
    data.microTriangleStates.insert(data.microTriangleStates.end(), generated.states.begin(), generated.states.end());
    data.primitives.push_back(generated.primitive);
}

void finalizeStats(OpacityMicromapCpuData& data) {
    data.stats.generatedPrimitiveCount = static_cast<uint32_t>(data.primitives.size());
    data.stats.microTriangleCount = static_cast<uint32_t>(data.microTriangleStates.size());
    data.stats.dataBytes = static_cast<uint64_t>(data.microTriangleStates.size()) * sizeof(uint8_t) +
        static_cast<uint64_t>(data.primitives.size()) * sizeof(OpacityMicromapPrimitiveCpuData);
}

[[nodiscard]] int32_t materialIndexForHandle(const SceneAsset& scene, MaterialAssetHandle handle) {
    if (!handle.valid()) {
        return -1;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(scene.materials.size()); ++i) {
        if (scene.materials[i].index == handle.index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

[[nodiscard]] int32_t textureIndexForHandle(const SceneAsset& scene, TextureAssetHandle handle) {
    if (!handle.valid()) {
        return -1;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(scene.textures.size()); ++i) {
        if (scene.textures[i].index == handle.index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

[[nodiscard]] AlphaTextureView textureView(const TextureAsset* texture) {
    AlphaTextureView view;
    if (texture == nullptr) {
        return view;
    }
    view.bytes = texture->rgba8.empty() ? nullptr : texture->rgba8.data();
    view.byteCount = texture->rgba8.size();
    view.width = texture->width;
    view.height = texture->height;
    view.format = texture->format;
    view.compressed = texture->isCompressed;
    view.fallback = texture->fallback;
    view.sampler = texture->sampler;
    return view;
}

[[nodiscard]] AlphaTextureView textureView(const CachedTextureData* texture) {
    AlphaTextureView view;
    if (texture == nullptr) {
        return view;
    }
    view.bytes = texture->rgba8.empty() ? nullptr : texture->rgba8.data();
    view.byteCount = texture->rgba8.size();
    view.width = texture->width;
    view.height = texture->height;
    view.format = static_cast<VkFormat>(texture->format);
    view.compressed = texture->isCompressed;
    view.fallback = texture->fallback;
    view.sampler = TextureSamplerDesc{
        .minFilter = static_cast<TextureFilter>(texture->minFilter),
        .magFilter = static_cast<TextureFilter>(texture->magFilter),
        .wrapS = static_cast<TextureWrap>(texture->wrapS),
        .wrapT = static_cast<TextureWrap>(texture->wrapT),
    };
    return view;
}

template <typename RequestBuilder>
[[nodiscard]] OpacityMicromapCpuData generateFromRequests(uint32_t subdivisionLevel, RequestBuilder&& buildRequests) {
    OpacityMicromapCpuData data;
    const auto start = std::chrono::steady_clock::now();
    subdivisionLevel = clampSubdivisionLevel(subdivisionLevel, data.stats);
    data.stats.subdivisionLevel = subdivisionLevel;

    std::vector<AlphaPrimitiveRequest> requests = buildRequests(data.stats);
    data.stats.eligiblePrimitiveCount = static_cast<uint32_t>(requests.size());

    std::unordered_map<std::string, GeneratedPrimitiveStates> cache;
    cache.reserve(requests.size());
    for (const AlphaPrimitiveRequest& request : requests) {
        const std::string key = cacheKey(request, subdivisionLevel);
        auto it = cache.find(key);
        if (it == cache.end()) {
            GeneratedPrimitiveStates generated = generatePrimitiveStates(request, subdivisionLevel, data.stats);
            it = cache.emplace(key, std::move(generated)).first;
        } else {
            ++data.stats.cacheHitCount;
        }
        appendGeneratedPrimitive(data, it->second);
        data.stats.totalTriangleCount += it->second.primitive.triangleCount;
    }

    data.stats.cacheEntryCount = static_cast<uint32_t>(cache.size());
    finalizeStats(data);
    const auto end = std::chrono::steady_clock::now();
    data.stats.preprocessingMs = std::chrono::duration<double, std::milli>(end - start).count();
    return data;
}

[[nodiscard]] std::array<uint8_t, 4> colorForState(uint8_t state) {
    switch (static_cast<OpacityMicromapCpuState>(state)) {
    case OpacityMicromapCpuState::Transparent: return {0, 0, 0, 255};
    case OpacityMicromapCpuState::Opaque: return {255, 255, 255, 255};
    case OpacityMicromapCpuState::Unknown: return {60, 120, 255, 255};
    case OpacityMicromapCpuState::Mixed: return {255, 210, 40, 255};
    }
    return {255, 0, 255, 255};
}

} // namespace

OpacityMicromapCpuData generateOpacityMicromapData(
    const SceneAsset& scene,
    const AssetManager& assets,
    uint32_t subdivisionLevel) {
    return generateFromRequests(subdivisionLevel, [&](OpacityMicromapPreprocessStats& stats) {
        std::vector<AlphaPrimitiveRequest> requests;
        for (uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(scene.meshes.size()); ++meshIndex) {
            const MeshAsset* mesh = assets.mesh(scene.meshes[meshIndex]);
            if (mesh == nullptr) {
                continue;
            }
            for (uint32_t primitiveIndex = 0; primitiveIndex < static_cast<uint32_t>(mesh->primitives.size()); ++primitiveIndex) {
                const MeshPrimitiveAsset& primitive = mesh->primitives[primitiveIndex];
                const int32_t sceneMaterialIndex = materialIndexForHandle(scene, primitive.material);
                const MaterialAsset* material = assets.material(primitive.material);
                if (primitiveAlphaClassForMaterial(material) != kPrimitiveAlphaClassAlphaTested) {
                    continue;
                }
                AlphaPrimitiveRequest request;
                request.meshIndex = meshIndex;
                request.primitiveIndex = primitiveIndex;
                request.materialIndex = sceneMaterialIndex >= 0 ? static_cast<uint32_t>(sceneMaterialIndex) : 0u;
                request.primitive = &primitive;
                request.mesh = mesh;
                if (material != nullptr) {
                    request.material.alphaMode = material->alphaMode;
                    request.material.alphaCutoff = material->alphaCutoff;
                    request.material.baseAlpha = material->baseColorFactor.a;
                    request.material.baseColorTextureIndex = textureIndexForHandle(scene, material->baseColorTexture);
                    request.material.baseColorTextureTransform = material->baseColorTextureTransform;
                    request.texture = textureView(assets.texture(material->baseColorTexture));
                }
                requests.push_back(request);
            }
        }
        if (requests.empty()) {
            stats.warnings.push_back("No alpha-tested primitives found for OMM preprocessing");
        }
        return requests;
    });
}

OpacityMicromapCpuData generateOpacityMicromapData(const CachedScene& cached, uint32_t subdivisionLevel) {
    AssetManager assets;
    SceneAsset scene;
    scene.name = cached.name;

    scene.textures.reserve(cached.textures.size());
    for (const CachedTextureData& cachedTexture : cached.textures) {
        TextureAsset texture;
        texture.name = cachedTexture.name;
        texture.sourcePath = cachedTexture.sourcePath;
        texture.width = cachedTexture.width;
        texture.height = cachedTexture.height;
        texture.channels = cachedTexture.channels;
        texture.mipLevels = cachedTexture.mipLevels;
        texture.srgb = cachedTexture.srgb;
        texture.fallback = cachedTexture.fallback;
        texture.isCompressed = cachedTexture.isCompressed;
        texture.linearColorSpace = cachedTexture.linearColorSpace;
        texture.format = static_cast<VkFormat>(cachedTexture.format);
        texture.compressedFormat = static_cast<VkFormat>(cachedTexture.compressedFormat);
        texture.rgba8 = cachedTexture.rgba8;
        texture.mipData = cachedTexture.mipData;
        texture.sampler = TextureSamplerDesc{
            .minFilter = static_cast<TextureFilter>(cachedTexture.minFilter),
            .magFilter = static_cast<TextureFilter>(cachedTexture.magFilter),
            .wrapS = static_cast<TextureWrap>(cachedTexture.wrapS),
            .wrapT = static_cast<TextureWrap>(cachedTexture.wrapT),
        };
        scene.textures.push_back(assets.addTexture(std::move(texture)));
    }

    scene.materials.reserve(cached.materials.size());
    for (const CachedMaterialData& cachedMaterial : cached.materials) {
        MaterialAsset material;
        material.name = cachedMaterial.name;
        material.baseColorFactor = cachedMaterial.baseColorFactor;
        material.alphaCutoff = cachedMaterial.alphaCutoff;
        material.alphaMode = cachedMaterial.alphaMode;
        material.doubleSided = cachedMaterial.doubleSided;
        material.baseColorTexture = cachedMaterial.baseColorTextureIndex >= 0 && static_cast<size_t>(cachedMaterial.baseColorTextureIndex) < scene.textures.size()
            ? scene.textures[static_cast<size_t>(cachedMaterial.baseColorTextureIndex)]
            : TextureAssetHandle{};
        material.baseColorTextureTransform = cachedMaterial.baseColorTextureTransform;
        scene.materials.push_back(assets.addMaterial(std::move(material)));
    }

    scene.meshes.reserve(cached.meshes.size());
    for (const CachedMeshData& cachedMesh : cached.meshes) {
        MeshAsset mesh;
        mesh.name = cachedMesh.name;
        mesh.vertices = cachedMesh.vertices;
        mesh.indices = cachedMesh.indices;
        mesh.primitives.reserve(cachedMesh.primitives.size());
        for (const CachedPrimitiveData& cachedPrimitive : cachedMesh.primitives) {
            MeshPrimitiveAsset primitive;
            primitive.firstVertex = cachedPrimitive.firstVertex;
            primitive.vertexCount = cachedPrimitive.vertexCount;
            primitive.firstIndex = cachedPrimitive.firstIndex;
            primitive.indexCount = cachedPrimitive.indexCount;
            if (cachedPrimitive.materialIndex >= 0 && static_cast<size_t>(cachedPrimitive.materialIndex) < scene.materials.size()) {
                primitive.material = scene.materials[static_cast<size_t>(cachedPrimitive.materialIndex)];
            }
            updatePrimitiveAlphaClassification(primitive, assets.material(primitive.material));
            mesh.primitives.push_back(primitive);
        }
        scene.meshes.push_back(assets.addMesh(std::move(mesh)));
    }

    OpacityMicromapCpuData data = generateOpacityMicromapData(scene, assets, subdivisionLevel);
    if (!data.stats.warnings.empty() && data.stats.warnings.front() == "No alpha-tested primitives found for OMM preprocessing") {
        data.stats.warnings.front() = "No alpha-tested cached primitives found for OMM preprocessing";
    }
    return data;
}

bool writeOpacityMicromapDebugImages(const OpacityMicromapCpuData& data, const std::filesystem::path& directory) {
    if (data.primitives.empty() || data.microTriangleStates.empty()) {
        return false;
    }

    std::filesystem::create_directories(directory);
    uint32_t maxStateCount = 1u;
    for (const OpacityMicromapPrimitiveCpuData& primitive : data.primitives) {
        maxStateCount = std::max(maxStateCount, primitive.stateCount);
    }

    const uint32_t width = std::min<uint32_t>(std::max(maxStateCount, 1u), 1024u);
    const uint32_t rowScale = 4u;
    const uint32_t height = std::max<uint32_t>(1u, static_cast<uint32_t>(data.primitives.size()) * rowScale);
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u, 0u);

    for (uint32_t row = 0; row < static_cast<uint32_t>(data.primitives.size()); ++row) {
        const OpacityMicromapPrimitiveCpuData& primitive = data.primitives[row];
        if (primitive.stateCount == 0 || primitive.stateOffset >= data.microTriangleStates.size()) {
            continue;
        }
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t local = std::min<uint32_t>(
                static_cast<uint32_t>((static_cast<uint64_t>(x) * primitive.stateCount) / width),
                primitive.stateCount - 1u);
            const uint32_t stateIndex = primitive.stateOffset + local;
            const std::array<uint8_t, 4> color = stateIndex < data.microTriangleStates.size()
                ? colorForState(data.microTriangleStates[stateIndex])
                : std::array<uint8_t, 4>{255, 0, 255, 255};
            for (uint32_t y = 0; y < rowScale; ++y) {
                const size_t dst = (static_cast<size_t>(row) * rowScale + y) * width * 4u + static_cast<size_t>(x) * 4u;
                pixels[dst + 0u] = color[0];
                pixels[dst + 1u] = color[1];
                pixels[dst + 2u] = color[2];
                pixels[dst + 3u] = color[3];
            }
        }
    }

    const std::filesystem::path output = directory / "opacity_micromap_microtriangles.png";
    return stbi_write_png(output.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels.data(), static_cast<int>(width * 4u)) != 0;
}

} // namespace rtv
