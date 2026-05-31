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
    AssetDependencyChanged,
};

struct AssetImportSettings {
    bool preserveHierarchy = true;
    bool importMaterials = true;
    bool importTextures = true;
    bool importCameras = true;
    bool importLights = true;
    bool generateTangents = true;
    bool buildBlasCache = true;
    float unitScale = 1.0f;
    std::string coordinateConversion = "None";
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
    std::vector<AssetDependency> dependencies;
    std::vector<AssetGuid> references;
    std::string sourceHash;
    std::string importedHash;
    std::string importSettingsHash;
    std::string lastModifiedTimestamp;
    AssetImportSettings importSettings;
    AssetImportStatus status = AssetImportStatus::Unknown;
    bool missing = false;
    bool stale = false;
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
