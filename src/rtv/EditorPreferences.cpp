#include "rtv/EditorPreferences.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace rtv {

void EditorPreferences::addRecentFile(const std::filesystem::path& path) {
    const std::string str = path.string();
    auto it = std::find(recentFiles.begin(), recentFiles.end(), str);
    if (it != recentFiles.end()) {
        recentFiles.erase(it);
    }
    recentFiles.insert(recentFiles.begin(), str);
    if (recentFiles.size() > maxRecentFiles) {
        recentFiles.resize(maxRecentFiles);
    }
}

void EditorPreferences::addFavorite(const std::filesystem::path& path) {
    const std::string str = path.string();
    if (std::find(favoriteFiles.begin(), favoriteFiles.end(), str) != favoriteFiles.end()) {
        return;
    }
    favoriteFiles.push_back(str);
}

void EditorPreferences::removeFavorite(const std::string& path) {
    const auto it = std::find(favoriteFiles.begin(), favoriteFiles.end(), path);
    if (it != favoriteFiles.end()) {
        favoriteFiles.erase(it);
    }
}

void EditorPreferences::save(const std::filesystem::path& path) const {
    nlohmann::json json;
    json["cameraMoveSpeed"] = cameraMoveSpeed;
    json["cameraFastMoveSpeed"] = cameraFastMoveSpeed;
    json["gridVisible"] = gridVisible;
    json["showHud"] = showHud;
    json["hudScale"] = hudScale;
    json["confirmDelete"] = confirmDelete;
    json["recentFiles"] = recentFiles;
    json["favoriteFiles"] = favoriteFiles;

    std::ofstream file(path);
    if (file.is_open()) {
        file << json.dump(2);
    }
}

void EditorPreferences::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    try {
        nlohmann::json json;
        file >> json;

        if (json.contains("cameraMoveSpeed")) cameraMoveSpeed = json["cameraMoveSpeed"].get<float>();
        if (json.contains("cameraFastMoveSpeed")) cameraFastMoveSpeed = json["cameraFastMoveSpeed"].get<float>();
        if (json.contains("gridVisible")) gridVisible = json["gridVisible"].get<bool>();
        if (json.contains("showHud")) showHud = json["showHud"].get<bool>();
        if (json.contains("hudScale")) hudScale = json["hudScale"].get<float>();
        if (json.contains("confirmDelete")) confirmDelete = json["confirmDelete"].get<bool>();
        if (json.contains("recentFiles")) recentFiles = json["recentFiles"].get<std::vector<std::string>>();
        if (json.contains("favoriteFiles")) favoriteFiles = json["favoriteFiles"].get<std::vector<std::string>>();
    } catch (...) {
        // Corrupt or incompatible file — use defaults
    }
}

std::filesystem::path EditorPreferences::defaultPath() {
    return std::filesystem::current_path() / "editor_preferences.json";
}

} // namespace rtv
