#pragma once

#include "rtv/MeshAsset.h"
#include "rtv/NativeTextureFormatPolicy.h"
#include "rtv/SceneCache.h"

#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace rtv {

class AssetManager;

class GltfLoader {
public:
    explicit GltfLoader(AssetManager& assets);

    void setCacheWritesEnabled(bool enabled) { cacheWritesEnabled_ = enabled; }
    void setNativeTextureFormatSupport(NativeTextureFormatSupport support) { nativeTextureFormatSupport_ = std::move(support); }

    [[nodiscard]] SceneAsset load(const std::filesystem::path& path);
    [[nodiscard]] SceneAsset loadWithCache(const std::filesystem::path& path);

private:
    [[nodiscard]] CachedScene buildCachedScene(const std::filesystem::path& path, const SceneAsset& scene, const std::vector<std::filesystem::path>& externalDependencies);

    AssetManager& assets_;
    bool useCache_ = true;
    bool cacheWritesEnabled_ = true;
    NativeTextureFormatSupport nativeTextureFormatSupport_ = nativeTextureOfflineFallbackFormatSupport();
    std::vector<std::filesystem::path> lastExternalDependencies_;
};

} // namespace rtv
