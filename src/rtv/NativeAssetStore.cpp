#include "rtv/NativeAssetStore.h"

#include "rtv/RtpkgIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <system_error>

namespace rtv {
namespace {

bool readFileRange(const std::filesystem::path& path, uint64_t offset, uint64_t size, std::vector<std::byte>& out, NativeBinaryError* error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (error) {
            error->code = NativeBinaryErrorCode::IoError;
            error->path = path;
            error->table = "file";
            error->message = "Could not open native asset store payload";
        }
        return false;
    }
    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0 || offset > static_cast<uint64_t>(fileSize) || size > static_cast<uint64_t>(fileSize) - offset) {
        if (error) {
            error->code = NativeBinaryErrorCode::CorruptTable;
            error->path = path;
            error->table = "payload";
            error->offset = offset;
            error->expectedSize = size;
            error->message = "Native asset store payload range is outside the file";
        }
        return false;
    }
    out.resize(static_cast<size_t>(size));
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!out.empty()) {
        file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
    return file.good() || out.empty();
}

bool isStandaloneNativeAssetPath(const std::filesystem::path& path) {
    const NativeAssetKind kind = nativeAssetKindFromExtension(path);
    return kind != NativeAssetKind::Unknown && kind != NativeAssetKind::Package;
}

std::string dependencyKey(const std::string& owner, const std::string& dependency) {
    return owner + " -> " + dependency;
}

std::filesystem::path storePathKey(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return (ec ? path : canonical).lexically_normal();
}

std::filesystem::path storeObjectRuntimePath(const NativeAssetStoreObject& object) {
    if (object.source == NativeAssetStoreSource::Package) {
        return object.packageObjectPath.empty() ? object.packagePath : object.packagePath / object.packageObjectPath;
    }
    return object.path;
}

} // namespace

const char* nativeAssetStoreSourceName(NativeAssetStoreSource source) {
    switch (source) {
    case NativeAssetStoreSource::LooseFile: return "loose_file";
    case NativeAssetStoreSource::Package: return "package";
    case NativeAssetStoreSource::Missing: break;
    }
    return "missing";
}

NativeAssetStoreMountReport NativeAssetStore::mountPackage(const std::filesystem::path& path) {
    NativeAssetStoreMountReport report;
    report.path = path;
    report.source = nativeAssetStoreSourceName(NativeAssetStoreSource::Package);
    report.generation = nextGeneration_++;
    RtpkgReader reader;
    const RtpkgInspection inspection = reader.inspect(path, true);
    if (!inspection.native.ok) {
        report.errors = inspection.native.errors;
        mounts_.push_back(report);
        return report;
    }

    for (size_t i = 0; i < inspection.embeddedAssets.size(); ++i) {
        const RtpkgEmbeddedAssetInfo& embedded = inspection.embeddedAssets[i];
        NativeAssetStoreObject object;
        object.guid = embedded.guid;
        object.kind = embedded.kind;
        object.source = NativeAssetStoreSource::Package;
        object.packagePath = path;
        object.packageObjectPath = embedded.packagePath;
        object.generation = report.generation;
        object.offset = embedded.packageOffset;
        object.size = embedded.packageSize;
        object.payloadHashValid = embedded.payloadHashValid;
        if (i < inspection.native.objects.size()) {
            const NativeObjectRecord& nativeObject = inspection.native.objects[i];
            for (uint32_t d = 0; d < nativeObject.dependencyCount; ++d) {
                const uint32_t dependencyIndex = nativeObject.firstDependency + d;
                if (dependencyIndex < inspection.native.dependencies.size()) {
                    object.dependencies.push_back(nativeGuidToText(inspection.native.dependencies[dependencyIndex].dependencyGuid));
                }
            }
        }
        if (objectsByGuid_.find(object.guid) == objectsByGuid_.end()) {
            objectsByGuid_[object.guid] = object;
        } else {
            warnings_.push_back("Mounted native package contains duplicate object GUID variant: " + object.guid);
        }
        allObjects_.push_back(object);
        ++report.objectCount;
    }
    report.ok = true;
    mounts_.push_back(report);
    return report;
}

