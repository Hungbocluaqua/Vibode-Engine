#include "rtv/EditorPreferences.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

namespace rtv {

namespace {

std::string trimPreferenceTag(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

void normalizePreferenceTags(std::vector<std::string>& tags) {
    for (std::string& tag : tags) {
        tag = trimPreferenceTag(std::move(tag));
    }
    tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& tag) { return tag.empty(); }), tags.end());
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
}

void normalizeGuidList(std::vector<std::string>& guids) {
    guids.erase(std::remove_if(guids.begin(), guids.end(), [](const std::string& guid) { return guid.empty(); }), guids.end());
    std::sort(guids.begin(), guids.end());
    guids.erase(std::unique(guids.begin(), guids.end()), guids.end());
}

void normalizeCollections(std::vector<EditorAssetCollection>& collections) {
    for (EditorAssetCollection& collection : collections) {
        collection.name = trimPreferenceTag(std::move(collection.name));
        normalizeGuidList(collection.assetGuids);
    }
    collections.erase(std::remove_if(collections.begin(), collections.end(), [](const EditorAssetCollection& collection) {
        return collection.name.empty();
    }), collections.end());
    std::sort(collections.begin(), collections.end(), [](const EditorAssetCollection& lhs, const EditorAssetCollection& rhs) {
        return lhs.name < rhs.name;
    });
    collections.erase(std::unique(collections.begin(), collections.end(), [](const EditorAssetCollection& lhs, const EditorAssetCollection& rhs) {
        return lhs.name == rhs.name;
    }), collections.end());
}

} // namespace

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

void EditorPreferences::addRecentProject(const std::filesystem::path& path) {
    const std::string str = path.string();
    auto it = std::find(recentProjects.begin(), recentProjects.end(), str);
    if (it != recentProjects.end()) {
        recentProjects.erase(it);
    }
    recentProjects.insert(recentProjects.begin(), str);
    if (recentProjects.size() > maxRecentFiles) {
        recentProjects.resize(maxRecentFiles);
    }
    lastOpenedProject = str;
}

