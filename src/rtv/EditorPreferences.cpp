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

void normalizeNativeTextureTargetSetLibrary(std::vector<EditorNativeTextureTargetSetLibraryProfile>& profiles) {
    for (EditorNativeTextureTargetSetLibraryProfile& profile : profiles) {
        profile.name = trimPreferenceTag(std::move(profile.name));
    }
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [](const EditorNativeTextureTargetSetLibraryProfile& profile) {
        return profile.name.empty();
    }), profiles.end());
    std::sort(profiles.begin(), profiles.end(), [](const EditorNativeTextureTargetSetLibraryProfile& lhs, const EditorNativeTextureTargetSetLibraryProfile& rhs) {
        return lhs.name < rhs.name;
    });
    profiles.erase(std::unique(profiles.begin(), profiles.end(), [](const EditorNativeTextureTargetSetLibraryProfile& lhs, const EditorNativeTextureTargetSetLibraryProfile& rhs) {
        return lhs.name == rhs.name;
    }), profiles.end());
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
    json["cameraMoveSpeed"] = std::clamp(cameraMoveSpeed, 0.05f, 100.0f);
    json["cameraFastMoveSpeed"] = std::clamp(cameraFastMoveSpeed, 0.05f, 250.0f);
    json["cameraMouseSensitivity"] = std::clamp(cameraMouseSensitivity, 0.0001f, 0.02f);
    json["cameraInvertLookX"] = cameraInvertLookX;
    json["cameraInvertLookY"] = cameraInvertLookY;
    json["gridVisible"] = gridVisible;
    json["showHud"] = showHud;
    json["linkedScale"] = linkedScale;
    json["viewportSnapEnabled"] = viewportSnapEnabled;
    json["viewportTranslationSnap"] = std::clamp(viewportTranslationSnap, 0.001f, 100.0f);
    json["viewportRotationSnap"] = std::clamp(viewportRotationSnap, 0.1f, 180.0f);
    json["viewportScaleSnap"] = std::clamp(viewportScaleSnap, 0.001f, 10.0f);
    json["hudScale"] = std::clamp(hudScale, 0.75f, 1.75f);
    json["uiScale"] = uiScale;
    json["themePreset"] = themePreset;
    json["workspacePreset"] = workspacePreset;
    json["layoutVersion"] = layoutVersion;
    json["contentBrowserMode"] = std::clamp(contentBrowserMode, 0, 1);
    json["contentBrowserGridView"] = contentBrowserGridView;
    json["contentBrowserShowDetails"] = contentBrowserShowDetails;
    json["cookEmitNativeTextureTargetSets"] = cookEmitNativeTextureTargetSets;
    json["cookNativeTextureTargetSetProfile"] = std::clamp(cookNativeTextureTargetSetProfile, 0, 5);
    json["cookNativeTextureTargetSetName"] = cookNativeTextureTargetSetName;
    json["cookNativeTextureTargetSetBc7Srgb"] = cookNativeTextureTargetSetBc7Srgb;
    json["cookNativeTextureTargetSetBc7Unorm"] = cookNativeTextureTargetSetBc7Unorm;
    json["cookNativeTextureTargetSetBc5"] = cookNativeTextureTargetSetBc5;
    json["cookNativeTextureTargetSetBc4"] = cookNativeTextureTargetSetBc4;
    json["cookNativeTextureTargetSetRgba8Srgb"] = cookNativeTextureTargetSetRgba8Srgb;
    json["cookNativeTextureTargetSetRgba8Unorm"] = cookNativeTextureTargetSetRgba8Unorm;
    json["cookNativeTextureTargetSetRgba16f"] = cookNativeTextureTargetSetRgba16f;
    std::vector<EditorNativeTextureTargetSetLibraryProfile> sortedTargetSetLibrary = cookNativeTextureTargetSetLibrary;
    normalizeNativeTextureTargetSetLibrary(sortedTargetSetLibrary);
    json["cookNativeTextureTargetSetLibrary"] = nlohmann::json::array();
    for (const EditorNativeTextureTargetSetLibraryProfile& profile : sortedTargetSetLibrary) {
        json["cookNativeTextureTargetSetLibrary"].push_back({
            {"name", profile.name},
            {"bc7SrgbSampled", profile.bc7SrgbSampled},
            {"bc7UnormSampled", profile.bc7UnormSampled},
            {"bc5UnormSampled", profile.bc5UnormSampled},
            {"bc4UnormSampled", profile.bc4UnormSampled},
            {"rgba8SrgbSampled", profile.rgba8SrgbSampled},
            {"rgba8UnormSampled", profile.rgba8UnormSampled},
            {"rgba16fSampled", profile.rgba16fSampled},
        });
    }
    json["viewportAxesVisible"] = viewportAxesVisible;
    json["viewportLocalTransformFrame"] = viewportLocalTransformFrame;
    json["renderSequenceFramesPerTimelineFrame"] = std::clamp(renderSequenceFramesPerTimelineFrame, 1, 512);
    json["confirmDelete"] = confirmDelete;
    json["viewportDropForceGridByDefault"] = viewportDropForceGridByDefault;
    json["viewportDropSurfaceAlignByDefault"] = viewportDropSurfaceAlignByDefault;
    json["viewportDropDuplicatePlacementByDefault"] = viewportDropDuplicatePlacementByDefault;
    json["viewportDropMultiPlaceByDefault"] = viewportDropMultiPlaceByDefault;
    json["viewportDropMouseWheelRotationEnabled"] = viewportDropMouseWheelRotationEnabled;
    json["viewportPickMeshEntities"] = viewportPickMeshEntities;
    json["viewportPickActorIcons"] = viewportPickActorIcons;
    json["viewportSurfaceSnappingEnabled"] = viewportSurfaceSnappingEnabled;
    json["viewportSurfaceSnapAlignToNormal"] = viewportSurfaceSnapAlignToNormal;
    json["viewportSurfaceSnapPreserveYaw"] = viewportSurfaceSnapPreserveYaw;
    json["viewportSurfaceSnapOffset"] = std::clamp(viewportSurfaceSnapOffset, -100.0f, 100.0f);
    json["viewportSurfaceSnapBoundsBottom"] = viewportSurfaceSnapBoundsBottom;
    json["viewportSurfaceSnapAxisConstraint"] = std::clamp(viewportSurfaceSnapAxisConstraint, 0, 3);
    json["viewportScatterPaletteByDefault"] = viewportScatterPaletteByDefault;
    json["viewportScatterPaletteDensity"] = std::clamp(viewportScatterPaletteDensity, 0.0f, 10000.0f);
    json["viewportScatterPaletteSlopeMinDegrees"] = std::clamp(viewportScatterPaletteSlopeMinDegrees, 0.0f, 180.0f);
    json["viewportScatterPaletteSlopeMaxDegrees"] = std::clamp(viewportScatterPaletteSlopeMaxDegrees, 0.0f, 180.0f);
    json["viewportScatterPaletteHeightMin"] = viewportScatterPaletteHeightMin;
    json["viewportScatterPaletteHeightMax"] = viewportScatterPaletteHeightMax;
    json["viewportScatterPaletteScaleMin"] = std::clamp(viewportScatterPaletteScaleMin, 0.001f, 1000.0f);
    json["viewportScatterPaletteScaleMax"] = std::clamp(viewportScatterPaletteScaleMax, 0.001f, 1000.0f);
    json["viewportScatterPaletteYawRandomDegrees"] = std::clamp(viewportScatterPaletteYawRandomDegrees, 0.0f, 360.0f);
    json["viewportScatterPaletteSeed"] = viewportScatterPaletteSeed;
    json["viewportScatterPaletteSpacing"] = std::clamp(viewportScatterPaletteSpacing, 0.001f, 10000.0f);
    json["viewportScatterPaletteCollisionRadius"] = std::clamp(viewportScatterPaletteCollisionRadius, 0.0f, 10000.0f);
    json["viewportScatterPaletteSurfaceAlignment"] = viewportScatterPaletteSurfaceAlignment;
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

        if (json.contains("cameraMoveSpeed")) cameraMoveSpeed = std::clamp(json["cameraMoveSpeed"].get<float>(), 0.05f, 100.0f);
        if (json.contains("cameraFastMoveSpeed")) cameraFastMoveSpeed = std::clamp(json["cameraFastMoveSpeed"].get<float>(), 0.05f, 250.0f);
        if (json.contains("cameraMouseSensitivity")) cameraMouseSensitivity = std::clamp(json["cameraMouseSensitivity"].get<float>(), 0.0001f, 0.02f);
        if (json.contains("cameraInvertLookX")) cameraInvertLookX = json["cameraInvertLookX"].get<bool>();
        if (json.contains("cameraInvertLookY")) cameraInvertLookY = json["cameraInvertLookY"].get<bool>();
        if (json.contains("gridVisible")) gridVisible = json["gridVisible"].get<bool>();
        if (json.contains("showHud")) showHud = json["showHud"].get<bool>();
        if (json.contains("linkedScale")) linkedScale = json["linkedScale"].get<bool>();
        if (json.contains("viewportSnapEnabled")) viewportSnapEnabled = json["viewportSnapEnabled"].get<bool>();
        if (json.contains("viewportTranslationSnap")) viewportTranslationSnap = std::clamp(json["viewportTranslationSnap"].get<float>(), 0.001f, 100.0f);
        if (json.contains("viewportRotationSnap")) viewportRotationSnap = std::clamp(json["viewportRotationSnap"].get<float>(), 0.1f, 180.0f);
        if (json.contains("viewportScaleSnap")) viewportScaleSnap = std::clamp(json["viewportScaleSnap"].get<float>(), 0.001f, 10.0f);
        if (json.contains("hudScale")) hudScale = std::clamp(json["hudScale"].get<float>(), 0.75f, 1.75f);
        if (json.contains("uiScale")) uiScale = std::clamp(json["uiScale"].get<float>(), 0.75f, 1.75f);
        if (json.contains("themePreset")) themePreset = json["themePreset"].get<int>();
        if (json.contains("workspacePreset")) workspacePreset = json["workspacePreset"].get<int>();
        if (json.contains("layoutVersion")) layoutVersion = json["layoutVersion"].get<int>();
        if (json.contains("contentBrowserMode")) contentBrowserMode = std::clamp(json["contentBrowserMode"].get<int>(), 0, 1);
        if (json.contains("contentBrowserGridView")) contentBrowserGridView = json["contentBrowserGridView"].get<bool>();
        if (json.contains("contentBrowserShowDetails")) contentBrowserShowDetails = json["contentBrowserShowDetails"].get<bool>();
        if (json.contains("cookEmitNativeTextureTargetSets")) cookEmitNativeTextureTargetSets = json["cookEmitNativeTextureTargetSets"].get<bool>();
        if (json.contains("cookNativeTextureTargetSetProfile")) cookNativeTextureTargetSetProfile = std::clamp(json["cookNativeTextureTargetSetProfile"].get<int>(), 0, 5);
        if (json.contains("cookNativeTextureTargetSetName")) cookNativeTextureTargetSetName = json["cookNativeTextureTargetSetName"].get<std::string>();
        if (json.contains("cookNativeTextureTargetSetBc7Srgb")) cookNativeTextureTargetSetBc7Srgb = json["cookNativeTextureTargetSetBc7Srgb"].get<bool>();
        if (json.contains("cookNativeTextureTargetSetBc7Unorm")) cookNativeTextureTargetSetBc7Unorm = json["cookNativeTextureTargetSetBc7Unorm"].get<bool>();
        if (json.contains("cookNativeTextureTargetSetBc5")) cookNativeTextureTargetSetBc5 = json["cookNativeTextureTargetSetBc5"].get<bool>();
        if (json.contains("cookNativeTextureTargetSetBc4")) cookNativeTextureTargetSetBc4 = json["cookNativeTextureTargetSetBc4"].get<bool>();
        if (json.contains("cookNativeTextureTargetSetRgba8Srgb")) cookNativeTextureTargetSetRgba8Srgb = json["cookNativeTextureTargetSetRgba8Srgb"].get<bool>();
        if (json.contains("cookNativeTextureTargetSetRgba8Unorm")) cookNativeTextureTargetSetRgba8Unorm = json["cookNativeTextureTargetSetRgba8Unorm"].get<bool>();
        if (json.contains("cookNativeTextureTargetSetRgba16f")) cookNativeTextureTargetSetRgba16f = json["cookNativeTextureTargetSetRgba16f"].get<bool>();
        if (json.contains("cookNativeTextureTargetSetLibrary") && json["cookNativeTextureTargetSetLibrary"].is_array()) {
            cookNativeTextureTargetSetLibrary.clear();
            for (const nlohmann::json& item : json["cookNativeTextureTargetSetLibrary"]) {
                if (!item.is_object()) {
                    continue;
                }
                EditorNativeTextureTargetSetLibraryProfile profile;
                profile.name = item.value("name", std::string{});
                profile.bc7SrgbSampled = item.value("bc7SrgbSampled", profile.bc7SrgbSampled);
                profile.bc7UnormSampled = item.value("bc7UnormSampled", profile.bc7UnormSampled);
                profile.bc5UnormSampled = item.value("bc5UnormSampled", profile.bc5UnormSampled);
                profile.bc4UnormSampled = item.value("bc4UnormSampled", profile.bc4UnormSampled);
                profile.rgba8SrgbSampled = item.value("rgba8SrgbSampled", profile.rgba8SrgbSampled);
                profile.rgba8UnormSampled = item.value("rgba8UnormSampled", profile.rgba8UnormSampled);
                profile.rgba16fSampled = item.value("rgba16fSampled", profile.rgba16fSampled);
                cookNativeTextureTargetSetLibrary.push_back(std::move(profile));
            }
            normalizeNativeTextureTargetSetLibrary(cookNativeTextureTargetSetLibrary);
        }
        if (json.contains("viewportAxesVisible")) viewportAxesVisible = json["viewportAxesVisible"].get<bool>();
        if (json.contains("viewportLocalTransformFrame")) viewportLocalTransformFrame = json["viewportLocalTransformFrame"].get<bool>();
        if (json.contains("renderSequenceFramesPerTimelineFrame")) renderSequenceFramesPerTimelineFrame = std::clamp(json["renderSequenceFramesPerTimelineFrame"].get<int>(), 1, 512);
        if (json.contains("confirmDelete")) confirmDelete = json["confirmDelete"].get<bool>();
        if (json.contains("viewportDropForceGridByDefault")) viewportDropForceGridByDefault = json["viewportDropForceGridByDefault"].get<bool>();
        if (json.contains("viewportDropSurfaceAlignByDefault")) viewportDropSurfaceAlignByDefault = json["viewportDropSurfaceAlignByDefault"].get<bool>();
        if (json.contains("viewportDropDuplicatePlacementByDefault")) viewportDropDuplicatePlacementByDefault = json["viewportDropDuplicatePlacementByDefault"].get<bool>();
        if (json.contains("viewportDropMultiPlaceByDefault")) viewportDropMultiPlaceByDefault = json["viewportDropMultiPlaceByDefault"].get<bool>();
        if (json.contains("viewportDropMouseWheelRotationEnabled")) viewportDropMouseWheelRotationEnabled = json["viewportDropMouseWheelRotationEnabled"].get<bool>();
        if (json.contains("viewportPickMeshEntities")) viewportPickMeshEntities = json["viewportPickMeshEntities"].get<bool>();
        if (json.contains("viewportPickActorIcons")) viewportPickActorIcons = json["viewportPickActorIcons"].get<bool>();
        if (json.contains("viewportSurfaceSnappingEnabled")) viewportSurfaceSnappingEnabled = json["viewportSurfaceSnappingEnabled"].get<bool>();
        if (json.contains("viewportSurfaceSnapAlignToNormal")) viewportSurfaceSnapAlignToNormal = json["viewportSurfaceSnapAlignToNormal"].get<bool>();
        if (json.contains("viewportSurfaceSnapPreserveYaw")) viewportSurfaceSnapPreserveYaw = json["viewportSurfaceSnapPreserveYaw"].get<bool>();
        if (json.contains("viewportSurfaceSnapOffset")) viewportSurfaceSnapOffset = std::clamp(json["viewportSurfaceSnapOffset"].get<float>(), -100.0f, 100.0f);
        if (json.contains("viewportSurfaceSnapBoundsBottom")) viewportSurfaceSnapBoundsBottom = json["viewportSurfaceSnapBoundsBottom"].get<bool>();
        if (json.contains("viewportSurfaceSnapAxisConstraint")) viewportSurfaceSnapAxisConstraint = std::clamp(json["viewportSurfaceSnapAxisConstraint"].get<int>(), 0, 3);
        if (json.contains("viewportScatterPaletteByDefault")) viewportScatterPaletteByDefault = json["viewportScatterPaletteByDefault"].get<bool>();
        if (json.contains("viewportScatterPaletteDensity")) viewportScatterPaletteDensity = std::clamp(json["viewportScatterPaletteDensity"].get<float>(), 0.0f, 10000.0f);
        if (json.contains("viewportScatterPaletteSlopeMinDegrees")) viewportScatterPaletteSlopeMinDegrees = std::clamp(json["viewportScatterPaletteSlopeMinDegrees"].get<float>(), 0.0f, 180.0f);
        if (json.contains("viewportScatterPaletteSlopeMaxDegrees")) viewportScatterPaletteSlopeMaxDegrees = std::clamp(json["viewportScatterPaletteSlopeMaxDegrees"].get<float>(), 0.0f, 180.0f);
        if (json.contains("viewportScatterPaletteHeightMin")) viewportScatterPaletteHeightMin = json["viewportScatterPaletteHeightMin"].get<float>();
        if (json.contains("viewportScatterPaletteHeightMax")) viewportScatterPaletteHeightMax = json["viewportScatterPaletteHeightMax"].get<float>();
        if (json.contains("viewportScatterPaletteScaleMin")) viewportScatterPaletteScaleMin = std::clamp(json["viewportScatterPaletteScaleMin"].get<float>(), 0.001f, 1000.0f);
        if (json.contains("viewportScatterPaletteScaleMax")) viewportScatterPaletteScaleMax = std::clamp(json["viewportScatterPaletteScaleMax"].get<float>(), 0.001f, 1000.0f);
        if (json.contains("viewportScatterPaletteYawRandomDegrees")) viewportScatterPaletteYawRandomDegrees = std::clamp(json["viewportScatterPaletteYawRandomDegrees"].get<float>(), 0.0f, 360.0f);
        if (json.contains("viewportScatterPaletteSeed")) viewportScatterPaletteSeed = json["viewportScatterPaletteSeed"].get<uint32_t>();
        if (json.contains("viewportScatterPaletteSpacing")) viewportScatterPaletteSpacing = std::clamp(json["viewportScatterPaletteSpacing"].get<float>(), 0.001f, 10000.0f);
        if (json.contains("viewportScatterPaletteCollisionRadius")) viewportScatterPaletteCollisionRadius = std::clamp(json["viewportScatterPaletteCollisionRadius"].get<float>(), 0.0f, 10000.0f);
        if (json.contains("viewportScatterPaletteSurfaceAlignment")) viewportScatterPaletteSurfaceAlignment = json["viewportScatterPaletteSurfaceAlignment"].get<bool>();
        if (viewportScatterPaletteSlopeMaxDegrees < viewportScatterPaletteSlopeMinDegrees) std::swap(viewportScatterPaletteSlopeMinDegrees, viewportScatterPaletteSlopeMaxDegrees);
        if (viewportScatterPaletteHeightMax < viewportScatterPaletteHeightMin) std::swap(viewportScatterPaletteHeightMin, viewportScatterPaletteHeightMax);
        if (viewportScatterPaletteScaleMax < viewportScatterPaletteScaleMin) std::swap(viewportScatterPaletteScaleMin, viewportScatterPaletteScaleMax);
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
