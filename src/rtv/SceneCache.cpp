#include "rtv/SceneCache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace rtv {

namespace {

constexpr uint32_t kCacheMagic = 0x53434E45;
constexpr uint32_t kCacheVersion = 53;

uint64_t fnv1a64(const uint8_t* data, size_t len) {
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

bool readUint32(std::FILE* file, uint32_t& out) {
    return std::fread(&out, sizeof(out), 1, file) == 1;
}

bool readInt32(std::FILE* file, int32_t& out) {
    return std::fread(&out, sizeof(out), 1, file) == 1;
}

bool readUint64(std::FILE* file, uint64_t& out) {
    return std::fread(&out, sizeof(out), 1, file) == 1;
}

bool readFloat(std::FILE* file, float& out) {
    return std::fread(&out, sizeof(out), 1, file) == 1;
}

bool readBytes(std::FILE* file, void* buffer, size_t count) {
    return std::fread(buffer, 1, count, file) == count;
}

bool writeUint32(std::FILE* file, uint32_t value) {
    return std::fwrite(&value, sizeof(value), 1, file) == 1;
}

bool writeInt32(std::FILE* file, int32_t value) {
    return std::fwrite(&value, sizeof(value), 1, file) == 1;
}

bool writeUint64(std::FILE* file, uint64_t value) {
    return std::fwrite(&value, sizeof(value), 1, file) == 1;
}

bool writeFloat(std::FILE* file, float value) {
    return std::fwrite(&value, sizeof(value), 1, file) == 1;
}

bool writeBytes(std::FILE* file, const void* buffer, size_t count) {
    return std::fwrite(buffer, 1, count, file) == count;
}

bool writeVec3Vector(std::FILE* file, const std::vector<glm::vec3>& values) {
    const uint32_t count = static_cast<uint32_t>(values.size());
    if (!writeUint32(file, count)) {
        return false;
    }
    return count == 0 || writeBytes(file, values.data(), sizeof(glm::vec3) * count);
}

bool readVec3Vector(std::FILE* file, std::vector<glm::vec3>& values) {
    uint32_t count = 0;
    if (!readUint32(file, count)) {
        return false;
    }
    values.resize(count);
    return count == 0 || readBytes(file, values.data(), sizeof(glm::vec3) * count);
}

bool writeFloatVector(std::FILE* file, const std::vector<float>& values) {
    const uint32_t count = static_cast<uint32_t>(values.size());
    if (!writeUint32(file, count)) {
        return false;
    }
    return count == 0 || writeBytes(file, values.data(), sizeof(float) * count);
}

bool readFloatVector(std::FILE* file, std::vector<float>& values) {
    uint32_t count = 0;
    if (!readUint32(file, count)) {
        return false;
    }
    values.resize(count);
    return count == 0 || readBytes(file, values.data(), sizeof(float) * count);
}

bool writeUint32Vector(std::FILE* file, const std::vector<uint32_t>& values) {
    const uint32_t count = static_cast<uint32_t>(values.size());
    if (!writeUint32(file, count)) {
        return false;
    }
    return count == 0 || writeBytes(file, values.data(), sizeof(uint32_t) * count);
}

bool readUint32Vector(std::FILE* file, std::vector<uint32_t>& values) {
    uint32_t count = 0;
    if (!readUint32(file, count)) {
        return false;
    }
    values.resize(count);
    return count == 0 || readBytes(file, values.data(), sizeof(uint32_t) * count);
}

bool writeMat4Vector(std::FILE* file, const std::vector<glm::mat4>& values) {
    const uint32_t count = static_cast<uint32_t>(values.size());
    if (!writeUint32(file, count)) {
        return false;
    }
    return count == 0 || writeBytes(file, values.data(), sizeof(glm::mat4) * count);
}

bool readMat4Vector(std::FILE* file, std::vector<glm::mat4>& values) {
    uint32_t count = 0;
    if (!readUint32(file, count)) {
        return false;
    }
    values.resize(count);
    return count == 0 || readBytes(file, values.data(), sizeof(glm::mat4) * count);
}

} // namespace

uint64_t SceneCache::hashPath(const std::filesystem::path& path) {
    auto canonical = path.lexically_normal().string();
    return fnv1a64(reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
}

uint64_t SceneCache::fileMtime(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return 0;
    }
    return static_cast<uint64_t>(std::filesystem::last_write_time(path).time_since_epoch().count());
}

std::filesystem::path SceneCache::cachePathFor(const std::filesystem::path& gltfPath) {
    auto stem = gltfPath.stem();
    return gltfPath.parent_path() / (stem.string() + ".cache.bin");
}

bool SceneCache::isCacheValid(const std::filesystem::path& gltfPath, const std::filesystem::path& cachePath) {
    if (!std::filesystem::exists(cachePath)) {
        return false;
    }
    if (!std::filesystem::exists(gltfPath)) {
        return false;
    }

    uint64_t gltfMtime = fileMtime(gltfPath);
    uint64_t cacheMtime = fileMtime(cachePath);
    if (gltfMtime > cacheMtime) {
        return false;
    }

    std::FILE* file = nullptr;
    fopen_s(&file, cachePath.string().c_str(), "rb");
    if (file == nullptr) {
        return false;
    }

    uint32_t magic = 0, version = 0;
    uint64_t storedPathHash = 0, storedSourceMtime = 0, storedSourceBinMtime = 0;

    if (!readUint32(file, magic) || magic != kCacheMagic) {
        std::fclose(file);
        return false;
    }
    if (!readUint32(file, version) || version != kCacheVersion) {
        std::fclose(file);
        return false;
    }
    if (!readUint64(file, storedPathHash)) {
        std::fclose(file);
        return false;
    }
    if (!readUint64(file, storedSourceMtime)) {
        std::fclose(file);
        return false;
    }
    if (!readUint64(file, storedSourceBinMtime)) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);

