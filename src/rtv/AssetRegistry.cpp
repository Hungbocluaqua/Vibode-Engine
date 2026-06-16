#include "rtv/AssetRegistry.h"

#include "rtv/Project.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace rtv {

namespace {

nlohmann::json importSettingsJson(const AssetImportSettings& settings) {
    return {
        {"copySourceIntoProject", settings.copySourceIntoProject},
        {"preserveHierarchy", settings.preserveHierarchy},
        {"importMaterials", settings.importMaterials},
        {"importTextures", settings.importTextures},
        {"importCameras", settings.importCameras},
        {"importLights", settings.importLights},
        {"generateTangents", settings.generateTangents},
        {"buildBlasCache", settings.buildBlasCache},
        {"generatePrefabAsset", settings.generatePrefabAsset},
        {"buildCookedPayloadsNow", settings.buildCookedPayloadsNow},
        {"generateThumbnails", settings.generateThumbnails},
        {"unitScale", settings.unitScale},
        {"emissiveScale", settings.emissiveScale},
        {"coordinateConversion", settings.coordinateConversion},
        {"materialImportMode", settings.materialImportMode},
        {"textureImportMode", settings.textureImportMode},
        {"textureCompression", settings.textureCompression},
    };
}

AssetImportSettings importSettingsFromJson(const nlohmann::json& json) {
    AssetImportSettings settings;
    if (!json.is_object()) {
        return settings;
    }
    settings.copySourceIntoProject = json.value("copySourceIntoProject", settings.copySourceIntoProject);
    settings.preserveHierarchy = json.value("preserveHierarchy", settings.preserveHierarchy);
    settings.importMaterials = json.value("importMaterials", settings.importMaterials);
    settings.importTextures = json.value("importTextures", settings.importTextures);
    settings.importCameras = json.value("importCameras", settings.importCameras);
    settings.importLights = json.value("importLights", settings.importLights);
    settings.generateTangents = json.value("generateTangents", settings.generateTangents);
    settings.buildBlasCache = json.value("buildBlasCache", settings.buildBlasCache);
    settings.generatePrefabAsset = json.value("generatePrefabAsset", settings.generatePrefabAsset);
    settings.buildCookedPayloadsNow = json.value("buildCookedPayloadsNow", settings.buildCookedPayloadsNow);
    settings.generateThumbnails = json.value("generateThumbnails", settings.generateThumbnails);
    settings.unitScale = json.value("unitScale", settings.unitScale);
    settings.emissiveScale = json.value("emissiveScale", settings.emissiveScale);
    settings.coordinateConversion = json.value("coordinateConversion", settings.coordinateConversion);
    settings.materialImportMode = json.value("materialImportMode", settings.materialImportMode);
    settings.textureImportMode = json.value("textureImportMode", settings.textureImportMode);
    settings.textureCompression = json.value("textureCompression", settings.textureCompression);
    return settings;
}

std::filesystem::path resolveRecordPath(const std::filesystem::path& root, const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::filesystem::path resolved = path;
    if (!resolved.is_absolute()) {
        resolved = root / resolved;
    }
    return resolved;
}

bool regularFileExists(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

uint64_t writeStamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto value = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    return static_cast<uint64_t>(value.time_since_epoch().count());
}

bool assetRecordSaveLess(const AssetRecord& lhs, const AssetRecord& rhs) {
    if (lhs.importGroupName != rhs.importGroupName) return lhs.importGroupName < rhs.importGroupName;
    if (lhs.importGroupId != rhs.importGroupId) return lhs.importGroupId < rhs.importGroupId;
    if (lhs.importRootGuid != rhs.importRootGuid) return lhs.importRootGuid < rhs.importRootGuid;
    if ((lhs.guid == lhs.importRootGuid) != (rhs.guid == rhs.importRootGuid)) return lhs.guid == lhs.importRootGuid;
    if (lhs.type != rhs.type) return std::string_view(assetTypeName(lhs.type)) < std::string_view(assetTypeName(rhs.type));
    if (lhs.displayName != rhs.displayName) return lhs.displayName < rhs.displayName;
    if (lhs.guid != rhs.guid) return lhs.guid < rhs.guid;
    if (lhs.importedPath != rhs.importedPath) return lhs.importedPath < rhs.importedPath;
    return lhs.displayName < rhs.displayName;
}

bool assetDependencySaveLess(const AssetDependency& lhs, const AssetDependency& rhs) {
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    return lhs.guid < rhs.guid;
}

std::vector<std::string> normalizedTags(std::vector<std::string> tags) {
    for (std::string& tag : tags) {
        tag.erase(tag.begin(), std::find_if(tag.begin(), tag.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        tag.erase(std::find_if(tag.rbegin(), tag.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), tag.end());
    }
    tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& tag) { return tag.empty(); }), tags.end());
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

} // namespace

