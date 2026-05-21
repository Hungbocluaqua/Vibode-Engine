#include "rtv/SceneCache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace rtv {

namespace {

constexpr uint32_t kCacheMagic = 0x53434E45;
constexpr uint32_t kCacheVersion = 14;

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

    std::filesystem::path binPath = gltfPath.parent_path() / (gltfPath.stem().string() + ".bin");
    if (std::filesystem::exists(binPath)) {
        uint64_t binMtime = fileMtime(binPath);
        if (binMtime > cacheMtime) {
            return false;
        }
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
        writeUint32(file, tex.srgb ? 1u : 0u);
        writeUint32(file, tex.fallback ? 1u : 0u);
        writeUint32(file, tex.minFilter);
        writeUint32(file, tex.magFilter);
        writeUint32(file, tex.wrapS);
        writeUint32(file, tex.wrapT);
        writeBytes(file, tex.rgba8.data(), tex.rgba8.size());
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
        writeFloat(file, mat.alphaCutoff);
        writeUint32(file, mat.alphaMode);
        writeUint32(file, mat.doubleSided);
        writeInt32(file, mat.baseColorTextureIndex);
        writeInt32(file, mat.normalTextureIndex);
        writeInt32(file, mat.metallicRoughnessTextureIndex);
        writeInt32(file, mat.emissiveTextureIndex);
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
        writeUint32(file, node.hasCamera);
        writeFloat(file, node.cameraYfov);
        writeFloat(file, node.cameraNear);
        writeFloat(file, node.cameraFar);
        writeInt32(file, node.parentIndex);
        uint32_t childCount = static_cast<uint32_t>(node.children.size());
        writeUint32(file, childCount);
        if (childCount > 0) {
            writeBytes(file, node.children.data(), sizeof(uint32_t) * childCount);
        }
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
        size_t pixelSize = static_cast<size_t>(tex.width) * tex.height * tex.channels;
        tex.rgba8.resize(pixelSize);
        if (pixelSize > 0) {
            if (!readBytes(file, tex.rgba8.data(), pixelSize)) { std::fclose(file); return std::nullopt; }
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
        if (!readFloat(file, mat.alphaCutoff)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.alphaMode)) { std::fclose(file); return std::nullopt; }
        if (!readUint32(file, mat.doubleSided)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.baseColorTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.normalTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.metallicRoughnessTextureIndex)) { std::fclose(file); return std::nullopt; }
        if (!readInt32(file, mat.emissiveTextureIndex)) { std::fclose(file); return std::nullopt; }
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
        if (!readUint32(file, node.hasCamera)) { std::fclose(file); return std::nullopt; }
        if (!readFloat(file, node.cameraYfov)) { std::fclose(file); return std::nullopt; }
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
