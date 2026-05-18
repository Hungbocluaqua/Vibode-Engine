#include "rtv/AssetManager.h"

namespace rtv {

TextureAssetHandle AssetManager::addTexture(TextureAsset texture) {
    const TextureAssetHandle handle{static_cast<uint32_t>(textures_.size())};
    textures_.push_back(std::move(texture));
    return handle;
}

MaterialAssetHandle AssetManager::addMaterial(MaterialAsset material) {
    const MaterialAssetHandle handle{static_cast<uint32_t>(materials_.size())};
    materials_.push_back(std::move(material));
    return handle;
}

MeshAssetHandle AssetManager::addMesh(MeshAsset mesh) {
    const MeshAssetHandle handle{static_cast<uint32_t>(meshes_.size())};
    meshes_.push_back(std::move(mesh));
    return handle;
}

TextureAsset* AssetManager::texture(TextureAssetHandle handle) {
    return handle.index < textures_.size() ? &textures_[handle.index] : nullptr;
}

MaterialAsset* AssetManager::material(MaterialAssetHandle handle) {
    return handle.index < materials_.size() ? &materials_[handle.index] : nullptr;
}

MeshAsset* AssetManager::mesh(MeshAssetHandle handle) {
    return handle.index < meshes_.size() ? &meshes_[handle.index] : nullptr;
}

const TextureAsset* AssetManager::texture(TextureAssetHandle handle) const {
    return handle.index < textures_.size() ? &textures_[handle.index] : nullptr;
}

const MaterialAsset* AssetManager::material(MaterialAssetHandle handle) const {
    return handle.index < materials_.size() ? &materials_[handle.index] : nullptr;
}

const MeshAsset* AssetManager::mesh(MeshAssetHandle handle) const {
    return handle.index < meshes_.size() ? &meshes_[handle.index] : nullptr;
}

void AssetManager::markTextureResident(TextureAssetHandle handle, bool resident) {
    if (TextureAsset* asset = texture(handle)) {
        asset->resident = resident;
    }
}

AssetLoadStats AssetManager::stats() const {
    return {
        .textureCount = static_cast<uint32_t>(textures_.size()),
        .materialCount = static_cast<uint32_t>(materials_.size()),
        .meshCount = static_cast<uint32_t>(meshes_.size()),
    };
}

void AssetManager::clear() {
    textures_.clear();
    materials_.clear();
    meshes_.clear();
}

} // namespace rtv
