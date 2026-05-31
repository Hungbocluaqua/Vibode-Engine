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
    AssetGuid meshGuid;
    std::vector<AssetGuid> materialGuids;
    std::vector<uint32_t> children;
};

struct PrefabAsset {
    AssetGuid guid;
    std::string name;
    std::filesystem::path sourcePath;
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