    std::filesystem::path derivedCachePath = cachePathFor(gltfPath);
    uint64_t expectedPathHash = hashPath(derivedCachePath.parent_path() / derivedCachePath.stem().string());
    if (storedPathHash != expectedPathHash) {
        return false;
    }

    return true;
}

bool SceneCache::readString(std::FILE* file, std::string& out) {
    uint32_t length = 0;
    if (!readUint32(file, length)) {
        return false;
    }
    out.resize(length);
    if (length > 0) {
        if (std::fread(out.data(), 1, length, file) != length) {
            return false;
        }
    }
    return true;
}

void SceneCache::writeString(std::FILE* file, const std::string& str) {
    uint32_t length = static_cast<uint32_t>(str.size());
    writeUint32(file, length);
    if (length > 0) {
        writeBytes(file, str.data(), length);
    }
}

bool SceneCache::save(const std::filesystem::path& cachePath, const CachedScene& scene) {
    std::FILE* file = nullptr;
    fopen_s(&file, cachePath.string().c_str(), "wb");
    if (file == nullptr) {
        std::cerr << "SceneCache: Failed to create cache file: " << cachePath.string() << "\n";
        return false;
    }

    if (!writeUint32(file, kCacheMagic)) {
        std::fclose(file);
        return false;
    }
    if (!writeUint32(file, kCacheVersion)) {
        std::fclose(file);
        return false;
    }
    uint64_t pathHash = hashPath(cachePath.parent_path() / (cachePath.stem().string()));
    if (!writeUint64(file, pathHash)) {
        std::fclose(file);
        return false;
    }
    if (!writeUint64(file, scene.sourceMtime)) {
        std::fclose(file);
        return false;
    }
    if (!writeUint64(file, scene.sourceBinMtime)) {
        std::fclose(file);
        return false;
    }
    writeString(file, scene.name);

    uint32_t textureCount = static_cast<uint32_t>(scene.textures.size());
    if (!writeUint32(file, textureCount)) {
        std::fclose(file);
        return false;
    }
    for (const auto& tex : scene.textures) {
        writeString(file, tex.name);
        writeString(file, tex.sourcePath);
        writeUint32(file, tex.width);
        writeUint32(file, tex.height);
        writeUint32(file, tex.channels);
        writeUint32(file, tex.sourceArrayLayers);
        writeUint32(file, tex.sourceDepth);
        writeUint32(file, tex.sourceFaceCount);
        writeUint32(file, tex.sourceIsCubemap ? 1u : 0u);
        writeInt32(file, tex.mipLevels);
        writeUint32(file, tex.srgb ? 1u : 0u);
        writeUint32(file, tex.fallback ? 1u : 0u);
        writeUint32(file, tex.minFilter);
        writeUint32(file, tex.magFilter);
        writeUint32(file, tex.wrapS);
        writeUint32(file, tex.wrapT);
        writeUint32(file, tex.isCompressed ? 1u : 0u);
        writeUint32(file, tex.linearColorSpace ? 1u : 0u);
        writeUint32(file, tex.format);
        writeUint32(file, tex.compressedFormat);
        writeUint32(file, static_cast<uint32_t>(tex.rgba8.size()));
        writeBytes(file, tex.rgba8.data(), tex.rgba8.size());
        const uint32_t mipDataCount = static_cast<uint32_t>(tex.mipData.size());
        writeUint32(file, mipDataCount);
        if (mipDataCount > 0) {
            writeBytes(file, tex.mipData.data(), sizeof(TextureMipLevel) * mipDataCount);
        }
    }

    uint32_t materialCount = static_cast<uint32_t>(scene.materials.size());
    if (!writeUint32(file, materialCount)) {
        std::fclose(file);
        return false;
    }
    for (const auto& mat : scene.materials) {
        writeString(file, mat.name);
        writeBytes(file, &mat.baseColorFactor, sizeof(mat.baseColorFactor));
        writeBytes(file, &mat.emissiveFactor, sizeof(mat.emissiveFactor));
        writeFloat(file, mat.metallicFactor);
        writeFloat(file, mat.roughnessFactor);
        writeFloat(file, mat.iorFactor);
        writeFloat(file, mat.alphaCutoff);
        writeUint32(file, mat.alphaMode);
        writeUint32(file, mat.doubleSided);
        writeUint32(file, mat.hasIor);
        writeUint32(file, mat.hasClearcoat);
        writeFloat(file, mat.clearcoatFactor);
        writeFloat(file, mat.clearcoatRoughnessFactor);
        writeUint32(file, mat.hasTransmission);
        writeFloat(file, mat.transmissionFactor);
        writeUint32(file, mat.hasVolume);
        writeFloat(file, mat.volumeThicknessFactor);
        writeFloat(file, mat.volumeAttenuationDistance);
        writeBytes(file, &mat.volumeAttenuationColor, sizeof(mat.volumeAttenuationColor));
        writeInt32(file, mat.nestedPriority);
        writeUint32(file, mat.hasDispersion);
        writeFloat(file, mat.dispersionFactor);
        writeUint32(file, mat.hasSpecular);
        writeFloat(file, mat.specularFactor);
        writeBytes(file, &mat.specularColorFactor, sizeof(mat.specularColorFactor));
        writeUint32(file, mat.hasSheen);
        writeBytes(file, &mat.sheenColorFactor, sizeof(mat.sheenColorFactor));
        writeFloat(file, mat.sheenRoughnessFactor);
        writeUint32(file, mat.hasIridescence);
        writeFloat(file, mat.iridescenceFactor);
        writeFloat(file, mat.iridescenceIor);
        writeFloat(file, mat.iridescenceThicknessMinimum);
        writeFloat(file, mat.iridescenceThicknessMaximum);
        writeUint32(file, mat.hasEmissiveStrength);
        writeFloat(file, mat.emissiveStrength);
        writeUint32(file, mat.hasAnisotropy);
        writeFloat(file, mat.anisotropyStrength);
        writeFloat(file, mat.anisotropyRotation);
        writeFloat(file, mat.occlusionStrength);
        writeUint32(file, mat.useConductorOptics);
        writeBytes(file, &mat.conductorEta, sizeof(mat.conductorEta));
        writeBytes(file, &mat.conductorK, sizeof(mat.conductorK));
        writeInt32(file, mat.baseColorTextureIndex);
        writeInt32(file, mat.normalTextureIndex);
        writeInt32(file, mat.metallicRoughnessTextureIndex);
        writeInt32(file, mat.emissiveTextureIndex);
        writeInt32(file, mat.clearcoatTextureIndex);
        writeInt32(file, mat.clearcoatRoughnessTextureIndex);
        writeInt32(file, mat.clearcoatNormalTextureIndex);
        writeInt32(file, mat.transmissionTextureIndex);
        writeInt32(file, mat.volumeThicknessTextureIndex);
        writeInt32(file, mat.specularTextureIndex);
        writeInt32(file, mat.specularColorTextureIndex);
        writeInt32(file, mat.sheenColorTextureIndex);
        writeInt32(file, mat.sheenRoughnessTextureIndex);
        writeInt32(file, mat.iridescenceTextureIndex);
        writeInt32(file, mat.iridescenceThicknessTextureIndex);
        writeInt32(file, mat.anisotropyTextureIndex);
        writeInt32(file, mat.occlusionTextureIndex);
        writeInt32(file, mat.opacityTextureIndex);
        writeInt32(file, mat.heightTextureIndex);
        writeFloat(file, mat.heightScale);
        writeBytes(file, &mat.baseColorTextureTransform, sizeof(mat.baseColorTextureTransform));
        writeBytes(file, &mat.metallicRoughnessTextureTransform, sizeof(mat.metallicRoughnessTextureTransform));
        writeBytes(file, &mat.normalTextureTransform, sizeof(mat.normalTextureTransform));
        writeBytes(file, &mat.emissiveTextureTransform, sizeof(mat.emissiveTextureTransform));
        writeBytes(file, &mat.occlusionTextureTransform, sizeof(mat.occlusionTextureTransform));
        writeBytes(file, &mat.clearcoatTextureTransform, sizeof(mat.clearcoatTextureTransform));
        writeBytes(file, &mat.clearcoatRoughnessTextureTransform, sizeof(mat.clearcoatRoughnessTextureTransform));
        writeBytes(file, &mat.clearcoatNormalTextureTransform, sizeof(mat.clearcoatNormalTextureTransform));
        writeBytes(file, &mat.transmissionTextureTransform, sizeof(mat.transmissionTextureTransform));
        writeBytes(file, &mat.volumeThicknessTextureTransform, sizeof(mat.volumeThicknessTextureTransform));
        writeBytes(file, &mat.specularTextureTransform, sizeof(mat.specularTextureTransform));
        writeBytes(file, &mat.specularColorTextureTransform, sizeof(mat.specularColorTextureTransform));
        writeBytes(file, &mat.sheenColorTextureTransform, sizeof(mat.sheenColorTextureTransform));
        writeBytes(file, &mat.sheenRoughnessTextureTransform, sizeof(mat.sheenRoughnessTextureTransform));
        writeBytes(file, &mat.iridescenceTextureTransform, sizeof(mat.iridescenceTextureTransform));
        writeBytes(file, &mat.iridescenceThicknessTextureTransform, sizeof(mat.iridescenceThicknessTextureTransform));
        writeBytes(file, &mat.anisotropyTextureTransform, sizeof(mat.anisotropyTextureTransform));
        writeUint32(file, mat.materialWorkflow);
        writeUint32(file, mat.normalMapConvention);
        writeUint32(file, mat.specularTextureAlphaMode);
        writeUint32(file, mat.shaderCompatibilityMask);
    }

    uint32_t materialVariantCount = static_cast<uint32_t>(scene.materialVariants.size());
    if (!writeUint32(file, materialVariantCount)) {
        std::fclose(file);
        return false;
    }
    for (const std::string& variantName : scene.materialVariants) {
        writeString(file, variantName);
    }

    uint32_t meshCount = static_cast<uint32_t>(scene.meshes.size());
    if (!writeUint32(file, meshCount)) {
        std::fclose(file);
        return false;
    }
    for (const auto& mesh : scene.meshes) {
        writeString(file, mesh.name);
        uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
        uint32_t primitiveCount = static_cast<uint32_t>(mesh.primitives.size());
        writeUint32(file, vertexCount);
        writeUint32(file, indexCount);
        writeUint32(file, primitiveCount);
        if (!writeFloatVector(file, mesh.defaultMorphWeights)) {
            std::fclose(file);
            return false;
        }
        if (vertexCount > 0) {
            writeBytes(file, mesh.vertices.data(), sizeof(MeshVertex) * vertexCount);
        }
        if (indexCount > 0) {
            writeBytes(file, mesh.indices.data(), sizeof(uint32_t) * indexCount);
        }
        for (const auto& prim : mesh.primitives) {
            writeUint32(file, prim.firstVertex);
            writeUint32(file, prim.vertexCount);
            writeUint32(file, prim.firstIndex);
            writeUint32(file, prim.indexCount);
            writeInt32(file, prim.materialIndex);
            uint32_t morphTargetCount = static_cast<uint32_t>(prim.morphTargets.size());
            writeUint32(file, morphTargetCount);
            for (const auto& target : prim.morphTargets) {
                writeString(file, target.name);
                if (!writeVec3Vector(file, target.positionDeltas) ||
                    !writeVec3Vector(file, target.normalDeltas) ||
                    !writeVec3Vector(file, target.tangentDeltas)) {
                    std::fclose(file);
                    return false;
                }
            }
            uint32_t variantCount = static_cast<uint32_t>(prim.materialVariants.size());
            writeUint32(file, variantCount);
            for (const auto& variant : prim.materialVariants) {
                writeUint32(file, variant.variantIndex);
                writeString(file, variant.variantName);
                writeInt32(file, variant.materialIndex);
            }
        }
    }

    uint32_t nodeCount = static_cast<uint32_t>(scene.nodes.size());
    if (!writeUint32(file, nodeCount)) {
        std::fclose(file);
        return false;
    }
    for (const auto& node : scene.nodes) {
        writeString(file, node.name);
        writeBytes(file, &node.transform, sizeof(node.transform));
        writeInt32(file, node.meshIndex);
        if (!writeFloatVector(file, node.morphWeights)) {
            std::fclose(file);
            return false;
        }
        writeInt32(file, node.skinIndex);
        writeUint32(file, node.hasCamera);
        writeUint32(file, node.cameraProjection);
        writeFloat(file, node.cameraYfov);
        writeFloat(file, node.cameraAspectRatio);
        writeFloat(file, node.cameraOrthoXmag);
        writeFloat(file, node.cameraOrthoYmag);
        writeFloat(file, node.cameraNear);
        writeFloat(file, node.cameraFar);
        writeInt32(file, node.parentIndex);
        uint32_t childCount = static_cast<uint32_t>(node.children.size());
        writeUint32(file, childCount);
        if (childCount > 0) {
            writeBytes(file, node.children.data(), sizeof(uint32_t) * childCount);
        }
    }

    uint32_t skinCount = static_cast<uint32_t>(scene.skins.size());
    if (!writeUint32(file, skinCount)) {
        std::fclose(file);
        return false;
    }
    for (const auto& skin : scene.skins) {
        writeString(file, skin.name);
        writeInt32(file, skin.skeletonRoot);
        if (!writeUint32Vector(file, skin.joints) || !writeMat4Vector(file, skin.inverseBindMatrices)) {
            std::fclose(file);
            return false;
        }
    }

    uint32_t sceneLightCount = static_cast<uint32_t>(scene.sceneLights.size());
    if (!writeUint32(file, sceneLightCount)) {
        std::fclose(file);
        return false;
    }
    for (const auto& light : scene.sceneLights) {
        writeUint32(file, light.type);
        writeBytes(file, &light.transform, sizeof(light.transform));
        writeBytes(file, &light.color, sizeof(light.color));
        writeFloat(file, light.intensity);
        writeFloat(file, light.sizeOrRadius);
        writeFloat(file, light.innerConeRadians);
        writeFloat(file, light.outerConeRadians);
        writeUint32(file, light.enabled);
        writeInt32(file, light.nodeIndex);
    }

    uint32_t rootNodeCount = static_cast<uint32_t>(scene.rootNodes.size());
    if (!writeUint32(file, rootNodeCount)) {
        std::fclose(file);
        return false;
    }
    if (rootNodeCount > 0) {
        writeBytes(file, scene.rootNodes.data(), sizeof(uint32_t) * rootNodeCount);
    }

    uint32_t meshGpuRecordCount = static_cast<uint32_t>(scene.meshGpuRecords.size());
    if (!writeUint32(file, meshGpuRecordCount)) { std::fclose(file); return false; }
    for (const auto& rec : scene.meshGpuRecords) {
        writeBytes(file, &rec.vertexIndexData, sizeof(rec.vertexIndexData));
        writeBytes(file, &rec.primitiveData, sizeof(rec.primitiveData));
        writeBytes(file, &rec.bvhData, sizeof(rec.bvhData));
        writeBytes(file, &rec.flags, sizeof(rec.flags));
        writeUint32(file, static_cast<uint32_t>(rec.localBvh.packedNodes.size()));
        if (!rec.localBvh.packedNodes.empty()) {
            writeBytes(file, rec.localBvh.packedNodes.data(), sizeof(glm::vec4) * rec.localBvh.packedNodes.size());
        }
        writeUint32(file, static_cast<uint32_t>(rec.localBvh.triangleData.size()));
        if (!rec.localBvh.triangleData.empty()) {
            writeBytes(file, rec.localBvh.triangleData.data(), sizeof(glm::vec4) * rec.localBvh.triangleData.size());
        }
        writeUint32(file, rec.localBvh.triangleCount);
        writeUint32(file, rec.localBvh.leafTriangleCount);
    }

    uint32_t primRecordCount = static_cast<uint32_t>(scene.primitiveRecords.size());
    if (!writeUint32(file, primRecordCount)) { std::fclose(file); return false; }
    if (!scene.primitiveRecords.empty()) {
        writeBytes(file, scene.primitiveRecords.data(), sizeof(CachedPrimitiveRecord) * primRecordCount);
    }

    uint32_t instRecordCount = static_cast<uint32_t>(scene.instanceRecords.size());
    if (!writeUint32(file, instRecordCount)) { std::fclose(file); return false; }
    if (!scene.instanceRecords.empty()) {
        writeBytes(file, scene.instanceRecords.data(), sizeof(CachedInstanceRecord) * instRecordCount);
    }

    uint32_t instBoundsCount = static_cast<uint32_t>(scene.instanceBounds.size());
    if (!writeUint32(file, instBoundsCount)) { std::fclose(file); return false; }
    if (!scene.instanceBounds.empty()) {
        writeBytes(file, scene.instanceBounds.data(), sizeof(CachedInstanceBoundsRecord) * instBoundsCount);
    }

    uint32_t tlasNodeCount = static_cast<uint32_t>(scene.tlasNodes.size());
    if (!writeUint32(file, tlasNodeCount)) { std::fclose(file); return false; }
    if (!scene.tlasNodes.empty()) {
        writeBytes(file, scene.tlasNodes.data(), sizeof(glm::vec4) * tlasNodeCount);
    }

    uint32_t tlasInstIndexCount = static_cast<uint32_t>(scene.tlasInstanceIndices.size());
    if (!writeUint32(file, tlasInstIndexCount)) { std::fclose(file); return false; }
    if (!scene.tlasInstanceIndices.empty()) {
        writeBytes(file, scene.tlasInstanceIndices.data(), sizeof(uint32_t) * tlasInstIndexCount);
    }

    uint32_t lightRecordCount = static_cast<uint32_t>(scene.lightRecords.size());
    if (!writeUint32(file, lightRecordCount)) { std::fclose(file); return false; }
    if (!scene.lightRecords.empty()) {
        writeBytes(file, scene.lightRecords.data(), sizeof(CachedLightRecord) * lightRecordCount);
    }

    writeBytes(file, &scene.meshParams, sizeof(scene.meshParams));

    uint32_t depCount = static_cast<uint32_t>(scene.dependencies.size());
    if (!writeUint32(file, depCount)) { std::fclose(file); return false; }
    for (const auto& dep : scene.dependencies) {
        writeString(file, dep.path);
        writeUint64(file, dep.size);
        writeUint64(file, dep.mtime);
    }

    std::fclose(file);
    return true;
}

