#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

struct EditorPreferences {
    float cameraMoveSpeed = 2.4f;
    float cameraFastMoveSpeed = 7.5f;
    bool gridVisible = true;
    bool showHud = true;
    float hudScale = 1.0f;
    float uiScale = 1.0f;
    int themePreset = 0;
    int workspacePreset = 0;
    int layoutVersion = 2;
    bool confirmDelete = true;
    std::vector<std::string> recentFiles;
    std::vector<std::string> favoriteFiles;
    std::vector<std::string> recentProjects;
    std::string lastOpenedProject;
    bool openLastProject = false;

    static constexpr size_t maxRecentFiles = 10;

    void addRecentFile(const std::filesystem::path& path);
    void addRecentProject(const std::filesystem::path& path);
    void removeRecentProject(const std::string& path);
    void addFavorite(const std::filesystem::path& path);
    void removeFavorite(const std::string& path);
    void save(const std::filesystem::path& path) const;
    void load(const std::filesystem::path& path);
    [[nodiscard]] static std::filesystem::path defaultPath();
};

} // namespace rtv
