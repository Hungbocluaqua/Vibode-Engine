#pragma once

#include "rtv/AssetRegistry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rtv {

struct AssetImportRequest {
    std::filesystem::path sourcePath;
    std::filesystem::path destinationFolder = "Models";
    std::string mode = "ImportAsset";
    AssetImportSettings settings{};
};

struct StagedAssetImportResult {
    bool success = false;
    AssetRecord record{};
    std::vector<AssetRecord> records;
    std::vector<std::filesystem::path> generatedFiles;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::filesystem::path importReportPath;
};

struct AssetImportWorkspace {
    std::filesystem::path root;
    std::filesystem::path contentRoot;
    std::filesystem::path cacheRoot;
    std::filesystem::path registryPath;
    bool compatibilityMode = false;
};

[[nodiscard]] AssetType assetTypeForSourcePath(const std::filesystem::path& path);
[[nodiscard]] std::string assetSourceHashForPath(const std::filesystem::path& path);
[[nodiscard]] std::string assetImportSettingsHashForRequest(const AssetImportRequest& request);
[[nodiscard]] AssetGuid importedAssetGuidFor(
    std::string_view sourceHash,
    std::string_view settingsHash,
    std::string_view kind,
    size_t index);
[[nodiscard]] StagedAssetImportResult stagePlaceholderAssetImport(
    const AssetImportRequest& request,
    const AssetImportWorkspace& workspace);

} // namespace rtv