const char* assetTypeName(AssetType type) {
    switch (type) {
    case AssetType::Mesh: return "Mesh";
    case AssetType::Material: return "Material";
    case AssetType::Texture: return "Texture";
    case AssetType::HDRI: return "HDRI";
    case AssetType::Scene: return "Scene";
    case AssetType::Prefab: return "Prefab";
    case AssetType::Animation: return "Animation";
    case AssetType::Skeleton: return "Skeleton";
    case AssetType::SkeletalMesh: return "SkeletalMesh";
    case AssetType::AnimationController: return "AnimationController";
    case AssetType::Unknown: default: return "Unknown";
    }
}

AssetType assetTypeFromName(const std::string& name) {
    if (name == "Mesh") return AssetType::Mesh;
    if (name == "Material") return AssetType::Material;
    if (name == "Texture") return AssetType::Texture;
    if (name == "HDRI") return AssetType::HDRI;
    if (name == "Scene") return AssetType::Scene;
    if (name == "Prefab") return AssetType::Prefab;
    if (name == "Animation") return AssetType::Animation;
    if (name == "Skeleton") return AssetType::Skeleton;
    if (name == "SkeletalMesh") return AssetType::SkeletalMesh;
    if (name == "AnimationController") return AssetType::AnimationController;
    return AssetType::Unknown;
}

const char* assetImportStatusName(AssetImportStatus status) {
    switch (status) {
    case AssetImportStatus::Imported: return "Imported";
    case AssetImportStatus::Missing: return "Missing";
    case AssetImportStatus::Stale: return "Stale";
    case AssetImportStatus::Failed: return "Failed";
    case AssetImportStatus::Unknown: default: return "Unknown";
    }
}

AssetImportStatus assetImportStatusFromName(const std::string& name) {
    if (name == "Imported") return AssetImportStatus::Imported;
    if (name == "Missing") return AssetImportStatus::Missing;
    if (name == "Stale") return AssetImportStatus::Stale;
    if (name == "Failed") return AssetImportStatus::Failed;
    return AssetImportStatus::Unknown;
}

AssetGuid generateAssetGuid() {
    return generateProjectGuid();
}

void AssetRegistry::clear() {
    records_.clear();
    state_ = AssetRegistryState{};
}

void AssetRegistry::setPath(std::filesystem::path path) {
    state_.path = std::move(path);
}

