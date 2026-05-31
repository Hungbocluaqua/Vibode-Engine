#pragma once

#include "rtv/EntityId.h"
#include "rtv/MeshAsset.h"
#include "rtv/Prefab.h"
#include "rtv/SceneComponents.h"
#include "rtv/SceneRegistry.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace rtv {

struct RtLevelHeader {
    uint32_t formatVersion = 3;
    std::string sceneGuid;
    std::string engineVersion = "0.1";
    bool projectRelativePaths = true;
};

class SceneDocument {
public:
    [[nodiscard]] SceneRegistry& registry() { return registry_; }
    [[nodiscard]] const SceneRegistry& registry() const { return registry_; }

    [[nodiscard]] Environment& environment() { return environment_; }
    [[nodiscard]] const Environment& environment() const { return environment_; }
    [[nodiscard]] RenderSettings& renderSettings() { return renderSettings_; }
    [[nodiscard]] const RenderSettings& renderSettings() const { return renderSettings_; }
    [[nodiscard]] WorldSettings& worldSettings() { return worldSettings_; }
    [[nodiscard]] const WorldSettings& worldSettings() const { return worldSettings_; }

    void setEnvironment(Environment environment);
    void setRenderSettings(RenderSettings settings);
    void setWorldSettings(WorldSettings settings);
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
    void setTimelineJson(const nlohmann::json& json);
    [[nodiscard]] const std::optional<nlohmann::json>& timelineJson() const { return timelineJson_; }
    void clearTimelineJson();
    void addPrefabInstance(PrefabInstance instance);
    [[nodiscard]] const std::vector<PrefabInstance>& prefabInstances() const { return prefabInstances_; }
    [[nodiscard]] const RtLevelHeader& rtLevelHeader() const { return header_; }

    void markDirty(SceneUpdateKind kind);
    void clearDirty();
    [[nodiscard]] bool dirty() const { return dirty_ || registry_.dirty(); }
    [[nodiscard]] SceneUpdateKind pendingUpdate() const;
    [[nodiscard]] const std::string& lastChangeReason() const { return lastChangeReason_; }
    [[nodiscard]] const std::vector<std::string>& dirtyReasons() const { return dirtyReasons_; }

    void importSceneAsset(const SceneAsset& scene);
    [[nodiscard]] SceneAsset toSceneAsset() const;

    bool saveJson(const std::filesystem::path& path) const;
    bool loadJson(const std::filesystem::path& path);

private:
    static SceneUpdateKind combine(SceneUpdateKind current, SceneUpdateKind next);

    mutable RtLevelHeader header_{};
    SceneRegistry registry_;
    Environment environment_{};
    RenderSettings renderSettings_{};
    WorldSettings worldSettings_{};
    EntityId activeCamera_{};
    EntityId primarySun_{};
    bool dirty_ = true;
    SceneUpdateKind pendingUpdate_ = SceneUpdateKind::TopologyChanged;
    std::string lastChangeReason_ = "SceneChanged";
    std::vector<std::string> dirtyReasons_;
    std::optional<std::filesystem::path> sourceGltfPath_;
    std::optional<std::filesystem::path> sourceHdrPath_;
    std::optional<nlohmann::json> bookmarksJson_;
    std::optional<nlohmann::json> timelineJson_;
    std::vector<PrefabInstance> prefabInstances_;
    std::vector<TextureAssetHandle> sceneTextures_;
    std::vector<MaterialAssetHandle> sceneMaterials_;
    std::vector<MeshAssetHandle> sceneMeshes_;
};

} // namespace rtv
