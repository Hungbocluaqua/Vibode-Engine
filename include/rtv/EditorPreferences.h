#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

struct EditorAssetCollection {
    std::string name;
    std::vector<std::string> assetGuids;
};

struct EditorPreferences {
    float cameraMoveSpeed = 2.4f;
    float cameraFastMoveSpeed = 7.5f;
    bool gridVisible = true;
    bool showHud = true;
    bool linkedScale = false;
    float hudScale = 1.0f;
    float uiScale = 1.0f;
    int themePreset = 0;
    int workspacePreset = 0;
    int layoutVersion = 2;
    int renderSequenceFramesPerTimelineFrame = 1;
    bool confirmDelete = true;
    std::vector<std::string> recentFiles;
    std::vector<std::string> favoriteFiles;
    std::vector<std::string> recentProjects;
    std::vector<std::string> assetTagPresets;
    std::vector<std::string> favoriteAssetGuids;
    std::vector<EditorAssetCollection> assetCollections;
    std::string lastOpenedProject;
    bool openLastProject = false;
    std::unordered_map<std::string, std::string> commandShortcutOverrides;

    static constexpr size_t maxRecentFiles = 10;

    void addRecentFile(const std::filesystem::path& path);
    void addRecentProject(const std::filesystem::path& path);
    void removeRecentProject(const std::string& path);
    void addFavorite(const std::filesystem::path& path);
    void removeFavorite(const std::string& path);
    void addAssetTagPreset(const std::string& tag);
    void removeAssetTagPreset(const std::string& tag);
    void addFavoriteAsset(const std::string& assetGuid);
    void removeFavoriteAsset(const std::string& assetGuid);
    void addAssetsToCollection(const std::string& name, const std::vector<std::string>& assetGuids);
    void removeAssetsFromCollection(const std::string& name, const std::vector<std::string>& assetGuids);
    void removeAssetCollection(const std::string& name);
    bool save(const std::filesystem::path& path) const;
    void load(const std::filesystem::path& path);
    [[nodiscard]] static std::filesystem::path defaultPath();
};

} // namespace rtv
