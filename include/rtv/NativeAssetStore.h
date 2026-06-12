#pragma once

#include "rtv/NativeBinaryIO.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

enum class NativeAssetStoreSource : uint32_t {
    Missing = 0,
    LooseFile = 1,
    Package = 2,
};

struct NativeAssetStoreObject {
    std::string guid;
    NativeAssetKind kind = NativeAssetKind::Unknown;
    NativeAssetStoreSource source = NativeAssetStoreSource::Missing;
    std::filesystem::path path;
    std::filesystem::path packagePath;
    std::string packageObjectPath;
    uint64_t generation = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t referenceCount = 0;
    bool payloadHashValid = false;
    std::vector<std::string> dependencies;
};

struct NativeAssetStoreMountReport {
    bool ok = false;
    std::filesystem::path path;
    std::string source;
    uint64_t generation = 0;
    uint32_t objectCount = 0;
    std::vector<std::string> warnings;
    std::vector<NativeBinaryError> errors;
};

struct NativeAssetStoreReferenceReport {
    bool ok = false;
    std::string guid;
    uint32_t referenceCount = 0;
    std::string operation;
    std::string message;
};

struct NativeAssetStoreUnmountReport {
    bool ok = false;
    std::filesystem::path path;
    std::string source;
    uint64_t generation = 0;
    uint32_t removedObjectCount = 0;
    uint32_t blockedReferenceCount = 0;
    std::vector<std::string> blockedGuids;
    std::vector<std::string> warnings;
    std::vector<NativeBinaryError> errors;
};

struct NativeAssetStoreInspection {
    bool ok = false;
    std::vector<NativeAssetStoreMountReport> mounts;
    std::vector<NativeAssetStoreReferenceReport> references;
    std::vector<NativeAssetStoreUnmountReport> unmounts;
    std::vector<NativeAssetStoreObject> objects;
    std::vector<NativeAssetStoreObject> queries;
    std::vector<std::string> duplicateObjectGuids;
    std::vector<std::string> missingDependencies;
    std::vector<std::string> warnings;
};

class NativeAssetStore {
public:
    [[nodiscard]] NativeAssetStoreMountReport mountPackage(const std::filesystem::path& path);
    [[nodiscard]] NativeAssetStoreMountReport mountLooseRoot(const std::filesystem::path& root);
    [[nodiscard]] NativeAssetStoreReferenceReport retainObject(std::string_view guid);
    [[nodiscard]] NativeAssetStoreReferenceReport releaseObject(std::string_view guid);
    [[nodiscard]] NativeAssetStoreUnmountReport unmountPackage(const std::filesystem::path& path);
    [[nodiscard]] std::optional<NativeAssetStoreObject> find(std::string_view guid) const;
    [[nodiscard]] std::vector<NativeAssetStoreObject> variants(std::string_view guid) const;
    [[nodiscard]] std::vector<std::byte> readObjectBytes(std::string_view guid, NativeBinaryError* error = nullptr) const;
    [[nodiscard]] std::vector<std::byte> readObjectBytes(const NativeAssetStoreObject& object, NativeBinaryError* error = nullptr) const;
    [[nodiscard]] NativeAssetStoreInspection inspect(const std::vector<std::string>& queryGuids = {}) const;

private:
    std::unordered_map<std::string, NativeAssetStoreObject> objectsByGuid_;
    std::vector<NativeAssetStoreObject> allObjects_;
    std::vector<NativeAssetStoreMountReport> mounts_;
    std::vector<NativeAssetStoreReferenceReport> references_;
    std::vector<NativeAssetStoreUnmountReport> unmounts_;
    std::vector<std::string> warnings_;
    uint64_t nextGeneration_ = 1;
};

[[nodiscard]] const char* nativeAssetStoreSourceName(NativeAssetStoreSource source);
[[nodiscard]] nlohmann::json nativeAssetStoreInspectionToJson(const NativeAssetStoreInspection& inspection);
[[nodiscard]] int inspectNativeAssetStoreCommand(
    const std::vector<std::filesystem::path>& packagePaths,
    const std::vector<std::filesystem::path>& looseRoots,
    const std::vector<std::string>& queryGuids,
    const std::vector<std::string>& retainGuids = {},
    const std::vector<std::string>& releaseGuids = {},
    const std::vector<std::filesystem::path>& unmountPackagePaths = {},
    const std::filesystem::path& jsonOut = {});

} // namespace rtv
