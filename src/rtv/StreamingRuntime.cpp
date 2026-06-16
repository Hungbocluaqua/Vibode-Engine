#include "rtv/StreamingRuntime.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <system_error>
#include <unordered_set>

namespace rtv {
namespace {

std::string lowerAsciiLocal(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::filesystem::path resolveRecordPath(const std::filesystem::path& root, const std::string& value) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path(value);
    return path.is_absolute() ? path : root / path;
}

bool nativeRuntimeAssetType(AssetType type) {
    return type == AssetType::Texture ||
        type == AssetType::Material ||
        type == AssetType::Mesh ||
        type == AssetType::Skeleton ||
        type == AssetType::Animation ||
        type == AssetType::AnimationController ||
        type == AssetType::SkeletalMesh;
}

uint64_t regularFileBytes(const std::filesystem::path& path, bool* exists = nullptr) {
    std::error_code ec;
    const bool isFile = !path.empty() && std::filesystem::is_regular_file(path, ec);
    if (exists != nullptr) {
        *exists = isFile && !ec;
    }
    if (!isFile || ec) {
        return 0;
    }
    const uintmax_t bytes = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(bytes);
}

nlohmann::json dependencyJson(const std::vector<AssetDependency>& dependencies) {
    nlohmann::json out = nlohmann::json::array();
    for (const AssetDependency& dependency : dependencies) {
        out.push_back({
            {"guid", dependency.guid},
            {"kind", dependency.kind},
        });
    }
    return out;
}

} // namespace

const char* streamingIoBackendKindName(StreamingIoBackendKind kind) {
    switch (kind) {
    case StreamingIoBackendKind::Win32: return "win32";
    case StreamingIoBackendKind::DirectStorage: return "directstorage";
    case StreamingIoBackendKind::Mock: return "mock";
    }
    return "win32";
}

const char* streamingAssetStateName(StreamingAssetState state) {
    switch (state) {
    case StreamingAssetState::Unknown: return "unknown";
    case StreamingAssetState::Cataloged: return "cataloged";
    case StreamingAssetState::Requested: return "requested";
    case StreamingAssetState::IoQueued: return "io_queued";
    case StreamingAssetState::IoComplete: return "io_complete";
    case StreamingAssetState::Decoding: return "decoding";
    case StreamingAssetState::CpuReadyTransient: return "cpu_ready_transient";
    case StreamingAssetState::UploadQueued: return "upload_queued";
    case StreamingAssetState::Uploading: return "uploading";
    case StreamingAssetState::GpuResident: return "gpu_resident";
    case StreamingAssetState::Failed: return "failed";
    case StreamingAssetState::EvictQueued: return "evict_queued";
    case StreamingAssetState::Retiring: return "retiring";
    case StreamingAssetState::Evicted: return "evicted";
    }
    return "unknown";
}

StreamingIoBackendKind parseStreamingIoBackendKind(std::string value) {
    value = lowerAsciiLocal(std::move(value));
    if (value == "directstorage" || value == "dstorage") {
        return StreamingIoBackendKind::DirectStorage;
    }
    if (value == "mock" || value == "test") {
        return StreamingIoBackendKind::Mock;
    }
    return StreamingIoBackendKind::Win32;
}

bool parseStreamingOnOff(std::string_view value) {
    std::string lower(value);
    lower = lowerAsciiLocal(std::move(lower));
    return lower == "1" || lower == "true" || lower == "on" || lower == "yes" || lower == "enabled";
}

bool applyStreamingBudgetPreset(std::string_view preset, StreamingRuntimeOptions& options) {
    std::string name(preset);
    name = lowerAsciiLocal(std::move(name));
    std::replace(name.begin(), name.end(), '_', '-');
    constexpr uint64_t kMiB = 1024ull * 1024ull;
    if (name == "editor" || name == "editor-interactive" || name == "streaming-editor-interactive") {
        options.budgetPreset = "editor-interactive";
        options.cpuMemoryBudgetBytes = 512ull * kMiB;
        options.gpuMemoryBudgetBytes = 1024ull * kMiB;
        options.uploadBytesPerFrame = 64ull * kMiB;
    } else if (name == "runtime" || name == "runtime-16ms" || name == "mid" || name == "mid-pc" || name == "streaming-runtime-16ms") {
        options.budgetPreset = "runtime-16ms";
        options.cpuMemoryBudgetBytes = 384ull * kMiB;
        options.gpuMemoryBudgetBytes = 768ull * kMiB;
        options.uploadBytesPerFrame = 32ull * kMiB;
    } else if (name == "memory" || name == "memory-safe" || name == "memory-safe-8gb" || name == "streaming-memory-safe-8gb") {
        options.budgetPreset = "memory-safe-8gb";
        options.cpuMemoryBudgetBytes = 256ull * kMiB;
        options.gpuMemoryBudgetBytes = 512ull * kMiB;
        options.uploadBytesPerFrame = 16ull * kMiB;
    } else if (name == "high" || name == "high-end" || name == "high-end-pc" || name == "streaming-high-end-pc") {
        options.budgetPreset = "high-end-pc";
        options.cpuMemoryBudgetBytes = 1024ull * kMiB;
        options.gpuMemoryBudgetBytes = 4096ull * kMiB;
        options.uploadBytesPerFrame = 128ull * kMiB;
    } else if (name == "custom" || name == "none" || name == "off") {
        options.budgetPreset = "custom";
        return true;
    } else {
        return false;
    }
    options.budgetBytes = options.gpuMemoryBudgetBytes;
    options.cpuBatchBytes = std::max<uint64_t>(16ull * kMiB, options.cpuMemoryBudgetBytes / 2ull);
    return true;
}

nlohmann::json streamingRuntimeOptionsToJson(const StreamingRuntimeOptions& options) {
    return {
        {"enabled", options.enabled},
        {"budget_preset", options.budgetPreset},
        {"budget_bytes", options.budgetBytes},
        {"cpu_memory_budget_bytes", options.cpuMemoryBudgetBytes},
        {"gpu_memory_budget_bytes", options.gpuMemoryBudgetBytes},
        {"upload_bytes_per_frame", options.uploadBytesPerFrame},
        {"cpu_batch_bytes", options.cpuBatchBytes},
        {"io_backend", streamingIoBackendKindName(options.ioBackend)},
        {"directstorage_enabled", options.directStorageEnabled},
        {"force_cpu_decompress", options.forceCpuDecompress},
        {"eviction_enabled", options.evictionEnabled},
    };
}

nlohmann::json streamingRootSnapshotToJson(const StreamingRootSnapshot& snapshot) {
    return {
        {"serial", snapshot.serial},
        {"root_guid", snapshot.rootGuid},
        {"label", snapshot.label},
        {"root_path", snapshot.rootPath.generic_string()},
        {"total_files", snapshot.totalFiles},
        {"loaded_files", snapshot.loadedFiles},
        {"failed_files", snapshot.failedFiles},
        {"queued_bytes", snapshot.queuedBytes},
        {"loaded_bytes", snapshot.loadedBytes},
        {"applied_bytes", snapshot.appliedBytes},
        {"rebound_renderers", snapshot.reboundRenderers},
        {"active", snapshot.active},
        {"complete", snapshot.complete},
        {"cancelled", snapshot.cancelled},
        {"failed", snapshot.failed},
        {"status", snapshot.status},
    };
}

void NativeAssetCatalog::clear() {
    entries_.clear();
}

void NativeAssetCatalog::buildFromRegistry(const AssetRegistry& registry, const std::filesystem::path& root) {
    entries_.clear();
    entries_.reserve(registry.records().size());
    for (const AssetRecord& record : registry.records()) {
        if (!nativeRuntimeAssetType(record.type) || record.guid.empty()) {
            continue;
        }
        NativeAssetCatalogEntry entry;
        entry.guid = record.guid;
        entry.assetType = record.type;
        entry.displayName = record.displayName;
        entry.cachePath = resolveRecordPath(root, record.cachePath).lexically_normal();
        entry.nativeKind = nativeAssetKindFromExtension(entry.cachePath);
        entry.dependencies = record.dependencies;
        entry.fileBytes = regularFileBytes(entry.cachePath, &entry.cacheFileExists);
        entry.estimatedCpuBytes = entry.fileBytes;
        entry.estimatedGpuBytes = entry.fileBytes;
        entry.streamable = entry.cacheFileExists && entry.nativeKind != NativeAssetKind::Unknown && entry.nativeKind != NativeAssetKind::Package;
        entries_[entry.guid] = std::move(entry);
    }
}

const NativeAssetCatalogEntry* NativeAssetCatalog::find(const AssetGuid& guid) const {
    const auto it = entries_.find(guid);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<AssetGuid> NativeAssetCatalog::dependencyClosure(const AssetGuid& rootGuid) const {
    std::vector<AssetGuid> out;
    std::vector<AssetGuid> pending{rootGuid};
    std::unordered_map<AssetGuid, bool> visited;
    while (!pending.empty()) {
        AssetGuid guid = std::move(pending.back());
        pending.pop_back();
        if (guid.empty() || visited[guid]) {
            continue;
        }
        visited[guid] = true;
        const auto it = entries_.find(guid);
        if (it == entries_.end()) {
            continue;
        }
        out.push_back(guid);
        for (const AssetDependency& dependency : it->second.dependencies) {
            if (!dependency.guid.empty()) {
                pending.push_back(dependency.guid);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

uint64_t NativeAssetCatalog::closureFileBytes(const AssetGuid& rootGuid) const {
    uint64_t total = 0;
    for (const AssetGuid& guid : dependencyClosure(rootGuid)) {
        if (const NativeAssetCatalogEntry* entry = find(guid)) {
            total += entry->fileBytes;
        }
    }
    return total;
}

nlohmann::json NativeAssetCatalog::toJson() const {
    nlohmann::json assets = nlohmann::json::array();
    for (const auto& [guid, entry] : entries_) {
        assets.push_back({
            {"guid", guid},
            {"asset_type", assetTypeName(entry.assetType)},
            {"native_kind", nativeAssetKindName(entry.nativeKind)},
            {"display_name", entry.displayName},
            {"cache_path", entry.cachePath.generic_string()},
            {"file_bytes", entry.fileBytes},
            {"estimated_cpu_bytes", entry.estimatedCpuBytes},
            {"estimated_gpu_bytes", entry.estimatedGpuBytes},
            {"cache_file_exists", entry.cacheFileExists},
            {"streamable", entry.streamable},
            {"dependencies", dependencyJson(entry.dependencies)},
        });
    }
    return {
        {"schema", "NativeAssetCatalogV1"},
        {"asset_count", entries_.size()},
        {"assets", assets},
    };
}

void StreamingRuntimeState::setOptions(StreamingRuntimeOptions options) {
    options_ = options;
}

void StreamingRuntimeState::catalogAsset(const AssetGuid& guid) {
    if (!guid.empty() && assetStates_.find(guid) == assetStates_.end()) {
        assetStates_[guid] = StreamingAssetState::Cataloged;
    }
}

void StreamingRuntimeState::setAssetState(const AssetGuid& guid, StreamingAssetState state, std::string reason) {
    if (guid.empty()) {
        return;
    }
    assetStates_[guid] = state;
    if (state == StreamingAssetState::Failed && !reason.empty()) {
        failureReasons_[guid] = std::move(reason);
    }
}

StreamingAssetState StreamingRuntimeState::assetState(const AssetGuid& guid) const {
    const auto it = assetStates_.find(guid);
    return it == assetStates_.end() ? StreamingAssetState::Unknown : it->second;
}

StreamingQueueSnapshot StreamingRuntimeState::queueSnapshot() const {
    StreamingQueueSnapshot snapshot;
    for (const auto& [guid, state] : assetStates_) {
        (void)guid;
        switch (state) {
        case StreamingAssetState::Requested: ++snapshot.requested; break;
        case StreamingAssetState::IoQueued: ++snapshot.ioQueued; break;
        case StreamingAssetState::Decoding: ++snapshot.decoding; break;
        case StreamingAssetState::CpuReadyTransient: ++snapshot.cpuReadyTransient; break;
        case StreamingAssetState::UploadQueued: ++snapshot.uploadQueued; break;
        case StreamingAssetState::Uploading: ++snapshot.uploading; break;
        case StreamingAssetState::GpuResident: ++snapshot.gpuResident; break;
        case StreamingAssetState::Failed: ++snapshot.failed; break;
        default: break;
        }
    }
    return snapshot;
}

void StreamingRuntimeState::setActiveRoot(StreamingRootSnapshot snapshot) {
    activeRoot_ = std::move(snapshot);
    hasActiveRoot_ = true;
}

void StreamingRuntimeState::clearActiveRoot() {
    hasActiveRoot_ = false;
    activeRoot_ = {};
}

const StreamingRootSnapshot* StreamingRuntimeState::activeRoot() const {
    return hasActiveRoot_ ? &activeRoot_ : nullptr;
}

void StreamingRuntimeState::pushEvent(std::string event) {
    if (event.empty()) {
        return;
    }
    events_.push_back(std::move(event));
    constexpr size_t kMaxEvents = 256;
    if (events_.size() > kMaxEvents) {
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(events_.size() - kMaxEvents));
    }
}

nlohmann::json StreamingRuntimeState::toJson(const NativeAssetCatalog* catalog) const {
    nlohmann::json assetStates = nlohmann::json::array();
    for (const auto& [guid, state] : assetStates_) {
        nlohmann::json item = {
            {"guid", guid},
            {"state", streamingAssetStateName(state)},
        };
        if (const auto reasonIt = failureReasons_.find(guid); reasonIt != failureReasons_.end()) {
            item["failure_reason"] = reasonIt->second;
        }
        assetStates.push_back(std::move(item));
    }
    const StreamingQueueSnapshot queue = queueSnapshot();
    nlohmann::json out = {
        {"schema", "StreamingRuntimeReportV1"},
        {"options", streamingRuntimeOptionsToJson(options_)},
        {"asset_states", assetStates},
        {"queue_snapshot", {
            {"requested", queue.requested},
            {"io_queued", queue.ioQueued},
            {"decoding", queue.decoding},
            {"cpu_ready_transient", queue.cpuReadyTransient},
            {"upload_queued", queue.uploadQueued},
            {"uploading", queue.uploading},
            {"gpu_resident", queue.gpuResident},
            {"failed", queue.failed},
        }},
        {"events", events_},
    };
    if (hasActiveRoot_) {
        out["active_root"] = streamingRootSnapshotToJson(activeRoot_);
    }
    if (catalog != nullptr) {
        out["catalog"] = catalog->toJson();
    }
    return out;
}

int validateNativeAssetCatalogCommand(
    const std::filesystem::path& registryPath,
    const std::filesystem::path& root,
    const std::filesystem::path& jsonOut) {
    AssetRegistry registry;
    std::string loadError;
    if (!registry.load(registryPath, &loadError)) {
        std::cerr << "Could not load asset registry for native catalog validation: " << loadError << '\n';
        return 1;
    }

    const std::filesystem::path resolvedRoot = root.empty()
        ? (registryPath.has_parent_path() ? registryPath.parent_path() : std::filesystem::current_path())
        : root;

    NativeAssetCatalog catalog;
    catalog.buildFromRegistry(registry, resolvedRoot);

    std::unordered_set<AssetGuid> registryGuids;
    registryGuids.reserve(registry.records().size());
    for (const AssetRecord& record : registry.records()) {
        if (!record.guid.empty()) {
            registryGuids.insert(record.guid);
        }
    }

    uint64_t streamableBytes = 0;
    uint64_t missingBytes = 0;
    uint32_t streamableCount = 0;
    uint32_t missingCacheCount = 0;
    uint32_t nonStreamableCount = 0;
    uint32_t missingDependencyCount = 0;
    nlohmann::json issues = nlohmann::json::array();
    nlohmann::json roots = nlohmann::json::array();

    for (const auto& [guid, entry] : catalog.entries()) {
        if (entry.streamable) {
            ++streamableCount;
            streamableBytes += entry.fileBytes;
        } else {
            ++nonStreamableCount;
        }
        if (!entry.cacheFileExists) {
            ++missingCacheCount;
            missingBytes += entry.estimatedCpuBytes;
            issues.push_back({
                {"severity", "error"},
                {"kind", "MissingNativeCacheFile"},
                {"guid", guid},
                {"asset_type", assetTypeName(entry.assetType)},
                {"cache_path", entry.cachePath.generic_string()},
            });
        } else if (!entry.streamable) {
            issues.push_back({
                {"severity", "warning"},
                {"kind", "NonStreamableNativeCache"},
                {"guid", guid},
                {"asset_type", assetTypeName(entry.assetType)},
                {"native_kind", nativeAssetKindName(entry.nativeKind)},
                {"cache_path", entry.cachePath.generic_string()},
            });
        }
        for (const AssetDependency& dependency : entry.dependencies) {
            if (!dependency.guid.empty() && registryGuids.find(dependency.guid) == registryGuids.end()) {
                ++missingDependencyCount;
                issues.push_back({
                    {"severity", "error"},
                    {"kind", "MissingDependencyRecord"},
                    {"guid", guid},
                    {"dependency_guid", dependency.guid},
                    {"dependency_kind", dependency.kind},
                });
            }
        }
    }

    for (const AssetRecord& record : registry.records()) {
        if (record.type != AssetType::Prefab && record.type != AssetType::Scene && record.type != AssetType::Mesh) {
            continue;
        }
        const std::vector<AssetGuid> closure = catalog.dependencyClosure(record.guid);
        if (closure.empty()) {
            continue;
        }
        roots.push_back({
            {"guid", record.guid},
            {"asset_type", assetTypeName(record.type)},
            {"display_name", record.displayName},
            {"dependency_count", closure.size()},
            {"estimated_streaming_bytes", catalog.closureFileBytes(record.guid)},
            {"dependencies", closure},
        });
    }

    const nlohmann::json report = {
        {"schema", "NativeAssetCatalogValidationV1"},
        {"ok", missingCacheCount == 0 && missingDependencyCount == 0},
        {"registry_path", registryPath.generic_string()},
        {"root", resolvedRoot.generic_string()},
        {"asset_count", catalog.entries().size()},
        {"streamable_count", streamableCount},
        {"non_streamable_count", nonStreamableCount},
        {"missing_cache_count", missingCacheCount},
        {"missing_dependency_count", missingDependencyCount},
        {"streamable_bytes", streamableBytes},
        {"missing_bytes", missingBytes},
        {"roots", roots},
        {"issues", issues},
        {"catalog", catalog.toJson()},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Could not create catalog validation output directory: " << parent.string() << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Could not write catalog validation JSON: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return report.value("ok", false) ? 0 : 1;
}

} // namespace rtv
