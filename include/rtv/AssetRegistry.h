#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

using AssetGuid = std::string;

enum class AssetType {
    Unknown,
    Mesh,
    Material,
    Texture,
    HDRI,
    Scene,
    Prefab,
    Animation,
    Skeleton,
    SkeletalMesh,
    AnimationController,
};

enum class AssetImportStatus {
    Unknown,
    Imported,
    Missing,
    Stale,
    Failed,
};

enum class AssetRegistryDirtyReason {
    AssetImported,
    AssetReimported,
    AssetDeleted,
    AssetRenamed,
    AssetMoved,
    AssetEdited,
    AssetDependencyChanged,
    AssetTagsChanged,
    AssetAutosaveRestored,
};

struct AssetImportSettings {
    bool copySourceIntoProject = false;
    bool preserveHierarchy = true;
    bool importMaterials = true;
    bool importTextures = true;
    bool importCameras = true;
    bool importLights = true;
    bool generateTangents = true;
    bool buildBlasCache = true;
    bool generatePrefabAsset = true;
    bool buildCookedPayloadsNow = true;
    bool generateThumbnails = true;
    float unitScale = 1.0f;
    // Optional source-package emissive scaling applied to imported material emissive factors.
    // 1.0 leaves emissive untouched. Used to normalize over-bright authored emissive from
    // some source packages without editing the source asset.
    float emissiveScale = 1.0f;
    std::string coordinateConversion = "None";
    std::string materialImportMode = "ImportMaterials";
    std::string textureImportMode = "ImportTextures";
    std::string textureCompression = "PreserveSource";
};

struct AssetDependency {
    AssetGuid guid;
    std::string kind;
};

struct AssetRecord {
    AssetGuid guid;
    AssetType type = AssetType::Unknown;
    std::string displayName;
    std::string sourcePath;
    std::string importedPath;
    std::string cachePath;
    std::string thumbnailPath;
    std::string importGroupId;
    std::string importGroupName;
    AssetGuid importRootGuid;
    std::vector<AssetDependency> dependencies;
    std::vector<AssetGuid> references;
    std::string sourceHash;
    std::string importedHash;
    std::string importSettingsHash;
    std::string lastModifiedTimestamp;
    std::vector<std::string> tags;
    AssetImportSettings importSettings;
    AssetImportStatus status = AssetImportStatus::Unknown;
    bool missing = false;
    bool stale = false;
    bool sourceMissing = false;
    bool importedMetadataMissing = false;
    bool cookedPayloadMissing = false;
    bool dependenciesMissing = false;
};

struct AssetRegistryState {
    bool dirty = false;
    std::filesystem::path path;
    std::vector<AssetRegistryDirtyReason> dirtyReasons;
};

class AssetRegistry {
public:
    [[nodiscard]] const std::vector<AssetRecord>& records() const { return records_; }
    [[nodiscard]] const AssetRegistryState& state() const { return state_; }
    [[nodiscard]] bool dirty() const { return state_.dirty; }

    void clear();
    void setPath(std::filesystem::path path);
    [[nodiscard]] bool load(const std::filesystem::path& path, std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path) const;
    [[nodiscard]] bool save() const;
    void markDirty(AssetRegistryDirtyReason reason);
    void clearDirty();
    void addOrReplaceRecord(AssetRecord record, AssetRegistryDirtyReason reason = AssetRegistryDirtyReason::AssetImported);
    [[nodiscard]] size_t removeRecords(const std::vector<AssetGuid>& guids, AssetRegistryDirtyReason reason = AssetRegistryDirtyReason::AssetDeleted);
    [[nodiscard]] bool refreshRecordHealth(const std::filesystem::path& root, bool markDirtyOnChange = true);

private:
    AssetRegistryState state_{};
    std::vector<AssetRecord> records_;
};

[[nodiscard]] const char* assetTypeName(AssetType type);
[[nodiscard]] AssetType assetTypeFromName(const std::string& name);
[[nodiscard]] const char* assetImportStatusName(AssetImportStatus status);
[[nodiscard]] AssetImportStatus assetImportStatusFromName(const std::string& name);
[[nodiscard]] AssetGuid generateAssetGuid();

} // namespace rtv
