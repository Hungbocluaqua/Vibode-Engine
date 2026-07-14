#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

struct EditorAssetCollection {
    std::string name;
    std::vector<std::string> assetGuids;
};

struct EditorNativeTextureTargetSetLibraryProfile {
    std::string name = "editor-custom-target-set";
    bool bc7SrgbSampled = true;
    bool bc7UnormSampled = true;
    bool bc5UnormSampled = true;
    bool bc4UnormSampled = true;
    bool rgba8SrgbSampled = true;
    bool rgba8UnormSampled = true;
    bool rgba16fSampled = true;
};

struct EditorNativeTextureTargetSetCustomProfile {
    std::string platformName = "editor-custom-target-set";
    bool bc7SrgbSampled = true;
    bool bc7UnormSampled = true;
    bool bc5UnormSampled = true;
    bool bc4UnormSampled = true;
    bool rgba8SrgbSampled = true;
    bool rgba8UnormSampled = true;
    bool rgba16fSampled = true;
};

struct EditorPreferences {
    float cameraMoveSpeed = 2.4f;
    float cameraFastMoveSpeed = 7.5f;
    float cameraMouseSensitivity = 0.0025f;
    bool cameraInvertLookX = false;
    bool cameraInvertLookY = false;
    bool gridVisible = true;
    bool showHud = true;
    bool linkedScale = false;
    bool viewportSnapEnabled = false;
    float viewportTranslationSnap = 0.25f;
    float viewportRotationSnap = 15.0f;
    float viewportScaleSnap = 0.1f;
    float hudScale = 1.0f;
    float uiScale = 1.0f;
    int themePreset = 0;
    int workspacePreset = 0;
    int layoutVersion = 4;
    int contentBrowserMode = 0;
    bool contentBrowserGridView = true;
    bool contentBrowserShowDetails = false;
    bool cookEmitNativeTextureTargetSets = false;
    int cookNativeTextureTargetSetProfile = 0;
    std::string cookNativeTextureTargetSetName = "editor-custom-target-set";
    bool cookNativeTextureTargetSetBc7Srgb = true;
    bool cookNativeTextureTargetSetBc7Unorm = true;
    bool cookNativeTextureTargetSetBc5 = true;
    bool cookNativeTextureTargetSetBc4 = true;
    bool cookNativeTextureTargetSetRgba8Srgb = true;
    bool cookNativeTextureTargetSetRgba8Unorm = true;
    bool cookNativeTextureTargetSetRgba16f = true;
    std::vector<EditorNativeTextureTargetSetLibraryProfile> cookNativeTextureTargetSetLibrary;
    bool viewportAxesVisible = true;
    bool viewportLocalTransformFrame = false;
    int renderSequenceFramesPerTimelineFrame = 1;
    bool confirmDelete = true;
    bool viewportDropForceGridByDefault = false;
    bool viewportDropSurfaceAlignByDefault = false;
    bool viewportDropDuplicatePlacementByDefault = false;
    bool viewportDropMultiPlaceByDefault = false;
    bool viewportDropMouseWheelRotationEnabled = true;
    bool viewportPickMeshEntities = true;
    bool viewportPickActorIcons = true;
    bool viewportSurfaceSnappingEnabled = false;
    bool viewportSurfaceSnapAlignToNormal = true;
    bool viewportSurfaceSnapPreserveYaw = true;
    float viewportSurfaceSnapOffset = 0.0f;
    bool viewportSurfaceSnapBoundsBottom = true;
    int viewportSurfaceSnapAxisConstraint = 0;
    bool viewportScatterPaletteByDefault = false;
    float viewportScatterPaletteDensity = 1.0f;
    float viewportScatterPaletteSlopeMinDegrees = 0.0f;
    float viewportScatterPaletteSlopeMaxDegrees = 60.0f;
    float viewportScatterPaletteHeightMin = -10000.0f;
    float viewportScatterPaletteHeightMax = 10000.0f;
    float viewportScatterPaletteScaleMin = 1.0f;
    float viewportScatterPaletteScaleMax = 1.0f;
    float viewportScatterPaletteYawRandomDegrees = 180.0f;
    uint32_t viewportScatterPaletteSeed = 1;
    float viewportScatterPaletteSpacing = 1.0f;
    float viewportScatterPaletteCollisionRadius = 0.25f;
    bool viewportScatterPaletteSurfaceAlignment = true;
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
