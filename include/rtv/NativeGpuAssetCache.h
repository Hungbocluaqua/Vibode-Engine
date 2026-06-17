#pragma once

#include "rtv/AssetRegistry.h"
#include "rtv/Buffer.h"
#include "rtv/Image.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rtv {

class ResourceAllocator;

enum class NativeGpuAssetKind : uint8_t {
    Mesh,
    Texture,
    Material,
    Blas,
};

enum class NativeGpuAssetResidency : uint8_t {
    Unloaded,
    Queued,
    Uploading,
    Resident,
    Evicting,
    Retired,
    Failed,
};

struct NativeGpuAssetCacheBudget {
    uint64_t maxGpuBytes = 1024ull * 1024ull * 1024ull;
    uint64_t maxCpuBytes = 512ull * 1024ull * 1024ull;
    bool allowSelectedEviction = false;
    bool allowPinnedEviction = false;
};

struct NativeGpuAssetDesc {
    NativeGpuAssetKind kind = NativeGpuAssetKind::Mesh;
    AssetGuid guid;
    std::string label;
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    uint64_t uploadBytes = 0;
    uint64_t uploadTicketId = 0;
    uint64_t blasBuildTicketId = 0;
    uint64_t tlasPatchTicketId = 0;
    uint32_t mipCount = 0;
    uint32_t residentMipCount = 0;
    uint32_t lowestResidentMip = UINT32_MAX;
    uint32_t highestResidentMip = UINT32_MAX;
    uint32_t descriptorSlot = 0;
    uint32_t descriptorPatchTicketId = 0;
    uint32_t descriptorDependencyCount = 0;
    uint32_t descriptorResidentDependencyCount = 0;
    uint32_t generation = 0;
    bool fallbackDescriptorBound = true;
    bool blasBuildPending = false;
    bool blasReady = false;
    bool tlasPatchPending = false;
    bool tlasVisible = false;
    bool descriptorPatchPending = false;
    bool descriptorPatchComplete = false;
    bool restirLightCandidate = false;
    bool pinned = false;
    bool selected = false;
};

struct NativeGpuAssetSnapshot {
    NativeGpuAssetKind kind = NativeGpuAssetKind::Mesh;
    NativeGpuAssetResidency residency = NativeGpuAssetResidency::Unloaded;
    AssetGuid guid;
    std::string label;
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    uint64_t uploadBytes = 0;
    uint64_t ownedGpuBufferBytes = 0;
    uint64_t ownedGpuImageBytes = 0;
    uint64_t uploadTicketId = 0;
    uint64_t blasBuildTicketId = 0;
    uint64_t tlasPatchTicketId = 0;
    uint64_t lastTouchedFrame = 0;
    uint32_t mipCount = 0;
    uint32_t residentMipCount = 0;
    uint32_t lowestResidentMip = UINT32_MAX;
    uint32_t highestResidentMip = UINT32_MAX;
    uint32_t descriptorSlot = 0;
    uint32_t descriptorPatchTicketId = 0;
    uint32_t descriptorDependencyCount = 0;
    uint32_t descriptorResidentDependencyCount = 0;
    uint32_t generation = 0;
    uint32_t refCount = 0;
    bool fallbackDescriptorBound = true;
    bool blasBuildPending = false;
    bool blasReady = false;
    bool tlasPatchPending = false;
    bool tlasVisible = false;
    bool descriptorPatchPending = false;
    bool descriptorPatchComplete = false;
    bool restirLightCandidate = false;
    bool pinned = false;
    bool selected = false;
    bool evictable = false;
    bool ownsGpuBuffer = false;
    bool ownsGpuImage = false;
};

struct NativeGpuAssetCacheStats {
    uint32_t assetCount = 0;
    uint32_t residentCount = 0;
    uint32_t uploadingCount = 0;
    uint32_t retiredCount = 0;
    uint32_t evictableCount = 0;
    uint32_t textureCount = 0;
    uint32_t textureFullyResidentCount = 0;
    uint32_t texturePartiallyResidentCount = 0;
    uint32_t textureFallbackCount = 0;
    uint32_t residentTextureMipCount = 0;
    uint32_t totalTextureMipCount = 0;
    uint32_t blasPendingCount = 0;
    uint32_t blasReadyCount = 0;
    uint32_t tlasPatchPendingCount = 0;
    uint32_t tlasVisibleCount = 0;
    uint32_t meshRenderableCount = 0;
    uint32_t descriptorPatchPendingCount = 0;
    uint32_t descriptorPatchCompleteCount = 0;
    uint32_t descriptorFallbackMaterialCount = 0;
    uint32_t restirLightCandidateMaterialCount = 0;
    uint32_t fallbackDescriptorCount = 0;
    uint64_t residentGpuBytes = 0;
    uint64_t residentCpuBytes = 0;
    uint64_t inFlightUploadBytes = 0;
};