void EditorPreferences::removeRecentProject(const std::string& path) {
    const auto it = std::find(recentProjects.begin(), recentProjects.end(), path);
    if (it != recentProjects.end()) {
        recentProjects.erase(it);
    }
    if (lastOpenedProject == path) {
        lastOpenedProject.clear();
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

void EditorPreferences::addAssetTagPreset(const std::string& tag) {
    std::string value = trimPreferenceTag(tag);
    if (value.empty()) {
        return;
    }
    assetTagPresets.push_back(std::move(value));
    normalizePreferenceTags(assetTagPresets);
}

void EditorPreferences::removeAssetTagPreset(const std::string& tag) {
    const std::string value = trimPreferenceTag(tag);
    assetTagPresets.erase(std::remove_if(assetTagPresets.begin(), assetTagPresets.end(), [&](const std::string& existing) {
        return existing == value;
    }), assetTagPresets.end());
}

void EditorPreferences::addFavoriteAsset(const std::string& assetGuid) {
    if (assetGuid.empty()) {
        return;
    }
    favoriteAssetGuids.push_back(assetGuid);
    normalizeGuidList(favoriteAssetGuids);
}

void EditorPreferences::removeFavoriteAsset(const std::string& assetGuid) {
    favoriteAssetGuids.erase(std::remove(favoriteAssetGuids.begin(), favoriteAssetGuids.end(), assetGuid), favoriteAssetGuids.end());
}

void EditorPreferences::addAssetsToCollection(const std::string& name, const std::vector<std::string>& assetGuids) {
    std::string collectionName = trimPreferenceTag(name);
    if (collectionName.empty() || assetGuids.empty()) {
        return;
    }
    auto it = std::find_if(assetCollections.begin(), assetCollections.end(), [&](const EditorAssetCollection& collection) {
        return collection.name == collectionName;
    });
    if (it == assetCollections.end()) {
        EditorAssetCollection collection;
        collection.name = std::move(collectionName);
        collection.assetGuids = assetGuids;
        assetCollections.push_back(std::move(collection));
    } else {
        it->assetGuids.insert(it->assetGuids.end(), assetGuids.begin(), assetGuids.end());
    }
    normalizeCollections(assetCollections);
}

void EditorPreferences::removeAssetsFromCollection(const std::string& name, const std::vector<std::string>& assetGuids) {
    const std::string collectionName = trimPreferenceTag(name);
    if (collectionName.empty() || assetGuids.empty()) {
        return;
    }
    auto it = std::find_if(assetCollections.begin(), assetCollections.end(), [&](const EditorAssetCollection& collection) {
        return collection.name == collectionName;
    });
    if (it == assetCollections.end()) {
        return;
    }
    it->assetGuids.erase(std::remove_if(it->assetGuids.begin(), it->assetGuids.end(), [&](const std::string& guid) {
        return std::find(assetGuids.begin(), assetGuids.end(), guid) != assetGuids.end();
    }), it->assetGuids.end());
    normalizeCollections(assetCollections);
}

void EditorPreferences::removeAssetCollection(const std::string& name) {
    const std::string collectionName = trimPreferenceTag(name);
    assetCollections.erase(std::remove_if(assetCollections.begin(), assetCollections.end(), [&](const EditorAssetCollection& collection) {
        return collection.name == collectionName;
    }), assetCollections.end());
}

bool EditorPreferences::save(const std::filesystem::path& path) const {
    nlohmann::json json;
    json["cameraMoveSpeed"] = cameraMoveSpeed;
    json["cameraFastMoveSpeed"] = cameraFastMoveSpeed;
    json["gridVisible"] = gridVisible;
    json["showHud"] = showHud;
    json["linkedScale"] = linkedScale;
    json["hudScale"] = hudScale;
    json["uiScale"] = uiScale;
    json["themePreset"] = themePreset;
    json["workspacePreset"] = workspacePreset;
    json["layoutVersion"] = layoutVersion;
    json["renderSequenceFramesPerTimelineFrame"] = std::clamp(renderSequenceFramesPerTimelineFrame, 1, 512);
    json["confirmDelete"] = confirmDelete;
    json["recentFiles"] = recentFiles;
    json["favoriteFiles"] = favoriteFiles;
    json["recentProjects"] = recentProjects;
    std::vector<std::string> sortedTagPresets = assetTagPresets;
    normalizePreferenceTags(sortedTagPresets);
    json["assetTagPresets"] = sortedTagPresets;
    std::vector<std::string> sortedFavoriteAssetGuids = favoriteAssetGuids;
    normalizeGuidList(sortedFavoriteAssetGuids);
    json["favoriteAssetGuids"] = sortedFavoriteAssetGuids;
    std::vector<EditorAssetCollection> sortedCollections = assetCollections;
    normalizeCollections(sortedCollections);
    json["assetCollections"] = nlohmann::json::array();
    for (const EditorAssetCollection& collection : sortedCollections) {
        json["assetCollections"].push_back({
            {"name", collection.name},
            {"assetGuids", collection.assetGuids},
        });
    }
    json["lastOpenedProject"] = lastOpenedProject;
    json["openLastProject"] = openLastProject;
    json["commandShortcutOverrides"] = commandShortcutOverrides;

    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file << json.dump(2);
    return file.good();
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
        if (json.contains("linkedScale")) linkedScale = json["linkedScale"].get<bool>();
        if (json.contains("hudScale")) hudScale = json["hudScale"].get<float>();
        if (json.contains("uiScale")) uiScale = std::clamp(json["uiScale"].get<float>(), 0.75f, 1.75f);
        if (json.contains("themePreset")) themePreset = json["themePreset"].get<int>();
        if (json.contains("workspacePreset")) workspacePreset = json["workspacePreset"].get<int>();
        if (json.contains("layoutVersion")) layoutVersion = json["layoutVersion"].get<int>();
        if (json.contains("renderSequenceFramesPerTimelineFrame")) renderSequenceFramesPerTimelineFrame = std::clamp(json["renderSequenceFramesPerTimelineFrame"].get<int>(), 1, 512);
        if (json.contains("confirmDelete")) confirmDelete = json["confirmDelete"].get<bool>();
        if (json.contains("recentFiles")) recentFiles = json["recentFiles"].get<std::vector<std::string>>();
        if (json.contains("favoriteFiles")) favoriteFiles = json["favoriteFiles"].get<std::vector<std::string>>();
        if (json.contains("recentProjects")) recentProjects = json["recentProjects"].get<std::vector<std::string>>();
        if (json.contains("assetTagPresets")) {
            assetTagPresets = json["assetTagPresets"].get<std::vector<std::string>>();
            normalizePreferenceTags(assetTagPresets);
        }
        if (json.contains("favoriteAssetGuids")) {
            favoriteAssetGuids = json["favoriteAssetGuids"].get<std::vector<std::string>>();
            normalizeGuidList(favoriteAssetGuids);
        }
        if (json.contains("assetCollections") && json["assetCollections"].is_array()) {
            assetCollections.clear();
            for (const nlohmann::json& item : json["assetCollections"]) {
                if (!item.is_object()) {
                    continue;
                }
                EditorAssetCollection collection;
                collection.name = item.value("name", std::string{});
                if (item.contains("assetGuids") && item["assetGuids"].is_array()) {
                    collection.assetGuids = item["assetGuids"].get<std::vector<std::string>>();
                }
                assetCollections.push_back(std::move(collection));
            }
            normalizeCollections(assetCollections);
        }
        if (json.contains("lastOpenedProject")) lastOpenedProject = json["lastOpenedProject"].get<std::string>();
        if (json.contains("openLastProject")) openLastProject = json["openLastProject"].get<bool>();
        if (json.contains("commandShortcutOverrides")) commandShortcutOverrides = json["commandShortcutOverrides"].get<std::unordered_map<std::string, std::string>>();
    } catch (...) {
        // Corrupt or incompatible file — use defaults
    }
}

std::filesystem::path EditorPreferences::defaultPath() {
    return std::filesystem::current_path() / "editor_preferences.json";
}

} // namespace rtv
