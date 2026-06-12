#include "rtv/NativeAssetMigration.h"

#include "rtv/RtpkgIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace rtv {
namespace {

NativeBinaryError makeMigrationError(
    NativeBinaryErrorCode code,
    const std::filesystem::path& path,
    std::string table,
    uint64_t offset,
    uint64_t expectedSize,
    std::string message) {
    NativeBinaryError error;
    error.code = code;
    error.path = path;
    error.table = std::move(table);
    error.offset = offset;
    error.expectedSize = expectedSize;
    error.message = std::move(message);
    return error;
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes, NativeBinaryError* error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (error) {
            *error = makeMigrationError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not open file for migration");
        }
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        if (error) {
            *error = makeMigrationError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not determine file size for migration");
        }
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!file.good() && size != 0) {
        if (error) {
            *error = makeMigrationError(NativeBinaryErrorCode::IoError, path, "file", 0, static_cast<uint64_t>(size), "Could not read complete file for migration");
        }
        return false;
    }
    return true;
}

bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes, NativeBinaryError* error) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) {
                *error = makeMigrationError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not create migration output directory: " + ec.message());
            }
            return false;
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (error) {
            *error = makeMigrationError(NativeBinaryErrorCode::IoError, path, "file", 0, bytes.size(), "Could not open migration temp file for writing");
        }
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file.good()) {
        if (error) {
            *error = makeMigrationError(NativeBinaryErrorCode::IoError, path, "file", 0, bytes.size(), "Could not write complete migration temp file");
        }
        return false;
    }
    return true;
}

std::string timestampToken() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(millis);
}

std::filesystem::path uniqueBackupPath(const std::filesystem::path& path) {
    const std::string base = path.string() + ".before_migrate." + timestampToken();
    std::filesystem::path candidate = base + ".bak";
    for (uint32_t i = 1; std::filesystem::exists(candidate); ++i) {
        candidate = base + "." + std::to_string(i) + ".bak";
    }
    return candidate;
}