NativeAssetStoreMountReport NativeAssetStore::mountLooseRoot(const std::filesystem::path& root) {
    NativeAssetStoreMountReport report;
    report.path = root;
    report.source = nativeAssetStoreSourceName(NativeAssetStoreSource::LooseFile);
    report.generation = nextGeneration_++;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        report.errors.push_back(NativeBinaryError{.code = NativeBinaryErrorCode::IoError, .path = root, .table = "root", .message = "Loose native asset root is not a directory"});
        mounts_.push_back(report);
        return report;
    }

    NativeAssetReader reader;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            report.warnings.push_back("Stopped loose native asset scan: " + ec.message());
            break;
        }
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || !isStandaloneNativeAssetPath(entry.path())) {
            continue;
        }
        const NativeAssetInspection inspection = reader.inspect(entry.path(), true);
        if (!inspection.ok) {
            report.errors.insert(report.errors.end(), inspection.errors.begin(), inspection.errors.end());
            continue;
        }
        NativeAssetStoreObject object;
        object.guid = nativeGuidToText(inspection.header.assetGuid);
        object.kind = static_cast<NativeAssetKind>(inspection.header.assetKind);
        object.source = NativeAssetStoreSource::LooseFile;
        object.path = entry.path();
        object.generation = report.generation;
        object.size = inspection.header.fileSize;
        object.payloadHashValid = inspection.payloadHashValid;
        for (const NativeDependencyRecord& dependency : inspection.dependencies) {
            object.dependencies.push_back(nativeGuidToText(dependency.dependencyGuid));
        }
        if (objectsByGuid_.find(object.guid) == objectsByGuid_.end()) {
            objectsByGuid_[object.guid] = object;
        } else {
            warnings_.push_back("Mounted loose native root contains duplicate object GUID variant: " + object.guid);
        }
        allObjects_.push_back(object);
        ++report.objectCount;
    }
    report.ok = report.errors.empty();
    mounts_.push_back(report);
    return report;
}

NativeAssetStoreReferenceReport NativeAssetStore::retainObject(std::string_view guid) {
    NativeAssetStoreReferenceReport report;
    report.guid = std::string(guid);
    report.operation = "retain";
    const auto it = objectsByGuid_.find(report.guid);
    if (it == objectsByGuid_.end()) {
        report.message = "Native asset store object GUID was not mounted";
        references_.push_back(report);
        return report;
    }
    ++it->second.referenceCount;
    report.referenceCount = it->second.referenceCount;
    report.ok = true;
    report.message = "Native asset store object retained";
    references_.push_back(report);
    return report;
}

NativeAssetStoreReferenceReport NativeAssetStore::releaseObject(std::string_view guid) {
    NativeAssetStoreReferenceReport report;
    report.guid = std::string(guid);
    report.operation = "release";
    const auto it = objectsByGuid_.find(report.guid);
    if (it == objectsByGuid_.end()) {
        report.message = "Native asset store object GUID was not mounted";
        references_.push_back(report);
        return report;
    }
    if (it->second.referenceCount == 0) {
        report.message = "Native asset store object has no active references";
        references_.push_back(report);
        return report;
    }
    --it->second.referenceCount;
    report.referenceCount = it->second.referenceCount;
    report.ok = true;
    report.message = "Native asset store object released";
    references_.push_back(report);
    return report;
}

NativeAssetStoreUnmountReport NativeAssetStore::unmountPackage(const std::filesystem::path& path) {
    NativeAssetStoreUnmountReport report;
    report.path = path;
    report.source = nativeAssetStoreSourceName(NativeAssetStoreSource::Package);
    const std::filesystem::path target = storePathKey(path);
    std::vector<std::string> removableGuids;
    for (const NativeAssetStoreObject& object : allObjects_) {
        if (object.source != NativeAssetStoreSource::Package || storePathKey(object.packagePath) != target) {
            continue;
        }
        if (report.generation == 0) {
            report.generation = object.generation;
        }
        uint32_t referenceCount = object.referenceCount;
        const auto canonicalIt = objectsByGuid_.find(object.guid);
        if (canonicalIt != objectsByGuid_.end() && canonicalIt->second.source == NativeAssetStoreSource::Package && storePathKey(canonicalIt->second.packagePath) == target) {
            referenceCount = canonicalIt->second.referenceCount;
        }
        if (referenceCount > 0) {
            report.blockedGuids.push_back(object.guid);
            report.blockedReferenceCount += referenceCount;
            continue;
        }
        removableGuids.push_back(object.guid);
    }
    if (removableGuids.empty() && report.blockedGuids.empty()) {
        report.warnings.push_back("No mounted package objects matched the requested package path");
        unmounts_.push_back(report);
        return report;
    }
    if (!report.blockedGuids.empty()) {
        std::sort(report.blockedGuids.begin(), report.blockedGuids.end());
        report.warnings.push_back("Package unload blocked by active native asset store references");
        unmounts_.push_back(report);
        return report;
    }
    for (const std::string& guid : removableGuids) {
        const auto it = objectsByGuid_.find(guid);
        if (it != objectsByGuid_.end() && it->second.source == NativeAssetStoreSource::Package && storePathKey(it->second.packagePath) == target) {
            objectsByGuid_.erase(it);
        }
    }
    const auto beforeCount = allObjects_.size();
    allObjects_.erase(std::remove_if(allObjects_.begin(), allObjects_.end(), [&](const NativeAssetStoreObject& object) {
        return object.source == NativeAssetStoreSource::Package && storePathKey(object.packagePath) == target;
    }), allObjects_.end());
    report.ok = true;
    report.removedObjectCount = static_cast<uint32_t>(beforeCount - allObjects_.size());
    unmounts_.push_back(report);
    return report;
}

