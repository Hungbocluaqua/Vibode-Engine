#pragma once

#include "rtv/EntityId.h"
#include "rtv/MeshAsset.h"
#include "rtv/SceneComponents.h"
#include "rtv/SceneRegistry.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace rtv {

class SceneDocument {
public:
    [[nodiscard]] SceneRegistry& registry() { return registry_; }
    [[nodiscard]] const SceneRegistry& registry() const { return registry_; }

    [[nodiscard]] Environment& environment() { return environment_; }
    [[nodiscard]] const Environment& environment() const { return environment_; }
    [[nodiscard]] RenderSettings& renderSettings() { return renderSettings_; }
    [[nodiscard]] const RenderSettings& renderSettings() const { return renderSettings_; }

    void setEnvironment(Environment environment);
    void setRenderSettings(RenderSettings settings);
    void setActiveCamera(EntityId id);
    [[nodiscard]] EntityId activeCamera() const { return activeCamera_; }
    void setPrimarySun(EntityId id);
    [[nodiscard]] EntityId primarySun() const;

    void setSourceGltfPath(std::optional<std::filesystem::path> path);
    void setSourceHdrPath(std::optional<std::filesystem::path> path);
    [[nodiscard]] const std::optional<std::filesystem::path>& sourceGltfPath() const { return sourceGltfPath_; }
    [[nodiscard]] const std::optional<std::filesystem::path>& sourceHdrPath() const { return sourceHdrPath_; }

    void setBookmarksJson(const nlohmann::json& json);
    [[nodiscard]] const std::optional<nlohmann::json>& bookmarksJson() const { return bookmarksJson_; }
    void clearBookmarksJson();

    void markDirty(SceneUpdateKind kind);
    void clearDirty();
    [[nodiscard]] bool dirty() const { return dirty_ || registry_.dirty(); }
    [[nodiscard]] SceneUpdateKind pendingUpdate() const;
    [[nodiscard]] const std::string& lastChangeReason() const { return lastChangeReason_; }

    void importSceneAsset(const SceneAsset& scene);
    [[nodiscard]] SceneAsset toSceneAsset() const;

    bool saveJson(const std::filesystem::path& path) const;
    bool loadJson(const std::filesystem::path& path);

private:
    static SceneUpdateKind combine(SceneUpdateKind current, SceneUpdateKind next);

    SceneRegistry registry_;
    Environment environment_{};
    RenderSettings renderSettings_{};
    EntityId activeCamera_{};
    EntityId primarySun_{};
    bool dirty_ = true;
    SceneUpdateKind pendingUpdate_ = SceneUpdateKind::TopologyChanged;
    std::string lastChangeReason_ = "SceneChanged";
    std::optional<std::filesystem::path> sourceGltfPath_;
    std::optional<std::filesystem::path> sourceHdrPath_;
    std::optional<nlohmann::json> bookmarksJson_;
    std::vector<TextureAssetHandle> sceneTextures_;
    std::vector<MaterialAssetHandle> sceneMaterials_;
    std::vector<MeshAssetHandle> sceneMeshes_;
};

} // namespace rtv
