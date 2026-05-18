#pragma once

#include "rtv/MeshAsset.h"
#include "rtv/SceneCache.h"

#include <filesystem>
#include <optional>

namespace rtv {

class AssetManager;

class GltfLoader {
public:
    explicit GltfLoader(AssetManager& assets);

    [[nodiscard]] SceneAsset load(const std::filesystem::path& path);
    [[nodiscard]] SceneAsset loadWithCache(const std::filesystem::path& path);

private:
    [[nodiscard]] CachedScene buildCachedScene(const std::filesystem::path& path, const SceneAsset& scene);

    AssetManager& assets_;
    bool useCache_ = true;
};

} // namespace rtv
