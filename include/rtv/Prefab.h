#pragma once

#include "rtv/AssetRegistry.h"
#include "rtv/EntityId.h"
#include "rtv/MeshAsset.h"

#include <filesystem>
#include <unordered_map>
#include <string>
#include <vector>

namespace rtv {

struct PrefabOverride {
    std::string path;
    std::string valueJson;
};

struct PrefabNodeAsset {
    std::string name;
    int parent = -1;
    glm::mat4 transform{1.0f};
    AssetGuid meshGuid;
    std::vector<AssetGuid> materialGuids;
    bool hasCamera = false;
    uint32_t cameraProjection = 0;
    float cameraYfov = 60.0f * 0.017453292519943295f;
    float cameraAspectRatio = 0.0f;
    float cameraOrthoXmag = 1.0f;
    float cameraOrthoYmag = 1.0f;
    float cameraNear = 0.01f;
    float cameraFar = 1000.0f;
    std::vector<uint32_t> children;
};

struct PrefabAsset {
    AssetGuid guid;
    std::string name;
    std::filesystem::path sourcePath;
    std::string runtimePayloadKind;
    std::filesystem::path runtimeCachePath;
    std::string runtimePayloadHash;
    std::string runtimeSourceHash;
    std::string runtimeImportSettingsHash;
    std::vector<PrefabNodeAsset> nodes;
    std::vector<uint32_t> rootNodes;
};

struct PrefabInstance {
    AssetGuid prefabGuid;
    EntityId instanceRoot;
    std::vector<uint64_t> generatedEntityUuids;
    std::vector<PrefabOverride> overrides;
};

struct PrefabRuntimeBindings {
    std::unordered_map<AssetGuid, MeshAssetHandle> meshes;
    std::unordered_map<AssetGuid, MaterialAssetHandle> materials;
};

[[nodiscard]] bool loadPrefabAsset(const std::filesystem::path& path, PrefabAsset& outPrefab, std::string* error = nullptr);

} // namespace rtv
