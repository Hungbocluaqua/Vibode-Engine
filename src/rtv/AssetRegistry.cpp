#include "rtv/AssetRegistry.h"

#include "rtv/Project.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace rtv {

namespace {

nlohmann::json importSettingsJson(const AssetImportSettings& settings) {
    return {
        {"preserveHierarchy", settings.preserveHierarchy},
        {"importMaterials", settings.importMaterials},
        {"importTextures", settings.importTextures},
        {"importCameras", settings.importCameras},
        {"importLights", settings.importLights},
        {"generateTangents", settings.generateTangents},
        {"buildBlasCache", settings.buildBlasCache},
        {"unitScale", settings.unitScale},
        {"coordinateConversion", settings.coordinateConversion},
    };
}

AssetImportSettings importSettingsFromJson(const nlohmann::json& json) {
    AssetImportSettings settings;
    if (!json.is_object()) {
        return settings;
    }
    settings.preserveHierarchy = json.value("preserveHierarchy", settings.preserveHierarchy);
    settings.importMaterials = json.value("importMaterials", settings.importMaterials);
    settings.importTextures = json.value("importTextures", settings.importTextures);
    settings.importCameras = json.value("importCameras", settings.importCameras);
    settings.importLights = json.value("importLights", settings.importLights);
    settings.generateTangents = json.value("generateTangents", settings.generateTangents);
    settings.buildBlasCache = json.value("buildBlasCache", settings.buildBlasCache);
    settings.unitScale = json.value("unitScale", settings.unitScale);
    settings.coordinateConversion = json.value("coordinateConversion", settings.coordinateConversion);
    return settings;
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
                record.sourceHash = item.value("sourceHash", std::string{});
                record.importedHash = item.value("importedHash", std::string{});
                record.importSettingsHash = item.value("importSettingsHash", std::string{});
                record.lastModifiedTimestamp = item.value("lastModifiedTimestamp", std::string{});
                record.missing = item.value("missing", false);
                record.stale = item.value("stale", false);
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

    nlohmann::json recordsJson = nlohmann::json::array();
    for (const AssetRecord& record : records_) {
        nlohmann::json dependencies = nlohmann::json::array();
        for (const AssetDependency& dependency : record.dependencies) {
            dependencies.push_back({{"guid", dependency.guid}, {"kind", dependency.kind}});
        }
        recordsJson.push_back({
            {"guid", record.guid},
            {"type", assetTypeName(record.type)},
            {"displayName", record.displayName},
            {"sourcePath", record.sourcePath},
            {"importedPath", record.importedPath},
            {"cachePath", record.cachePath},
            {"thumbnailPath", record.thumbnailPath},
            {"dependencies", dependencies},
            {"references", record.references},
            {"sourceHash", record.sourceHash},
            {"importedHash", record.importedHash},
            {"importSettingsHash", record.importSettingsHash},
            {"lastModifiedTimestamp", record.lastModifiedTimestamp},
            {"importSettings", importSettingsJson(record.importSettings)},
            {"status", assetImportStatusName(record.status)},
            {"missing", record.missing},
            {"stale", record.stale},
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

} // namespace rtv