std::optional<CachedScene> SceneCache::load(const std::filesystem::path& cachePath) {
    std::FILE* file = nullptr;
    fopen_s(&file, cachePath.string().c_str(), "rb");
    if (file == nullptr) {
        return std::nullopt;
    }

    uint32_t magic = 0, version = 0;
    uint64_t pathHash = 0, sourceMtime = 0, sourceBinMtime = 0;

    if (!readUint32(file, magic) || magic != kCacheMagic) {
        std::fclose(file);
        return std::nullopt;
    }
    if (!readUint32(file, version) || version != kCacheVersion) {
        std::fclose(file);
        return std::nullopt;
    }
    if (!readUint64(file, pathHash)) {
        std::fclose(file);
        return std::nullopt;
    }
    if (!readUint64(file, sourceMtime)) {
        std::fclose(file);
        return std::nullopt;
    }
    if (!readUint64(file, sourceBinMtime)) {
        std::fclose(file);
        return std::nullopt;
    }

    CachedScene scene;
    scene.sourceMtime = sourceMtime;
    scene.sourceBinMtime = sourceBinMtime;
    if (!readString(file, scene.name)) {
        std::fclose(file);
        return std::nullopt;
    }

    uint32_t textureCount = 0;
    if (!readUint32(file, textureCount)) {
        std::fclose(file);
        return std::nullopt;
    }
    scene.textures.resize(textureCount);
    for (uint32_t i = 0; i < textureCount; ++i) {
        auto& tex = scene.textures[i];
        if (!readString(file, tex.name)) { std::fclose(file); return std::nullopt; }
        if (!readString(file, tex.sourcePath)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.width)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.height)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.channels)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.sourceArrayLayers)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.sourceDepth)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.sourceFaceCount)) { std::fclose(file); return std::nullopt; }
        uint32_t sourceIsCubemapVal = 0;
        if (!readUint32(file, sourceIsCubemapVal)) { std::fclose(file); return std::nullopt; }
        tex.sourceIsCubemap = sourceIsCubemapVal != 0u;
        int32_t mipLevelsVal = 1;
        if (!readInt32(file, mipLevelsVal)) { std::fclose(file); return std::nullopt; }
        tex.mipLevels = mipLevelsVal;
        uint32_t srgbVal = 0;
        if (!readUint32(file, srgbVal)) { std::fclose(file); return std::nullopt; }
        tex.srgb = srgbVal != 0;
        uint32_t fallbackVal = 0;
        if (!readUint32(file, fallbackVal)) { std::fclose(file); return std::nullopt; }
        tex.fallback = fallbackVal != 0;
        if (!readUint32(file, tex.minFilter)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.magFilter)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.wrapS)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.wrapT)) { std::fclose(file); return std::nullopt; }
        uint32_t isCompressedVal = 0;
        if (!readUint32(file, isCompressedVal)) { std::fclose(file); return std::nullopt; }
        tex.isCompressed = isCompressedVal != 0;
        uint32_t linearColorVal = 0;
        if (!readUint32(file, linearColorVal)) { std::fclose(file); return std::nullopt; }
        tex.linearColorSpace = linearColorVal != 0;
        if (!readUint32(file, tex.format)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, tex.compressedFormat)) { std::fclose(file); return std::nullopt; }
        uint32_t dataByteSize = 0;
        if (!readUint32(file, dataByteSize)) { std::fclose(file); return std::nullopt; }
        tex.rgba8.resize(dataByteSize);
        if (dataByteSize > 0) {
            if (!readBytes(file, tex.rgba8.data(), dataByteSize)) { std::fclose(file); return std::nullopt; }
        }
        uint32_t mipDataCount = 0;
        if (!readUint32(file, mipDataCount)) { std::fclose(file); return std::nullopt; }
        tex.mipData.resize(mipDataCount);
        if (mipDataCount > 0) {
            if (!readBytes(file, tex.mipData.data(), sizeof(TextureMipLevel) * mipDataCount)) { std::fclose(file); return std::nullopt; }
        }
    }

    uint32_t materialCount = 0;
    if (!readUint32(file, materialCount)) {
        std::fclose(file);
        return std::nullopt;
    }
    scene.materials.resize(materialCount);
    for (uint32_t i = 0; i < materialCount; ++i) {
        auto& mat = scene.materials[i];
        if (!readString(file, mat.name)) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.baseColorFactor, sizeof(mat.baseColorFactor))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.emissiveFactor, sizeof(mat.emissiveFactor))) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.metallicFactor)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.roughnessFactor)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.iorFactor)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.alphaCutoff)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.alphaMode)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.doubleSided)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasIor)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasClearcoat)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.clearcoatFactor)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.clearcoatRoughnessFactor)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasTransmission)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.transmissionFactor)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasVolume)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.volumeThicknessFactor)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.volumeAttenuationDistance)) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.volumeAttenuationColor, sizeof(mat.volumeAttenuationColor))) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.nestedPriority)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasDispersion)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.dispersionFactor)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasSpecular)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.specularFactor)) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.specularColorFactor, sizeof(mat.specularColorFactor))) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasSheen)) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.sheenColorFactor, sizeof(mat.sheenColorFactor))) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.sheenRoughnessFactor)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasIridescence)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.iridescenceFactor)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.iridescenceIor)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.iridescenceThicknessMinimum)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.iridescenceThicknessMaximum)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasEmissiveStrength)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.emissiveStrength)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.hasAnisotropy)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.anisotropyStrength)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.anisotropyRotation)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.occlusionStrength)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.useConductorOptics)) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.conductorEta, sizeof(mat.conductorEta))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.conductorK, sizeof(mat.conductorK))) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.baseColorTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.normalTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.metallicRoughnessTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.emissiveTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.clearcoatTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.clearcoatRoughnessTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.clearcoatNormalTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.transmissionTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.volumeThicknessTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.specularTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.specularColorTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.sheenColorTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.sheenRoughnessTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.iridescenceTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.iridescenceThicknessTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.anisotropyTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.occlusionTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.opacityTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.heightTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, mat.heightScale)) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.baseColorTextureTransform, sizeof(mat.baseColorTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.metallicRoughnessTextureTransform, sizeof(mat.metallicRoughnessTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.normalTextureTransform, sizeof(mat.normalTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.emissiveTextureTransform, sizeof(mat.emissiveTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.occlusionTextureTransform, sizeof(mat.occlusionTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.clearcoatTextureTransform, sizeof(mat.clearcoatTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.clearcoatRoughnessTextureTransform, sizeof(mat.clearcoatRoughnessTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.clearcoatNormalTextureTransform, sizeof(mat.clearcoatNormalTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.transmissionTextureTransform, sizeof(mat.transmissionTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.volumeThicknessTextureTransform, sizeof(mat.volumeThicknessTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.specularTextureTransform, sizeof(mat.specularTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.specularColorTextureTransform, sizeof(mat.specularColorTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.sheenColorTextureTransform, sizeof(mat.sheenColorTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.sheenRoughnessTextureTransform, sizeof(mat.sheenRoughnessTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.iridescenceTextureTransform, sizeof(mat.iridescenceTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.iridescenceThicknessTextureTransform, sizeof(mat.iridescenceThicknessTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &mat.anisotropyTextureTransform, sizeof(mat.anisotropyTextureTransform))) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.materialWorkflow)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.normalMapConvention)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.specularTextureAlphaMode)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.shaderCompatibilityMask)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t materialVariantCount = 0;
    if (!readUint32(file, materialVariantCount)) {
        std::fclose(file);
        return std::nullopt;
    }
    scene.materialVariants.resize(materialVariantCount);
    for (std::string& variantName : scene.materialVariants) {
        if (!readString(file, variantName)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t meshCount = 0;
    if (!readUint32(file, meshCount)) {
        std::fclose(file);
        return std::nullopt;
    }
    scene.meshes.resize(meshCount);
    for (uint32_t i = 0; i < meshCount; ++i) {
        auto& mesh = scene.meshes[i];
        if (!readString(file, mesh.name)) { std::fclose(file); return std::nullopt; }
        uint32_t vertexCount = 0, indexCount = 0, primitiveCount = 0;
        if (!readUint32(file, vertexCount)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, indexCount)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, primitiveCount)) { std::fclose(file); return std::nullopt; }
        if (!readFloatVector(file, mesh.defaultMorphWeights)) { std::fclose(file); return std::nullopt; }
        mesh.vertices.resize(vertexCount);
        if (vertexCount > 0) {
            if (!readBytes(file, mesh.vertices.data(), sizeof(MeshVertex) * vertexCount)) { std::fclose(file); return std::nullopt; }
        }
        mesh.indices.resize(indexCount);
        if (indexCount > 0) {
            if (!readBytes(file, mesh.indices.data(), sizeof(uint32_t) * indexCount)) { std::fclose(file); return std::nullopt; }
        }
        mesh.primitives.resize(primitiveCount);
        for (uint32_t p = 0; p < primitiveCount; ++p) {
            auto& prim = mesh.primitives[p];
            if (!readUint32(file, prim.firstVertex)) { std::fclose(file); return std::nullopt; }
            if (!readUint32(file, prim.vertexCount)) { std::fclose(file); return std::nullopt; }
            if (!readUint32(file, prim.firstIndex)) { std::fclose(file); return std::nullopt; }
            if (!readUint32(file, prim.indexCount)) { std::fclose(file); return std::nullopt; }
            if (!readInt32(file, prim.materialIndex)) { std::fclose(file); return std::nullopt; }
            uint32_t morphTargetCount = 0;
            if (!readUint32(file, morphTargetCount)) { std::fclose(file); return std::nullopt; }
            prim.morphTargets.resize(morphTargetCount);
            for (auto& target : prim.morphTargets) {
                if (!readString(file, target.name)) { std::fclose(file); return std::nullopt; }
                if (!readVec3Vector(file, target.positionDeltas)) { std::fclose(file); return std::nullopt; }
                if (!readVec3Vector(file, target.normalDeltas)) { std::fclose(file); return std::nullopt; }
                if (!readVec3Vector(file, target.tangentDeltas)) { std::fclose(file); return std::nullopt; }
            }
            uint32_t variantCount = 0;
            if (!readUint32(file, variantCount)) { std::fclose(file); return std::nullopt; }
            prim.materialVariants.resize(variantCount);
            for (auto& variant : prim.materialVariants) {
                if (!readUint32(file, variant.variantIndex)) { std::fclose(file); return std::nullopt; }
                if (!readString(file, variant.variantName)) { std::fclose(file); return std::nullopt; }
                if (!readInt32(file, variant.materialIndex)) { std::fclose(file); return std::nullopt; }
            }
        }
    }

    uint32_t nodeCount = 0;
    if (!readUint32(file, nodeCount)) {
        std::fclose(file);
        return std::nullopt;
    }
    scene.nodes.resize(nodeCount);
    for (uint32_t i = 0; i < nodeCount; ++i) {
        auto& node = scene.nodes[i];
        if (!readString(file, node.name)) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &node.transform, sizeof(node.transform))) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, node.meshIndex)) { std::fclose(file); return std::nullopt; }
        if (!readFloatVector(file, node.morphWeights)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, node.skinIndex)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, node.hasCamera)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, node.cameraProjection)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, node.cameraYfov)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, node.cameraAspectRatio)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, node.cameraOrthoXmag)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, node.cameraOrthoYmag)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, node.cameraNear)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, node.cameraFar)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, node.parentIndex)) { std::fclose(file); return std::nullopt; }
        uint32_t childCount = 0;
        if (!readUint32(file, childCount)) { std::fclose(file); return std::nullopt; }
        node.children.resize(childCount);
        if (childCount > 0) {
            if (!readBytes(file, node.children.data(), sizeof(uint32_t) * childCount)) { std::fclose(file); return std::nullopt; }
        }
    }

    uint32_t skinCount = 0;
    if (!readUint32(file, skinCount)) {
        std::fclose(file);
        return std::nullopt;
    }
    scene.skins.resize(skinCount);
    for (auto& skin : scene.skins) {
        if (!readString(file, skin.name)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, skin.skeletonRoot)) { std::fclose(file); return std::nullopt; }
        if (!readUint32Vector(file, skin.joints)) { std::fclose(file); return std::nullopt; }
        if (!readMat4Vector(file, skin.inverseBindMatrices)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t sceneLightCount = 0;
    if (!readUint32(file, sceneLightCount)) {
        std::fclose(file);
        return std::nullopt;
    }
    scene.sceneLights.resize(sceneLightCount);
    for (uint32_t i = 0; i < sceneLightCount; ++i) {
        auto& light = scene.sceneLights[i];
        if (!readUint32(file, light.type)) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &light.transform, sizeof(light.transform))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &light.color, sizeof(light.color))) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, light.intensity)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, light.sizeOrRadius)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, light.innerConeRadians)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, light.outerConeRadians)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, light.enabled)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, light.nodeIndex)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t rootNodeCount = 0;
    if (!readUint32(file, rootNodeCount)) {
        std::fclose(file);
        return std::nullopt;
    }
    scene.rootNodes.resize(rootNodeCount);
    if (rootNodeCount > 0) {
        if (!readBytes(file, scene.rootNodes.data(), sizeof(uint32_t) * rootNodeCount)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t meshGpuRecordCount = 0;
    if (!readUint32(file, meshGpuRecordCount)) { std::fclose(file); return std::nullopt; }
    scene.meshGpuRecords.resize(meshGpuRecordCount);
    for (uint32_t i = 0; i < meshGpuRecordCount; ++i) {
        auto& rec = scene.meshGpuRecords[i];
        if (!readBytes(file, &rec.vertexIndexData, sizeof(rec.vertexIndexData))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &rec.primitiveData, sizeof(rec.primitiveData))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &rec.bvhData, sizeof(rec.bvhData))) { std::fclose(file); return std::nullopt; }
        if (!readBytes(file, &rec.flags, sizeof(rec.flags))) { std::fclose(file); return std::nullopt; }
        uint32_t bvhNodeCount = 0, triCount = 0;
        if (!readUint32(file, bvhNodeCount)) { std::fclose(file); return std::nullopt; }
        rec.localBvh.packedNodes.resize(bvhNodeCount);
        if (bvhNodeCount > 0) {
            if (!readBytes(file, rec.localBvh.packedNodes.data(), sizeof(glm::vec4) * bvhNodeCount)) { std::fclose(file); return std::nullopt; }
        }
        if (!readUint32(file, triCount)) { std::fclose(file); return std::nullopt; }
        rec.localBvh.triangleData.resize(triCount);
        if (triCount > 0) {
            if (!readBytes(file, rec.localBvh.triangleData.data(), sizeof(glm::vec4) * triCount)) { std::fclose(file); return std::nullopt; }
        }
        if (!readUint32(file, rec.localBvh.triangleCount)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, rec.localBvh.leafTriangleCount)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t primRecordCount = 0;
    if (!readUint32(file, primRecordCount)) { std::fclose(file); return std::nullopt; }
    scene.primitiveRecords.resize(primRecordCount);
    if (primRecordCount > 0) {
        if (!readBytes(file, scene.primitiveRecords.data(), sizeof(CachedPrimitiveRecord) * primRecordCount)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t instRecordCount = 0;
    if (!readUint32(file, instRecordCount)) { std::fclose(file); return std::nullopt; }
    scene.instanceRecords.resize(instRecordCount);
    if (instRecordCount > 0) {
        if (!readBytes(file, scene.instanceRecords.data(), sizeof(CachedInstanceRecord) * instRecordCount)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t instBoundsCount = 0;
    if (!readUint32(file, instBoundsCount)) { std::fclose(file); return std::nullopt; }
    scene.instanceBounds.resize(instBoundsCount);
    if (instBoundsCount > 0) {
        if (!readBytes(file, scene.instanceBounds.data(), sizeof(CachedInstanceBoundsRecord) * instBoundsCount)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t tlasNodeCount = 0;
    if (!readUint32(file, tlasNodeCount)) { std::fclose(file); return std::nullopt; }
    scene.tlasNodes.resize(tlasNodeCount);
    if (tlasNodeCount > 0) {
        if (!readBytes(file, scene.tlasNodes.data(), sizeof(glm::vec4) * tlasNodeCount)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t tlasInstIndexCount = 0;
    if (!readUint32(file, tlasInstIndexCount)) { std::fclose(file); return std::nullopt; }
    scene.tlasInstanceIndices.resize(tlasInstIndexCount);
    if (tlasInstIndexCount > 0) {
        if (!readBytes(file, scene.tlasInstanceIndices.data(), sizeof(uint32_t) * tlasInstIndexCount)) { std::fclose(file); return std::nullopt; }
    }

    uint32_t lightRecordCount = 0;
    if (!readUint32(file, lightRecordCount)) { std::fclose(file); return std::nullopt; }
    scene.lightRecords.resize(lightRecordCount);
    if (lightRecordCount > 0) {
        if (!readBytes(file, scene.lightRecords.data(), sizeof(CachedLightRecord) * lightRecordCount)) { std::fclose(file); return std::nullopt; }
    }

    if (!readBytes(file, &scene.meshParams, sizeof(scene.meshParams))) { std::fclose(file); return std::nullopt; }

    uint32_t depCount = 0;
    if (!readUint32(file, depCount)) { std::fclose(file); return std::nullopt; }
    scene.dependencies.resize(depCount);
    for (uint32_t i = 0; i < depCount; ++i) {
        auto& dep = scene.dependencies[i];
        if (!readString(file, dep.path)) { std::fclose(file); return std::nullopt; }
        if (!readUint64(file, dep.size)) { std::fclose(file); return std::nullopt; }
        if (!readUint64(file, dep.mtime)) { std::fclose(file); return std::nullopt; }
    }

    std::fclose(file);
    return scene;
}

} // namespace rtv