bool AssetRegistry::load(const std::filesystem::path& path, std::string* error) {
    clear();
    state_.path = path;
    if (!std::filesystem::exists(path)) {
        return save(path);
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        if (error != nullptr) *error = "Could not open asset registry";
        return false;
    }

    try {
        nlohmann::json json;
        file >> json;
        nlohmann::json recordsJson = nlohmann::json::array();
        if (json.is_array()) {
            recordsJson = json;
        } else if (json.contains("assets")) {
            recordsJson = json["assets"];
        } else if (json.contains("records")) {
            recordsJson = json["records"];
        }

        if (recordsJson.is_array()) {
            for (const nlohmann::json& item : recordsJson) {
                if (!item.is_object()) {
                    continue;
                }
                AssetRecord record;
                record.guid = item.value("guid", item.value("assetGuid", std::string{}));
                record.type = assetTypeFromName(item.value("type", std::string("Unknown")));
                record.displayName = item.value("displayName", std::string{});
                record.sourcePath = item.value("sourcePath", std::string{});
                record.importedPath = item.value("importedPath", std::string{});
                record.cachePath = item.value("cachePath", std::string{});
                record.thumbnailPath = item.value("thumbnailPath", std::string{});
                record.importGroupId = item.value("importGroupId", std::string{});
                record.importGroupName = item.value("importGroupName", std::string{});
                record.importRootGuid = item.value("importRootGuid", std::string{});
                record.sourceHash = item.value("sourceHash", std::string{});
                record.importedHash = item.value("importedHash", std::string{});
                record.importSettingsHash = item.value("importSettingsHash", std::string{});
                record.lastModifiedTimestamp = item.value("lastModifiedTimestamp", std::string{});
                if (item.contains("tags") && item["tags"].is_array()) {
                    for (const nlohmann::json& tag : item["tags"]) {
                        if (tag.is_string()) {
                            record.tags.push_back(tag.get<std::string>());
                        }
                    }
                    record.tags = normalizedTags(std::move(record.tags));
                }
                record.missing = item.value("missing", false);
                record.stale = item.value("stale", false);
                record.sourceMissing = item.value("sourceMissing", false);
                record.importedMetadataMissing = item.value("importedMetadataMissing", false);
                record.cookedPayloadMissing = item.value("cookedPayloadMissing", false);
                record.dependenciesMissing = item.value("dependenciesMissing", false);
                record.status = assetImportStatusFromName(item.value("status", std::string("Unknown")));
                if (item.contains("importSettings")) {
                    record.importSettings = importSettingsFromJson(item["importSettings"]);
                }
                if (item.contains("dependencies") && item["dependencies"].is_array()) {
                    for (const nlohmann::json& dep : item["dependencies"]) {
                        if (dep.is_string()) {
                            record.dependencies.push_back(AssetDependency{dep.get<std::string>(), {}});
                        } else if (dep.is_object()) {
                            record.dependencies.push_back(AssetDependency{
                                dep.value("guid", dep.value("assetGuid", std::string{})),
                                dep.value("kind", std::string{}),
                            });
                        }
                    }
                }
                if (item.contains("references") && item["references"].is_array()) {
                    for (const nlohmann::json& ref : item["references"]) {
                        if (ref.is_string()) {
                            record.references.push_back(ref.get<std::string>());
                        }
                    }
                }
                records_.push_back(std::move(record));
            }
        }
        clearDirty();
        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr) *error = ex.what();
        return false;
    }
}

bool AssetRegistry::save(const std::filesystem::path& path) const {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    std::vector<AssetRecord> sortedRecords = records_;
    std::sort(sortedRecords.begin(), sortedRecords.end(), assetRecordSaveLess);

    nlohmann::json recordsJson = nlohmann::json::array();
    for (const AssetRecord& record : sortedRecords) {
        std::vector<AssetDependency> sortedDependencies = record.dependencies;
        std::sort(sortedDependencies.begin(), sortedDependencies.end(), assetDependencySaveLess);
        nlohmann::json dependencies = nlohmann::json::array();
        for (const AssetDependency& dependency : sortedDependencies) {
            dependencies.push_back({{"guid", dependency.guid}, {"kind", dependency.kind}});
        }
        std::vector<AssetGuid> sortedReferences = record.references;
        std::sort(sortedReferences.begin(), sortedReferences.end());
        std::vector<std::string> sortedTags = normalizedTags(record.tags);
        recordsJson.push_back({
            {"guid", record.guid},
            {"type", assetTypeName(record.type)},
            {"displayName", record.displayName},
            {"sourcePath", record.sourcePath},
            {"importedPath", record.importedPath},
            {"cachePath", record.cachePath},
            {"thumbnailPath", record.thumbnailPath},
            {"importGroupId", record.importGroupId},
            {"importGroupName", record.importGroupName},
            {"importRootGuid", record.importRootGuid},
            {"dependencies", dependencies},
            {"references", sortedReferences},
            {"sourceHash", record.sourceHash},
            {"importedHash", record.importedHash},
            {"importSettingsHash", record.importSettingsHash},
            {"lastModifiedTimestamp", record.lastModifiedTimestamp},
            {"tags", sortedTags},
            {"importSettings", importSettingsJson(record.importSettings)},
            {"status", assetImportStatusName(record.status)},
            {"missing", record.missing},
            {"stale", record.stale},
            {"sourceMissing", record.sourceMissing},
            {"importedMetadataMissing", record.importedMetadataMissing},
            {"cookedPayloadMissing", record.cookedPayloadMissing},
            {"dependenciesMissing", record.dependenciesMissing},
        });
    }

    nlohmann::json json;
    json["version"] = 1;
    json["assets"] = recordsJson;
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file << json.dump(2);
    return true;
}

bool AssetRegistry::save() const {
    return !state_.path.empty() && save(state_.path);
}

void AssetRegistry::markDirty(AssetRegistryDirtyReason reason) {
    state_.dirty = true;
    state_.dirtyReasons.push_back(reason);
}

void AssetRegistry::clearDirty() {
    state_.dirty = false;
    state_.dirtyReasons.clear();
}