std::optional<NativeAssetStoreObject> NativeAssetStore::find(std::string_view guid) const {
    const auto it = objectsByGuid_.find(std::string(guid));
    if (it == objectsByGuid_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<NativeAssetStoreObject> NativeAssetStore::variants(std::string_view guid) const {
    std::vector<NativeAssetStoreObject> result;
    const std::string key(guid);
    for (const NativeAssetStoreObject& object : allObjects_) {
        if (object.guid == key) {
            result.push_back(object);
        }
    }
    std::sort(result.begin(), result.end(), [](const NativeAssetStoreObject& a, const NativeAssetStoreObject& b) {
        return storePathKey(storeObjectRuntimePath(a)).generic_string() < storePathKey(storeObjectRuntimePath(b)).generic_string();
    });
    return result;
}

std::vector<std::byte> NativeAssetStore::readObjectBytes(std::string_view guid, NativeBinaryError* error) const {
    const auto object = find(guid);
    if (!object.has_value()) {
        if (error) {
            error->code = NativeBinaryErrorCode::MissingDependency;
            error->table = "store";
            error->message = "Native asset store object GUID was not mounted";
        }
        return {};
    }
    return readObjectBytes(*object, error);
}

std::vector<std::byte> NativeAssetStore::readObjectBytes(const NativeAssetStoreObject& object, NativeBinaryError* error) const {
    std::vector<std::byte> bytes;
    if (object.source == NativeAssetStoreSource::Package) {
        readFileRange(object.packagePath, object.offset, object.size, bytes, error);
    } else if (object.source == NativeAssetStoreSource::LooseFile) {
        readFileRange(object.path, 0, object.size, bytes, error);
    } else if (error) {
        error->code = NativeBinaryErrorCode::MissingDependency;
        error->table = "store";
        error->message = "Native asset store object source is missing";
    }
    return bytes;
}

NativeAssetStoreInspection NativeAssetStore::inspect(const std::vector<std::string>& queryGuids) const {
    NativeAssetStoreInspection inspection;
    inspection.mounts = mounts_;
    inspection.references = references_;
    inspection.unmounts = unmounts_;
    inspection.warnings = warnings_;
    inspection.objects = allObjects_;
    std::sort(inspection.objects.begin(), inspection.objects.end(), [](const NativeAssetStoreObject& a, const NativeAssetStoreObject& b) {
        if (a.guid != b.guid) {
            return a.guid < b.guid;
        }
        return a.packageObjectPath < b.packageObjectPath;
    });
    for (size_t i = 1; i < inspection.objects.size(); ++i) {
        if (inspection.objects[i].guid == inspection.objects[i - 1].guid &&
            (inspection.duplicateObjectGuids.empty() || inspection.duplicateObjectGuids.back() != inspection.objects[i].guid)) {
            inspection.duplicateObjectGuids.push_back(inspection.objects[i].guid);
        }
    }
    std::set<std::string> missing;
    for (const NativeAssetStoreObject& object : inspection.objects) {
        for (const std::string& dependency : object.dependencies) {
            if (objectsByGuid_.find(dependency) == objectsByGuid_.end()) {
                missing.insert(dependencyKey(object.guid, dependency));
            }
        }
    }
    inspection.missingDependencies.assign(missing.begin(), missing.end());
    for (const std::string& guid : queryGuids) {
        if (const auto found = find(guid)) {
            inspection.queries.push_back(*found);
        } else {
            NativeAssetStoreObject missingObject;
            missingObject.guid = guid;
            inspection.queries.push_back(missingObject);
        }
    }
    inspection.ok = std::all_of(mounts_.begin(), mounts_.end(), [](const NativeAssetStoreMountReport& mount) { return mount.ok; }) && inspection.missingDependencies.empty();
    return inspection;
}

nlohmann::json nativeAssetStoreInspectionToJson(const NativeAssetStoreInspection& inspection) {
    auto objectJson = [](const NativeAssetStoreObject& object) {
        return nlohmann::json{
            {"guid", object.guid},
            {"kind", nativeAssetKindName(object.kind)},
            {"source", nativeAssetStoreSourceName(object.source)},
            {"path", object.path.empty() ? std::string{} : object.path.generic_string()},
            {"package_path", object.packagePath.empty() ? std::string{} : object.packagePath.generic_string()},
            {"package_object_path", object.packageObjectPath},
            {"generation", object.generation},
            {"offset", object.offset},
            {"size", object.size},
            {"reference_count", object.referenceCount},
            {"payload_hash_valid", object.payloadHashValid},
            {"dependencies", object.dependencies},
        };
    };
    nlohmann::json mounts = nlohmann::json::array();
    for (const NativeAssetStoreMountReport& mount : inspection.mounts) {
        nlohmann::json errors = nlohmann::json::array();
        for (const NativeBinaryError& error : mount.errors) {
            errors.push_back({{"code", nativeBinaryErrorCodeName(error.code)}, {"path", error.path.generic_string()}, {"table", error.table}, {"message", error.message}});
        }
        mounts.push_back({{"ok", mount.ok}, {"path", mount.path.generic_string()}, {"source", mount.source}, {"generation", mount.generation}, {"object_count", mount.objectCount}, {"warnings", mount.warnings}, {"errors", errors}});
    }
    nlohmann::json references = nlohmann::json::array();
    for (const NativeAssetStoreReferenceReport& reference : inspection.references) {
        references.push_back({
            {"ok", reference.ok},
            {"guid", reference.guid},
            {"operation", reference.operation},
            {"reference_count", reference.referenceCount},
            {"message", reference.message},
        });
    }
    nlohmann::json unmounts = nlohmann::json::array();
    for (const NativeAssetStoreUnmountReport& unmount : inspection.unmounts) {
        nlohmann::json errors = nlohmann::json::array();
        for (const NativeBinaryError& error : unmount.errors) {
            errors.push_back({{"code", nativeBinaryErrorCodeName(error.code)}, {"path", error.path.generic_string()}, {"table", error.table}, {"message", error.message}});
        }
        unmounts.push_back({
            {"ok", unmount.ok},
            {"path", unmount.path.generic_string()},
            {"source", unmount.source},
            {"generation", unmount.generation},
            {"removed_object_count", unmount.removedObjectCount},
            {"blocked_reference_count", unmount.blockedReferenceCount},
            {"blocked_guids", unmount.blockedGuids},
            {"warnings", unmount.warnings},
            {"errors", errors},
        });
    }
    nlohmann::json objects = nlohmann::json::array();
    for (const NativeAssetStoreObject& object : inspection.objects) objects.push_back(objectJson(object));
    nlohmann::json queries = nlohmann::json::array();
    for (const NativeAssetStoreObject& object : inspection.queries) queries.push_back(objectJson(object));
    return {
        {"ok", inspection.ok},
        {"mounts", mounts},
        {"references", references},
        {"unmounts", unmounts},
        {"object_count", inspection.objects.size()},
        {"duplicate_object_guid_count", inspection.duplicateObjectGuids.size()},
        {"duplicate_object_guids", inspection.duplicateObjectGuids},
        {"missing_dependency_count", inspection.missingDependencies.size()},
        {"missing_dependencies", inspection.missingDependencies},
        {"objects", objects},
        {"queries", queries},
        {"warnings", inspection.warnings},
    };
}

int inspectNativeAssetStoreCommand(
    const std::vector<std::filesystem::path>& packagePaths,
    const std::vector<std::filesystem::path>& looseRoots,
    const std::vector<std::string>& queryGuids,
    const std::vector<std::string>& retainGuids,
    const std::vector<std::string>& releaseGuids,
    const std::vector<std::filesystem::path>& unmountPackagePaths,
    const std::filesystem::path& jsonOut) {
    NativeAssetStore store;
    for (const std::filesystem::path& packagePath : packagePaths) {
        (void)store.mountPackage(packagePath);
    }
    for (const std::filesystem::path& root : looseRoots) {
        (void)store.mountLooseRoot(root);
    }
    for (const std::string& guid : retainGuids) {
        (void)store.retainObject(guid);
    }
    for (const std::string& guid : releaseGuids) {
        (void)store.releaseObject(guid);
    }
    for (const std::filesystem::path& packagePath : unmountPackagePaths) {
        (void)store.unmountPackage(packagePath);
    }
    NativeAssetStoreInspection inspection = store.inspect(queryGuids);
    nlohmann::json report = nativeAssetStoreInspectionToJson(inspection);
    nlohmann::json byteReads = nlohmann::json::array();
    for (const std::string& guid : queryGuids) {
        NativeBinaryError error;
        const std::vector<std::byte> bytes = store.readObjectBytes(guid, &error);
        byteReads.push_back({{"guid", guid}, {"ok", !bytes.empty()}, {"size", bytes.size()}, {"error", error.message}});
    }
    report["query_byte_reads"] = byteReads;
    if (!jsonOut.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(jsonOut.parent_path(), ec);
        std::ofstream out(jsonOut);
        if (!out.is_open()) {
            std::cerr << "Failed to write native asset store inspection JSON: " << jsonOut << '\n';
            return 1;
        }
        out << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return inspection.ok ? 0 : 1;
}

} // namespace rtv