bool replaceFileAtomically(const std::filesystem::path& tempPath, const std::filesystem::path& destination, std::error_code& ec) {
    ec.clear();
#if defined(_WIN32)
    if (MoveFileExW(tempPath.wstring().c_str(), destination.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
#else
    std::filesystem::rename(tempPath, destination, ec);
    return !ec;
#endif
}

NativeAssetInspection inspectForMigration(const std::filesystem::path& path, bool package) {
    if (package) {
        RtpkgReader reader;
        return reader.inspect(path, true).native;
    }
    NativeAssetReader reader;
    return reader.inspect(path, true);
}

bool validateMigratedFile(const std::filesystem::path& path, bool package, NativeAssetMigrationReport& report) {
    const NativeAssetInspection inspection = inspectForMigration(path, package);
    if (!inspection.ok) {
        report.errors.insert(report.errors.end(), inspection.errors.begin(), inspection.errors.end());
        if (inspection.errors.empty()) {
            report.errors.push_back(makeMigrationError(NativeBinaryErrorCode::MigrationFailed, path, "file", 0, 0, "Migrated file validation failed"));
        }
        return false;
    }
    if (inspection.header.contentVersion != kNativeAssetReadableVersionMax) {
        report.errors.push_back(makeMigrationError(NativeBinaryErrorCode::MigrationFailed, path, "header", offsetof(NativeAssetHeader, contentVersion), sizeof(uint32_t), "Migrated file did not reach the current content version"));
        return false;
    }
    return true;
}

} // namespace

void NativeAssetMigratorRegistry::registerMigration(NativeAssetKind kind, uint32_t fromVersion, uint32_t toVersion, std::string name) {
    steps_.push_back(NativeAssetMigrationStep{.kind = kind, .fromVersion = fromVersion, .toVersion = toVersion, .name = std::move(name)});
}

std::vector<NativeAssetMigrationStep> NativeAssetMigratorRegistry::plan(NativeAssetKind kind, uint32_t fromVersion, uint32_t toVersion) const {
    std::vector<NativeAssetMigrationStep> result;
    uint32_t current = fromVersion;
    uint32_t guard = 0;
    while (current < toVersion && guard++ < 32) {
        const auto it = std::find_if(steps_.begin(), steps_.end(), [&](const NativeAssetMigrationStep& step) {
            return step.kind == kind && step.fromVersion == current && step.toVersion > current && step.toVersion <= toVersion;
        });
        if (it == steps_.end()) {
            result.clear();
            return result;
        }
        result.push_back(*it);
        current = it->toVersion;
    }
    if (current != toVersion) {
        result.clear();
    }
    return result;
}

bool NativeAssetMigratorRegistry::canMigrate(NativeAssetKind kind, uint32_t fromVersion, uint32_t toVersion) const {
    return fromVersion == toVersion || !plan(kind, fromVersion, toVersion).empty();
}

NativeAssetMigratorRegistry NativeAssetMigratorRegistry::createDefault() {
    NativeAssetMigratorRegistry registry;
    const NativeAssetKind kinds[] = {
        NativeAssetKind::Mesh,
        NativeAssetKind::Material,
        NativeAssetKind::Texture,
        NativeAssetKind::Skeleton,
        NativeAssetKind::Animation,
        NativeAssetKind::AnimationController,
        NativeAssetKind::SkeletalMesh,
        NativeAssetKind::Package,
    };
    for (NativeAssetKind kind : kinds) {
        registry.registerMigration(kind, 0, kNativeAssetReadableVersionMax, "HeaderContentVersion0To1");
    }
    return registry;
}

NativeAssetMigrationReport migrateNativeAssetFile(
    const std::filesystem::path& path,
    const NativeAssetMigrationOptions& options,
    const NativeAssetMigratorRegistry& registry) {
    NativeAssetMigrationReport report;
    report.path = path;
    report.dryRun = options.dryRun;
    report.package = options.package;
    report.toVersion = kNativeAssetReadableVersionMax;

    const NativeAssetInspection inspection = inspectForMigration(path, options.package);
    if (!inspection.ok) {
        report.errors = inspection.errors;
        return report;
    }

    report.kind = static_cast<NativeAssetKind>(inspection.header.assetKind);
    report.fromVersion = inspection.header.contentVersion;
    report.minimumReadableVersion = inspection.header.minimumReaderVersion;
    report.migrationRequired = inspection.header.contentVersion < kNativeAssetReadableVersionMax;
    if (!report.migrationRequired) {
        report.ok = true;
        return report;
    }

    report.steps = registry.plan(report.kind, inspection.header.contentVersion, kNativeAssetReadableVersionMax);
    report.migrationAvailable = !report.steps.empty();
    if (!report.migrationAvailable) {
        report.errors.push_back(makeMigrationError(NativeBinaryErrorCode::MigrationRequired, path, "migrationRegistry", inspection.header.contentVersion, kNativeAssetReadableVersionMax, "No registered native asset migration path is available"));
        return report;
    }
    if (options.dryRun) {
        report.ok = true;
        return report;
    }

    std::vector<std::byte> bytes;
    NativeBinaryError ioError;
    if (!readFileBytes(path, bytes, &ioError)) {
        report.errors.push_back(ioError);
        return report;
    }
    if (bytes.size() < sizeof(NativeAssetHeader)) {
        report.errors.push_back(makeMigrationError(NativeBinaryErrorCode::CorruptHeader, path, "header", 0, sizeof(NativeAssetHeader), "File is too small for native migration"));
        return report;
    }

    report.backupPath = uniqueBackupPath(path);
    std::error_code ec;
    std::filesystem::copy_file(path, report.backupPath, std::filesystem::copy_options::none, ec);
    if (ec) {
        report.errors.push_back(makeMigrationError(NativeBinaryErrorCode::IoError, report.backupPath, "backup", 0, 0, "Could not create migration backup: " + ec.message()));
        return report;
    }
    report.backupCreated = true;

    NativeAssetHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.contentVersion = kNativeAssetReadableVersionMax;
    std::memcpy(bytes.data(), &header, sizeof(header));

    report.tempPath = path.string() + ".migrate.tmp";
    report.mutationAttempted = true;
    if (!writeFileBytes(report.tempPath, bytes, &ioError)) {
        report.errors.push_back(ioError);
        return report;
    }
    if (!validateMigratedFile(report.tempPath, options.package, report)) {
        std::filesystem::remove(report.tempPath, ec);
        return report;
    }

    if (!replaceFileAtomically(report.tempPath, path, ec)) {
        report.errors.push_back(makeMigrationError(NativeBinaryErrorCode::IoError, path, "replace", 0, bytes.size(), "Could not replace original after migration validation: " + ec.message()));
        return report;
    }
    if (!validateMigratedFile(path, options.package, report)) {
        report.warnings.push_back("Migrated destination failed validation after replacement; backup remains available for manual recovery.");
        return report;
    }

    report.mutated = true;
    report.ok = true;
    return report;
}

nlohmann::json nativeAssetMigrationReportToJson(const NativeAssetMigrationReport& report) {
    nlohmann::json steps = nlohmann::json::array();
    for (const NativeAssetMigrationStep& step : report.steps) {
        steps.push_back({
            {"kind", nativeAssetKindName(step.kind)},
            {"from_version", step.fromVersion},
            {"to_version", step.toVersion},
            {"name", step.name},
        });
    }
    nlohmann::json errors = nlohmann::json::array();
    for (const NativeBinaryError& error : report.errors) {
        errors.push_back({
            {"code", nativeBinaryErrorCodeName(error.code)},
            {"path", error.path.generic_string()},
            {"table", error.table},
            {"offset", error.offset},
            {"expected_size", error.expectedSize},
            {"message", error.message},
        });
    }
    return {
        {"ok", report.ok},
        {"dry_run", report.dryRun},
        {"package", report.package},
        {"path", report.path.generic_string()},
        {"kind", nativeAssetKindName(report.kind)},
        {"from_version", report.fromVersion},
        {"to_version", report.toVersion},
        {"minimum_readable_version", report.minimumReadableVersion},
        {"migration_required", report.migrationRequired},
        {"migration_available", report.migrationAvailable},
        {"mutation_attempted", report.mutationAttempted},
        {"mutated", report.mutated},
        {"backup_created", report.backupCreated},
        {"backup_path", report.backupPath.empty() ? std::string{} : report.backupPath.generic_string()},
        {"temp_path", report.tempPath.empty() ? std::string{} : report.tempPath.generic_string()},
        {"steps", steps},
        {"openProductionScope", {
            {"currentReportScope", report.package ? "content-browser-rtpkg-migration" : "content-browser-native-asset-migration"},
            {"implementedScope", nlohmann::json::array({
                "dry-run-reporting",
                "explicit-confirmation-before-mutation",
                "timestamped-backup-before-replace",
                "temp-output-validation-before-replace",
                "idempotent-current-version-noop",
                "completed-job-center-telemetry",
                "active-progress-snapshot-contract",
                "background-migration-worker-scheduling",
                "active-background-progress-snapshots",
                "queued-background-migration-scheduling",
                "folder-batch-migration-aggregation",
                "recursive-cross-folder-batch-planning"
            })},
            {"openMigrationScope", nlohmann::json::array({
                "future-nontrivial-payload-schema-migration-fixtures",
                "provider-executed-checkout-lock-submit-before-mutation"
            })},
            {"dryRunOnly", report.dryRun},
            {"backgroundMigrationWorkerImplemented", true},
            {"futurePayloadSchemaMigrationFixturesAvailable", false},
            {"providerExecutedSourceControlMutationImplemented", false},
        }},
        {"warnings", report.warnings},
        {"errors", errors},
    };
}

int migrateNativeAssetCommand(const std::filesystem::path& path, const std::filesystem::path& reportPath, bool dryRun) {
    NativeAssetMigrationOptions options;
    options.dryRun = dryRun;
    options.package = false;
    const NativeAssetMigrationReport report = migrateNativeAssetFile(path, options);
    const nlohmann::json json = nativeAssetMigrationReportToJson(report);
    if (!reportPath.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = reportPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
        std::ofstream out(reportPath);
        if (!out.is_open()) {
            std::cerr << "Failed to write native asset migration report: " << reportPath << '\n';
            return 1;
        }
        out << json.dump(2);
    } else {
        std::cout << json.dump(2) << '\n';
    }
    return report.ok ? 0 : 1;
}

int migratePackageCommand(const std::filesystem::path& path, const std::filesystem::path& reportPath, bool dryRun) {
    NativeAssetMigrationOptions options;
    options.dryRun = dryRun;
    options.package = true;
    const NativeAssetMigrationReport report = migrateNativeAssetFile(path, options);
    const nlohmann::json json = nativeAssetMigrationReportToJson(report);
    if (!reportPath.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = reportPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
        std::ofstream out(reportPath);
        if (!out.is_open()) {
            std::cerr << "Failed to write native package migration report: " << reportPath << '\n';
            return 1;
        }
        out << json.dump(2);
    } else {
        std::cout << json.dump(2) << '\n';
    }
    return report.ok ? 0 : 1;
}

} // namespace rtv