void AssetRegistry::addOrReplaceRecord(AssetRecord record, AssetRegistryDirtyReason reason) {
    auto it = std::find_if(records_.begin(), records_.end(), [&](const AssetRecord& existing) {
        return existing.guid == record.guid;
    });
    if (it != records_.end()) {
        *it = std::move(record);
    } else {
        records_.push_back(std::move(record));
    }
    markDirty(reason);
}

size_t AssetRegistry::removeRecords(const std::vector<AssetGuid>& guids, AssetRegistryDirtyReason reason) {
    if (guids.empty()) {
        return 0;
    }
    std::unordered_set<AssetGuid> targets;
    targets.reserve(guids.size());
    for (const AssetGuid& guid : guids) {
        if (!guid.empty()) {
            targets.insert(guid);
        }
    }
    if (targets.empty()) {
        return 0;
    }
    const size_t before = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const AssetRecord& record) {
        return targets.find(record.guid) != targets.end();
    }), records_.end());
    const size_t removed = before - records_.size();
    if (removed > 0) {
        markDirty(reason);
    }
    return removed;
}

bool AssetRegistry::refreshRecordHealth(const std::filesystem::path& root, bool markDirtyOnChange) {
    std::unordered_set<AssetGuid> guids;
    guids.reserve(records_.size());
    for (const AssetRecord& record : records_) {
        if (!record.guid.empty()) {
            guids.insert(record.guid);
        }
    }

    bool changed = false;
    for (AssetRecord& record : records_) {
        const bool previousMissing = record.missing;
        const bool previousStale = record.stale;
        const bool previousSourceMissing = record.sourceMissing;
        const bool previousImportedMissing = record.importedMetadataMissing;
        const bool previousCookedMissing = record.cookedPayloadMissing;
        const bool previousDependenciesMissing = record.dependenciesMissing;
        const AssetImportStatus previousStatus = record.status;

        const std::filesystem::path sourcePath = resolveRecordPath(root, record.sourcePath);
        const std::filesystem::path importedPath = resolveRecordPath(root, record.importedPath);
        const std::filesystem::path cachePath = resolveRecordPath(root, record.cachePath);
        const bool hasSourcePath = !record.sourcePath.empty();
        const bool hasImportedPath = !record.importedPath.empty();
        const bool hasCachePath = !record.cachePath.empty();
        const bool sourceExists = hasSourcePath && regularFileExists(sourcePath);
        const bool importedExists = !hasImportedPath || regularFileExists(importedPath);
        const bool cacheExists = !hasCachePath || regularFileExists(cachePath);

        bool dependenciesExist = true;
        for (const AssetDependency& dependency : record.dependencies) {
            if (!dependency.guid.empty() && guids.find(dependency.guid) == guids.end()) {
                dependenciesExist = false;
                break;
            }
        }

        record.sourceMissing = hasSourcePath && !sourceExists;
        record.importedMetadataMissing = hasImportedPath && !importedExists;
        record.cookedPayloadMissing = hasCachePath && !cacheExists;
        record.dependenciesMissing = !dependenciesExist;
        record.missing = record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing;

        uint64_t sourceStamp = sourceExists ? writeStamp(sourcePath) : 0;
        uint64_t importedStamp = importedExists && hasImportedPath ? writeStamp(importedPath) : 0;
        uint64_t cacheStamp = cacheExists && hasCachePath ? writeStamp(cachePath) : 0;
        const bool importedStale = sourceStamp != 0 && importedStamp != 0 && sourceStamp > importedStamp;
        const bool cacheStale = sourceStamp != 0 && cacheStamp != 0 && sourceStamp > cacheStamp;
        record.stale = !record.missing && (importedStale || cacheStale);

        if (record.status != AssetImportStatus::Failed) {
            record.status = record.missing ? AssetImportStatus::Missing
                : record.stale ? AssetImportStatus::Stale
                : AssetImportStatus::Imported;
        }

        changed = changed
            || previousMissing != record.missing
            || previousStale != record.stale
            || previousSourceMissing != record.sourceMissing
            || previousImportedMissing != record.importedMetadataMissing
            || previousCookedMissing != record.cookedPayloadMissing
            || previousDependenciesMissing != record.dependenciesMissing
            || previousStatus != record.status;
    }

    if (changed && markDirtyOnChange) {
        markDirty(AssetRegistryDirtyReason::AssetDependencyChanged);
    }
    return changed;
}

} // namespace rtv
