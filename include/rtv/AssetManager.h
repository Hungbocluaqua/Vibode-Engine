#pragma once

#include "rtv/MeshAsset.h"
#include "rtv/TextureAsset.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rtv {

struct AssetLoadStats {
    uint32_t textureCount = 0;
    uint32_t materialCount = 0;
    uint32_t meshCount = 0;
    uint32_t nodeCount = 0;
};

class AssetManager {
public:
    [[nodiscard]] TextureAssetHandle addTexture(TextureAsset texture);
    [[nodiscard]] MaterialAssetHandle addMaterial(MaterialAsset material);
    [[nodiscard]] MeshAssetHandle addMesh(MeshAsset mesh);

    [[nodiscard]] TextureAsset* texture(TextureAssetHandle handle);
    [[nodiscard]] MaterialAsset* material(MaterialAssetHandle handle);
    [[nodiscard]] MeshAsset* mesh(MeshAssetHandle handle);

    [[nodiscard]] const TextureAsset* texture(TextureAssetHandle handle) const;
    [[nodiscard]] const MaterialAsset* material(MaterialAssetHandle handle) const;
    [[nodiscard]] const MeshAsset* mesh(MeshAssetHandle handle) const;

    void markTextureResident(TextureAssetHandle handle, bool resident);
    [[nodiscard]] AssetLoadStats stats() const;
    [[nodiscard]] const std::vector<TextureAsset>& textures() const { return textures_; }
    [[nodiscard]] const std::vector<MaterialAsset>& materials() const { return materials_; }
    [[nodiscard]] const std::vector<MeshAsset>& meshes() const { return meshes_; }
    void clear();

private:
    std::vector<TextureAsset> textures_;
    std::vector<MaterialAsset> materials_;
    std::vector<MeshAsset> meshes_;
};

} // namespace rtv
