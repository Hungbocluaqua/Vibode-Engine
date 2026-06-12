#pragma once

#include "rtv/NativeBinaryIO.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

struct NativeAssetMigrationStep {
    NativeAssetKind kind = NativeAssetKind::Unknown;
    uint32_t fromVersion = 0;
    uint32_t toVersion = 0;
    std::string name;
};

class NativeAssetMigratorRegistry {
public:
    void registerMigration(NativeAssetKind kind, uint32_t fromVersion, uint32_t toVersion, std::string name);
    [[nodiscard]] std::vector<NativeAssetMigrationStep> plan(NativeAssetKind kind, uint32_t fromVersion, uint32_t toVersion) const;
    [[nodiscard]] bool canMigrate(NativeAssetKind kind, uint32_t fromVersion, uint32_t toVersion) const;
    [[nodiscard]] static NativeAssetMigratorRegistry createDefault();

private:
    std::vector<NativeAssetMigrationStep> steps_;
};

struct NativeAssetMigrationOptions {
    bool dryRun = false;
    bool package = false;
};

struct NativeAssetMigrationReport {
    bool ok = false;
    bool dryRun = false;
    bool package = false;
    bool mutationAttempted = false;
    bool mutated = false;
    bool backupCreated = false;
    bool migrationRequired = false;
    bool migrationAvailable = false;
    NativeAssetKind kind = NativeAssetKind::Unknown;
    uint32_t fromVersion = 0;
    uint32_t toVersion = kNativeAssetReadableVersionMax;
    uint32_t minimumReadableVersion = kNativeAssetReadableVersionMin;
    std::filesystem::path path;
    std::filesystem::path backupPath;
    std::filesystem::path tempPath;
    std::vector<NativeAssetMigrationStep> steps;
    std::vector<std::string> warnings;
    std::vector<NativeBinaryError> errors;
};

[[nodiscard]] NativeAssetMigrationReport migrateNativeAssetFile(
    const std::filesystem::path& path,
    const NativeAssetMigrationOptions& options,
    const NativeAssetMigratorRegistry& registry = NativeAssetMigratorRegistry::createDefault());

[[nodiscard]] nlohmann::json nativeAssetMigrationReportToJson(const NativeAssetMigrationReport& report);
[[nodiscard]] int migrateNativeAssetCommand(const std::filesystem::path& path, const std::filesystem::path& reportPath = {}, bool dryRun = false);
[[nodiscard]] int migratePackageCommand(const std::filesystem::path& path, const std::filesystem::path& reportPath = {}, bool dryRun = false);

} // namespace rtv
