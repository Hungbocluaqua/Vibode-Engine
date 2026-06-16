#pragma once

#include "rtv/AssetRegistry.h"
#include "rtv/NativeBinaryIO.h"

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

enum class StreamingIoBackendKind : uint8_t {
    Win32,
    DirectStorage,
    Mock,
};

struct StreamingRuntimeOptions {
    bool enabled = true;
    std::string budgetPreset = "custom";
    uint64_t budgetBytes = 1024ull * 1024ull * 1024ull;
    uint64_t cpuMemoryBudgetBytes = 512ull * 1024ull * 1024ull;
    uint64_t gpuMemoryBudgetBytes = 1024ull * 1024ull * 1024ull;
    uint64_t uploadBytesPerFrame = 64ull * 1024ull * 1024ull;
    uint64_t cpuBatchBytes = 256ull * 1024ull * 1024ull;
    bool directStorageEnabled = false;
    bool forceCpuDecompress = false;
    bool evictionEnabled = true;
    StreamingIoBackendKind ioBackend = StreamingIoBackendKind::Win32;
};

enum class StreamingAssetState : uint8_t {
    Unknown,
    Cataloged,
    Requested,
    IoQueued,
    IoComplete,
    Decoding,
    CpuReadyTransient,
    UploadQueued,
    Uploading,
    GpuResident,
    Failed,
    EvictQueued,
    Retiring,
    Evicted,
};

struct NativeAssetCatalogEntry {
    AssetGuid guid;
    AssetType assetType = AssetType::Unknown;
    NativeAssetKind nativeKind = NativeAssetKind::Unknown;
    std::string displayName;
    std::filesystem::path cachePath;
    uint64_t fileBytes = 0;
    uint64_t estimatedCpuBytes = 0;
    uint64_t estimatedGpuBytes = 0;
    std::vector<AssetDependency> dependencies;
    bool cacheFileExists = false;
    bool streamable = false;
};

class NativeAssetCatalog {
public:
    void clear();
    void buildFromRegistry(const AssetRegistry& registry, const std::filesystem::path& root);

    [[nodiscard]] const std::unordered_map<AssetGuid, NativeAssetCatalogEntry>& entries() const { return entries_; }
    [[nodiscard]] const NativeAssetCatalogEntry* find(const AssetGuid& guid) const;
    [[nodiscard]] std::vector<AssetGuid> dependencyClosure(const AssetGuid& rootGuid) const;
    [[nodiscard]] uint64_t closureFileBytes(const AssetGuid& rootGuid) const;
    [[nodiscard]] nlohmann::json toJson() const;

private:
    std::unordered_map<AssetGuid, NativeAssetCatalogEntry> entries_;
};

struct StreamingQueueSnapshot {
    uint32_t requested = 0;
    uint32_t ioQueued = 0;
    uint32_t decoding = 0;
    uint32_t cpuReadyTransient = 0;
    uint32_t uploadQueued = 0;
    uint32_t uploading = 0;
    uint32_t gpuResident = 0;
    uint32_t failed = 0;
};

struct StreamingRootSnapshot {
    uint64_t serial = 0;
    AssetGuid rootGuid;
    std::string label;
    std::filesystem::path rootPath;
    uint32_t totalFiles = 0;
    uint32_t loadedFiles = 0;
    uint32_t failedFiles = 0;
    uint64_t queuedBytes = 0;
    uint64_t loadedBytes = 0;
    uint64_t appliedBytes = 0;
    uint32_t reboundRenderers = 0;
    bool active = false;
    bool complete = false;
    bool cancelled = false;
    bool failed = false;
    std::string status;
};

class StreamingRuntimeState {
public:
    void setOptions(StreamingRuntimeOptions options);
    [[nodiscard]] const StreamingRuntimeOptions& options() const { return options_; }

    void catalogAsset(const AssetGuid& guid);
    void setAssetState(const AssetGuid& guid, StreamingAssetState state, std::string reason = {});
    [[nodiscard]] StreamingAssetState assetState(const AssetGuid& guid) const;
    [[nodiscard]] StreamingQueueSnapshot queueSnapshot() const;

    void setActiveRoot(StreamingRootSnapshot snapshot);
    void clearActiveRoot();
    [[nodiscard]] const StreamingRootSnapshot* activeRoot() const;
    void pushEvent(std::string event);

    [[nodiscard]] nlohmann::json toJson(const NativeAssetCatalog* catalog = nullptr) const;

private:
    StreamingRuntimeOptions options_{};
    std::unordered_map<AssetGuid, StreamingAssetState> assetStates_;
    std::unordered_map<AssetGuid, std::string> failureReasons_;
    std::vector<std::string> events_;
    StreamingRootSnapshot activeRoot_{};
    bool hasActiveRoot_ = false;
};

[[nodiscard]] const char* streamingIoBackendKindName(StreamingIoBackendKind kind);
[[nodiscard]] const char* streamingAssetStateName(StreamingAssetState state);
[[nodiscard]] StreamingIoBackendKind parseStreamingIoBackendKind(std::string value);
[[nodiscard]] bool parseStreamingOnOff(std::string_view value);
[[nodiscard]] bool applyStreamingBudgetPreset(std::string_view preset, StreamingRuntimeOptions& options);
[[nodiscard]] nlohmann::json streamingRuntimeOptionsToJson(const StreamingRuntimeOptions& options);
[[nodiscard]] nlohmann::json streamingRootSnapshotToJson(const StreamingRootSnapshot& snapshot);
[[nodiscard]] int validateNativeAssetCatalogCommand(
    const std::filesystem::path& registryPath,
    const std::filesystem::path& root,
    const std::filesystem::path& jsonOut = {});

} // namespace rtv