struct NativeGpuAssetEvictionResult {
    uint32_t evictedAssets = 0;
    uint64_t freedGpuBytes = 0;
    uint64_t freedCpuBytes = 0;
    bool budgetMet = true;
    std::vector<AssetGuid> evictedGuids;
};

class NativeGpuAssetCache {
public:
    ~NativeGpuAssetCache();

    void upsert(NativeGpuAssetDesc desc);
    [[nodiscard]] bool addRef(const AssetGuid& guid);
    [[nodiscard]] bool release(const AssetGuid& guid);
    [[nodiscard]] bool markUploading(const AssetGuid& guid, uint64_t uploadTicketId);
    [[nodiscard]] bool markResident(const AssetGuid& guid);
    [[nodiscard]] bool markFailed(const AssetGuid& guid);
    [[nodiscard]] bool markTextureMipResident(const AssetGuid& guid, uint32_t mipLevel);
    [[nodiscard]] bool markBlasBuildQueued(const AssetGuid& guid, uint64_t blasBuildTicketId);
    [[nodiscard]] bool markBlasReady(const AssetGuid& guid);
    [[nodiscard]] bool markTlasPatchQueued(const AssetGuid& guid, uint64_t tlasPatchTicketId);
    [[nodiscard]] bool markTlasVisible(const AssetGuid& guid);
    [[nodiscard]] bool markDescriptorPatchQueued(const AssetGuid& guid, uint64_t descriptorTicketId);
    [[nodiscard]] bool markDescriptorPatchComplete(const AssetGuid& guid);
    [[nodiscard]] bool setPinned(const AssetGuid& guid, bool pinned);
    [[nodiscard]] bool setSelected(const AssetGuid& guid, bool selected);
    [[nodiscard]] bool touch(const AssetGuid& guid);
    [[nodiscard]] bool ensureBufferResource(ResourceAllocator& allocator, const AssetGuid& guid, uint64_t bytes, uint32_t usageFlags, const char* debugName);
    [[nodiscard]] Buffer* bufferResource(const AssetGuid& guid);
    [[nodiscard]] const Buffer* bufferResource(const AssetGuid& guid) const;
    [[nodiscard]] bool ensureImageResource(ResourceAllocator& allocator, const AssetGuid& guid, const ImageDesc& desc, uint64_t estimatedBytes);
    [[nodiscard]] Image* imageResource(const AssetGuid& guid);
    [[nodiscard]] const Image* imageResource(const AssetGuid& guid) const;
    [[nodiscard]] NativeGpuAssetEvictionResult evictToBudget(const NativeGpuAssetCacheBudget& budget);
    [[nodiscard]] NativeGpuAssetCacheStats stats() const;
    [[nodiscard]] std::vector<NativeGpuAssetSnapshot> snapshots() const;

private:
    struct Entry {
        NativeGpuAssetDesc desc{};
        NativeGpuAssetResidency residency = NativeGpuAssetResidency::Queued;
        uint32_t refCount = 0;
        uint64_t lastTouchedFrame = 0;
        uint64_t ownedGpuBufferBytes = 0;
        uint64_t ownedGpuImageBytes = 0;
        std::unique_ptr<Buffer> gpuBuffer;
        std::unique_ptr<Image> gpuImage;
    };

    [[nodiscard]] Entry* find(const AssetGuid& guid);
    [[nodiscard]] const Entry* find(const AssetGuid& guid) const;
    [[nodiscard]] bool evictable(const Entry& entry, const NativeGpuAssetCacheBudget& budget) const;

    uint64_t frameCounter_ = 1;
    std::vector<Entry> entries_;
};

[[nodiscard]] const char* nativeGpuAssetKindName(NativeGpuAssetKind kind);
[[nodiscard]] const char* nativeGpuAssetResidencyName(NativeGpuAssetResidency residency);
[[nodiscard]] nlohmann::json nativeGpuAssetCacheStatsJson(const NativeGpuAssetCacheStats& stats);
[[nodiscard]] nlohmann::json nativeGpuAssetEvictionResultJson(const NativeGpuAssetEvictionResult& result);
[[nodiscard]] nlohmann::json nativeGpuAssetCacheSnapshotsJson(const std::vector<NativeGpuAssetSnapshot>& snapshots);
[[nodiscard]] int simulateNativeGpuAssetCacheCommand(
    uint32_t assetCount,
    const NativeGpuAssetCacheBudget& budget,
    const std::filesystem::path& jsonOut = {});

} // namespace rtv
