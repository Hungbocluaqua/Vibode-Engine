#include "rtv/NativeGpuAssetCache.h"

#include "rtv/Buffer.h"
#include "rtv/ResourceAllocator.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <utility>

namespace rtv {

NativeGpuAssetCache::~NativeGpuAssetCache() = default;

const char* nativeGpuAssetKindName(NativeGpuAssetKind kind) {
    switch (kind) {
    case NativeGpuAssetKind::Mesh: return "mesh";
    case NativeGpuAssetKind::Texture: return "texture";
    case NativeGpuAssetKind::Material: return "material";
    case NativeGpuAssetKind::Blas: return "blas";
    }
    return "mesh";
}

const char* nativeGpuAssetResidencyName(NativeGpuAssetResidency residency) {
    switch (residency) {
    case NativeGpuAssetResidency::Unloaded: return "unloaded";
    case NativeGpuAssetResidency::Queued: return "queued";
    case NativeGpuAssetResidency::Uploading: return "uploading";
    case NativeGpuAssetResidency::Resident: return "resident";
    case NativeGpuAssetResidency::Evicting: return "evicting";
    case NativeGpuAssetResidency::Retired: return "retired";
    case NativeGpuAssetResidency::Failed: return "failed";
    }
    return "unloaded";
}

void NativeGpuAssetCache::upsert(NativeGpuAssetDesc desc) {
    if (desc.guid.empty()) {
        return;
    }
    if (desc.label.empty()) {
        desc.label = desc.guid;
    }
    if (Entry* existing = find(desc.guid)) {
        const uint32_t refCount = existing->refCount;
        const uint64_t lastTouchedFrame = ++frameCounter_;
        existing->desc = std::move(desc);
        existing->refCount = refCount;
        existing->lastTouchedFrame = lastTouchedFrame;
        if (existing->residency == NativeGpuAssetResidency::Retired ||
            existing->residency == NativeGpuAssetResidency::Failed ||
            existing->residency == NativeGpuAssetResidency::Unloaded) {
            existing->residency = NativeGpuAssetResidency::Queued;
        }
        return;
    }

    Entry entry;
    entry.desc = std::move(desc);
    entry.residency = NativeGpuAssetResidency::Queued;
    entry.lastTouchedFrame = ++frameCounter_;
    entries_.push_back(std::move(entry));
}

bool NativeGpuAssetCache::addRef(const AssetGuid& guid) {
    if (Entry* entry = find(guid)) {
        ++entry->refCount;
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::release(const AssetGuid& guid) {
    if (Entry* entry = find(guid)) {
        if (entry->refCount > 0) {
            --entry->refCount;
        }
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markUploading(const AssetGuid& guid, uint64_t uploadTicketId) {
    if (Entry* entry = find(guid)) {
        entry->desc.uploadTicketId = uploadTicketId;
        if (entry->desc.kind == NativeGpuAssetKind::Texture && entry->desc.mipCount > 0 && entry->desc.residentMipCount == 0) {
            entry->desc.residentMipCount = 1;
            entry->desc.lowestResidentMip = entry->desc.mipCount - 1u;
            entry->desc.highestResidentMip = entry->desc.mipCount - 1u;
        }
        entry->residency = NativeGpuAssetResidency::Uploading;
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markResident(const AssetGuid& guid) {
    if (Entry* entry = find(guid)) {
        entry->residency = NativeGpuAssetResidency::Resident;
        if (entry->desc.kind != NativeGpuAssetKind::Material || entry->desc.descriptorPatchComplete) {
            entry->desc.fallbackDescriptorBound = false;
        }
        if (entry->desc.kind == NativeGpuAssetKind::Texture && entry->desc.mipCount > 0) {
            entry->desc.residentMipCount = entry->desc.mipCount;
            entry->desc.lowestResidentMip = 0;
            entry->desc.highestResidentMip = entry->desc.mipCount - 1u;
        }
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markBlasBuildQueued(const AssetGuid& guid, uint64_t blasBuildTicketId) {
    if (Entry* entry = find(guid)) {
        entry->desc.blasBuildTicketId = blasBuildTicketId;
        entry->desc.blasBuildPending = true;
        entry->desc.blasReady = false;
        entry->desc.tlasVisible = false;
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markBlasReady(const AssetGuid& guid) {
    if (Entry* entry = find(guid)) {
        entry->desc.blasBuildPending = false;
        entry->desc.blasReady = true;
        if (entry->residency == NativeGpuAssetResidency::Queued) {
            entry->residency = NativeGpuAssetResidency::Uploading;
        }
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markTlasPatchQueued(const AssetGuid& guid, uint64_t tlasPatchTicketId) {
    if (Entry* entry = find(guid)) {
        entry->desc.tlasPatchTicketId = tlasPatchTicketId;
        entry->desc.tlasPatchPending = true;
        entry->desc.tlasVisible = false;
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markTlasVisible(const AssetGuid& guid) {
    if (Entry* entry = find(guid)) {
        entry->desc.tlasPatchPending = false;
        entry->desc.tlasVisible = true;
        if (entry->residency == NativeGpuAssetResidency::Queued) {
            entry->residency = NativeGpuAssetResidency::Uploading;
        }
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markTextureMipResident(const AssetGuid& guid, uint32_t mipLevel) {
    if (Entry* entry = find(guid)) {
        if (entry->desc.kind != NativeGpuAssetKind::Texture || entry->desc.mipCount == 0 || mipLevel >= entry->desc.mipCount) {
            return false;
        }
        if (entry->desc.residentMipCount == 0 ||
            entry->desc.lowestResidentMip == UINT32_MAX ||
            entry->desc.highestResidentMip == UINT32_MAX) {
            entry->desc.lowestResidentMip = mipLevel;
            entry->desc.highestResidentMip = mipLevel;
        } else {
            entry->desc.lowestResidentMip = std::min(entry->desc.lowestResidentMip, mipLevel);
            entry->desc.highestResidentMip = std::max(entry->desc.highestResidentMip, mipLevel);
        }
        entry->desc.residentMipCount = std::min(
            entry->desc.mipCount,
            entry->desc.highestResidentMip - entry->desc.lowestResidentMip + 1u);
        if (entry->desc.residentMipCount > 0 && entry->residency == NativeGpuAssetResidency::Queued) {
            entry->residency = NativeGpuAssetResidency::Uploading;
        }
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markDescriptorPatchQueued(const AssetGuid& guid, uint64_t descriptorTicketId) {
    if (Entry* entry = find(guid)) {
        entry->desc.descriptorPatchTicketId = static_cast<uint32_t>(std::min<uint64_t>(descriptorTicketId, UINT32_MAX));
        entry->desc.descriptorPatchPending = true;
        entry->desc.descriptorPatchComplete = false;
        entry->desc.fallbackDescriptorBound = true;
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markDescriptorPatchComplete(const AssetGuid& guid) {
    if (Entry* entry = find(guid)) {
        entry->desc.descriptorPatchPending = false;
        entry->desc.descriptorPatchComplete = true;
        entry->desc.descriptorResidentDependencyCount = entry->desc.descriptorDependencyCount;
        entry->desc.fallbackDescriptorBound = false;
        if (entry->residency == NativeGpuAssetResidency::Queued || entry->residency == NativeGpuAssetResidency::Uploading) {
            entry->residency = NativeGpuAssetResidency::Resident;
        }
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::markFailed(const AssetGuid& guid) {
    if (Entry* entry = find(guid)) {
        entry->residency = NativeGpuAssetResidency::Failed;
        entry->gpuBuffer.reset();
        entry->gpuImage.reset();
        entry->ownedGpuBufferBytes = 0;
        entry->ownedGpuImageBytes = 0;
        entry->desc.fallbackDescriptorBound = true;
        entry->desc.blasBuildPending = false;
        entry->desc.blasReady = false;
        entry->desc.tlasPatchPending = false;
        entry->desc.tlasVisible = false;
        entry->desc.descriptorPatchPending = false;
        entry->desc.descriptorPatchComplete = false;
        entry->desc.restirLightCandidate = false;
        if (entry->desc.kind == NativeGpuAssetKind::Texture) {
            entry->desc.residentMipCount = 0;
            entry->desc.lowestResidentMip = UINT32_MAX;
            entry->desc.highestResidentMip = UINT32_MAX;
        }
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::setPinned(const AssetGuid& guid, bool pinned) {
    if (Entry* entry = find(guid)) {
        entry->desc.pinned = pinned;
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::setSelected(const AssetGuid& guid, bool selected) {
    if (Entry* entry = find(guid)) {
        entry->desc.selected = selected;
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::touch(const AssetGuid& guid) {
    if (Entry* entry = find(guid)) {
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }
    return false;
}

bool NativeGpuAssetCache::ensureBufferResource(
    ResourceAllocator& allocator,
    const AssetGuid& guid,
    uint64_t bytes,
    uint32_t usageFlags,
    const char* debugName) {
    if (bytes == 0 || usageFlags == 0) {
        return false;
    }
    Entry* entry = find(guid);
    if (entry == nullptr) {
        return false;
    }
    if (entry->gpuBuffer != nullptr &&
        entry->gpuBuffer->handle() != VK_NULL_HANDLE &&
        entry->gpuBuffer->size() >= bytes &&
        (entry->gpuBuffer->usage() & usageFlags) == usageFlags) {
        entry->ownedGpuBufferBytes = static_cast<uint64_t>(entry->gpuBuffer->size());
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }

    BufferDesc desc{};
    desc.size = static_cast<VkDeviceSize>(bytes);
    desc.usage = static_cast<VkBufferUsageFlags>(usageFlags);
    desc.memory = BufferMemory::GpuOnly;
    desc.debugName = debugName;
    entry->gpuBuffer = std::make_unique<Buffer>(allocator, desc);
    entry->ownedGpuBufferBytes = bytes;
    entry->lastTouchedFrame = ++frameCounter_;
    return true;
}

Buffer* NativeGpuAssetCache::bufferResource(const AssetGuid& guid) {
    Entry* entry = find(guid);
    return entry != nullptr ? entry->gpuBuffer.get() : nullptr;
}

const Buffer* NativeGpuAssetCache::bufferResource(const AssetGuid& guid) const {
    const Entry* entry = find(guid);
    return entry != nullptr ? entry->gpuBuffer.get() : nullptr;
}

bool NativeGpuAssetCache::ensureImageResource(
    ResourceAllocator& allocator,
    const AssetGuid& guid,
    const ImageDesc& desc,
    uint64_t estimatedBytes) {
    Entry* entry = find(guid);
    if (entry == nullptr || desc.width == 0 || desc.height == 0 || desc.usage == 0) {
        return false;
    }
    if (entry->gpuImage != nullptr &&
        entry->gpuImage->handle() != VK_NULL_HANDLE &&
        entry->gpuImage->width() == std::max(desc.width, 1u) &&
        entry->gpuImage->height() == std::max(desc.height, 1u) &&
        entry->gpuImage->mipLevels() == std::max(desc.mipLevels, 1u) &&
        entry->gpuImage->format() == desc.format) {
        entry->ownedGpuImageBytes = std::max(entry->ownedGpuImageBytes, estimatedBytes);
        entry->lastTouchedFrame = ++frameCounter_;
        return true;
    }

    entry->gpuImage = std::make_unique<Image>(allocator, desc);
    entry->ownedGpuImageBytes = estimatedBytes;
    entry->lastTouchedFrame = ++frameCounter_;
    return true;
}

Image* NativeGpuAssetCache::imageResource(const AssetGuid& guid) {
    Entry* entry = find(guid);
    return entry != nullptr ? entry->gpuImage.get() : nullptr;
}

const Image* NativeGpuAssetCache::imageResource(const AssetGuid& guid) const {
    const Entry* entry = find(guid);
    return entry != nullptr ? entry->gpuImage.get() : nullptr;
}

NativeGpuAssetEvictionResult NativeGpuAssetCache::evictToBudget(const NativeGpuAssetCacheBudget& budget) {
    NativeGpuAssetEvictionResult result;
    NativeGpuAssetCacheStats current = stats();
    if (current.residentGpuBytes <= budget.maxGpuBytes && current.residentCpuBytes <= budget.maxCpuBytes) {
        result.budgetMet = true;
        return result;
    }

    std::vector<Entry*> candidates;
    for (Entry& entry : entries_) {
        if (evictable(entry, budget)) {
            candidates.push_back(&entry);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Entry* a, const Entry* b) {
        if (a->lastTouchedFrame != b->lastTouchedFrame) {
            return a->lastTouchedFrame < b->lastTouchedFrame;
        }
        return a->desc.guid < b->desc.guid;
    });

    for (Entry* entry : candidates) {
        if (current.residentGpuBytes <= budget.maxGpuBytes && current.residentCpuBytes <= budget.maxCpuBytes) {
            break;
        }
        entry->residency = NativeGpuAssetResidency::Retired;
        entry->gpuBuffer.reset();
        entry->gpuImage.reset();
        entry->ownedGpuBufferBytes = 0;
        entry->ownedGpuImageBytes = 0;
        entry->desc.fallbackDescriptorBound = true;
        entry->desc.blasBuildPending = false;
        entry->desc.blasReady = false;
        entry->desc.tlasPatchPending = false;
        entry->desc.tlasVisible = false;
        entry->desc.restirLightCandidate = false;
        if (entry->desc.kind == NativeGpuAssetKind::Texture) {
            entry->desc.residentMipCount = 0;
            entry->desc.lowestResidentMip = UINT32_MAX;
            entry->desc.highestResidentMip = UINT32_MAX;
        }
        result.evictedGuids.push_back(entry->desc.guid);
        result.freedGpuBytes += entry->desc.gpuBytes;
        result.freedCpuBytes += entry->desc.cpuBytes;
        ++result.evictedAssets;
        current.residentGpuBytes = entry->desc.gpuBytes > current.residentGpuBytes ? 0 : current.residentGpuBytes - entry->desc.gpuBytes;
        current.residentCpuBytes = entry->desc.cpuBytes > current.residentCpuBytes ? 0 : current.residentCpuBytes - entry->desc.cpuBytes;
        entry->lastTouchedFrame = ++frameCounter_;
    }
    result.budgetMet = current.residentGpuBytes <= budget.maxGpuBytes && current.residentCpuBytes <= budget.maxCpuBytes;
    return result;
}

NativeGpuAssetCacheStats NativeGpuAssetCache::stats() const {
    NativeGpuAssetCacheStats out;
    out.assetCount = static_cast<uint32_t>(entries_.size());
    for (const Entry& entry : entries_) {
        if (entry.desc.fallbackDescriptorBound) {
            ++out.fallbackDescriptorCount;
        }
        if (entry.desc.descriptorPatchPending) {
            ++out.descriptorPatchPendingCount;
        }
        if (entry.desc.descriptorPatchComplete) {
            ++out.descriptorPatchCompleteCount;
        }
        if (entry.desc.kind == NativeGpuAssetKind::Material && entry.desc.fallbackDescriptorBound) {
            ++out.descriptorFallbackMaterialCount;
        }
        if (entry.desc.kind == NativeGpuAssetKind::Material &&
            entry.desc.restirLightCandidate &&
            entry.residency == NativeGpuAssetResidency::Resident) {
            ++out.restirLightCandidateMaterialCount;
        }
        if (entry.desc.kind == NativeGpuAssetKind::Texture) {
            ++out.textureCount;
            out.residentTextureMipCount += entry.desc.residentMipCount;
            out.totalTextureMipCount += entry.desc.mipCount;
            if (entry.desc.fallbackDescriptorBound) {
                ++out.textureFallbackCount;
            }
            if (entry.desc.mipCount > 0 && entry.desc.residentMipCount >= entry.desc.mipCount) {
                ++out.textureFullyResidentCount;
            } else if (entry.desc.residentMipCount > 0) {
                ++out.texturePartiallyResidentCount;
            }
        }
        if (entry.desc.blasBuildPending) {
            ++out.blasPendingCount;
        }
        if (entry.desc.blasReady) {
            ++out.blasReadyCount;
        }
        if (entry.desc.tlasPatchPending) {
            ++out.tlasPatchPendingCount;
        }
        if (entry.desc.tlasVisible) {
            ++out.tlasVisibleCount;
        }
        if (entry.desc.kind == NativeGpuAssetKind::Mesh &&
            entry.residency == NativeGpuAssetResidency::Resident &&
            entry.desc.blasReady &&
            entry.desc.tlasVisible) {
            ++out.meshRenderableCount;
        }
        NativeGpuAssetCacheBudget defaultBudget;
        if (evictable(entry, defaultBudget)) {
            ++out.evictableCount;
        }
        if (entry.residency == NativeGpuAssetResidency::Resident) {
            ++out.residentCount;
            out.residentGpuBytes += entry.desc.gpuBytes;
            out.residentCpuBytes += entry.desc.cpuBytes;
        } else if (entry.residency == NativeGpuAssetResidency::Uploading) {
            ++out.uploadingCount;
            out.inFlightUploadBytes += entry.desc.uploadBytes;
        } else if (entry.residency == NativeGpuAssetResidency::Retired) {
            ++out.retiredCount;
        }
    }
    return out;
}

std::vector<NativeGpuAssetSnapshot> NativeGpuAssetCache::snapshots() const {
    std::vector<NativeGpuAssetSnapshot> out;
    out.reserve(entries_.size());
    NativeGpuAssetCacheBudget defaultBudget;
    for (const Entry& entry : entries_) {
        out.push_back({
            .kind = entry.desc.kind,
            .residency = entry.residency,
            .guid = entry.desc.guid,
            .label = entry.desc.label,
            .cpuBytes = entry.desc.cpuBytes,
            .gpuBytes = entry.desc.gpuBytes,
            .uploadBytes = entry.desc.uploadBytes,
            .ownedGpuBufferBytes = entry.ownedGpuBufferBytes,
            .ownedGpuImageBytes = entry.ownedGpuImageBytes,
            .uploadTicketId = entry.desc.uploadTicketId,
            .blasBuildTicketId = entry.desc.blasBuildTicketId,
            .tlasPatchTicketId = entry.desc.tlasPatchTicketId,
            .lastTouchedFrame = entry.lastTouchedFrame,
            .mipCount = entry.desc.mipCount,
            .residentMipCount = entry.desc.residentMipCount,
            .lowestResidentMip = entry.desc.lowestResidentMip,
            .highestResidentMip = entry.desc.highestResidentMip,
            .descriptorSlot = entry.desc.descriptorSlot,
            .descriptorPatchTicketId = entry.desc.descriptorPatchTicketId,
            .descriptorDependencyCount = entry.desc.descriptorDependencyCount,
            .descriptorResidentDependencyCount = entry.desc.descriptorResidentDependencyCount,
            .generation = entry.desc.generation,
            .refCount = entry.refCount,
            .fallbackDescriptorBound = entry.desc.fallbackDescriptorBound,
            .blasBuildPending = entry.desc.blasBuildPending,
            .blasReady = entry.desc.blasReady,
            .tlasPatchPending = entry.desc.tlasPatchPending,
            .tlasVisible = entry.desc.tlasVisible,
            .descriptorPatchPending = entry.desc.descriptorPatchPending,
            .descriptorPatchComplete = entry.desc.descriptorPatchComplete,
            .restirLightCandidate = entry.desc.restirLightCandidate,
            .pinned = entry.desc.pinned,
            .selected = entry.desc.selected,
            .evictable = evictable(entry, defaultBudget),
            .ownsGpuBuffer = entry.gpuBuffer != nullptr && entry.gpuBuffer->handle() != VK_NULL_HANDLE,
            .ownsGpuImage = entry.gpuImage != nullptr && entry.gpuImage->handle() != VK_NULL_HANDLE,
        });
    }
    std::sort(out.begin(), out.end(), [](const NativeGpuAssetSnapshot& a, const NativeGpuAssetSnapshot& b) {
        return a.guid < b.guid;
    });
    return out;
}

NativeGpuAssetCache::Entry* NativeGpuAssetCache::find(const AssetGuid& guid) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&guid](const Entry& entry) {
        return entry.desc.guid == guid;
    });
    return it == entries_.end() ? nullptr : &*it;
}

const NativeGpuAssetCache::Entry* NativeGpuAssetCache::find(const AssetGuid& guid) const {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&guid](const Entry& entry) {
        return entry.desc.guid == guid;
    });
    return it == entries_.end() ? nullptr : &*it;
}

bool NativeGpuAssetCache::evictable(const Entry& entry, const NativeGpuAssetCacheBudget& budget) const {
    if (entry.residency != NativeGpuAssetResidency::Resident) {
        return false;
    }
    if (entry.refCount != 0) {
        return false;
    }
    if (entry.desc.uploadTicketId != 0 && entry.residency == NativeGpuAssetResidency::Uploading) {
        return false;
    }
    if (entry.desc.pinned && !budget.allowPinnedEviction) {
        return false;
    }
    if (entry.desc.selected && !budget.allowSelectedEviction) {
        return false;
    }
    return true;
}

nlohmann::json nativeGpuAssetCacheStatsJson(const NativeGpuAssetCacheStats& stats) {
    return {
        {"asset_count", stats.assetCount},
        {"resident_count", stats.residentCount},
        {"uploading_count", stats.uploadingCount},
        {"retired_count", stats.retiredCount},
        {"evictable_count", stats.evictableCount},
        {"texture_count", stats.textureCount},
        {"texture_fully_resident_count", stats.textureFullyResidentCount},
        {"texture_partially_resident_count", stats.texturePartiallyResidentCount},
        {"texture_fallback_count", stats.textureFallbackCount},
        {"resident_texture_mip_count", stats.residentTextureMipCount},
        {"total_texture_mip_count", stats.totalTextureMipCount},
        {"blas_pending_count", stats.blasPendingCount},
        {"blas_ready_count", stats.blasReadyCount},
        {"tlas_patch_pending_count", stats.tlasPatchPendingCount},
        {"tlas_visible_count", stats.tlasVisibleCount},
        {"mesh_renderable_count", stats.meshRenderableCount},
        {"descriptor_patch_pending_count", stats.descriptorPatchPendingCount},
        {"descriptor_patch_complete_count", stats.descriptorPatchCompleteCount},
        {"descriptor_fallback_material_count", stats.descriptorFallbackMaterialCount},
        {"restir_light_candidate_material_count", stats.restirLightCandidateMaterialCount},
        {"fallback_descriptor_count", stats.fallbackDescriptorCount},
        {"resident_gpu_bytes", stats.residentGpuBytes},
        {"resident_cpu_bytes", stats.residentCpuBytes},
        {"in_flight_upload_bytes", stats.inFlightUploadBytes},
    };
}

nlohmann::json nativeGpuAssetEvictionResultJson(const NativeGpuAssetEvictionResult& result) {
    nlohmann::json evictedGuids = nlohmann::json::array();
    for (const AssetGuid& guid : result.evictedGuids) {
        evictedGuids.push_back(guid);
    }
    return {
        {"evicted_assets", result.evictedAssets},
        {"freed_gpu_bytes", result.freedGpuBytes},
        {"freed_cpu_bytes", result.freedCpuBytes},
        {"budget_met", result.budgetMet},
        {"evicted_guids", evictedGuids},
    };
}

nlohmann::json nativeGpuAssetCacheSnapshotsJson(const std::vector<NativeGpuAssetSnapshot>& snapshots) {
    nlohmann::json out = nlohmann::json::array();
    for (const NativeGpuAssetSnapshot& snapshot : snapshots) {
        out.push_back({
            {"kind", nativeGpuAssetKindName(snapshot.kind)},
            {"residency", nativeGpuAssetResidencyName(snapshot.residency)},
            {"guid", snapshot.guid},
            {"label", snapshot.label},
            {"cpu_bytes", snapshot.cpuBytes},
            {"gpu_bytes", snapshot.gpuBytes},
            {"upload_bytes", snapshot.uploadBytes},
            {"owned_gpu_buffer_bytes", snapshot.ownedGpuBufferBytes},
            {"owned_gpu_image_bytes", snapshot.ownedGpuImageBytes},
            {"upload_ticket_id", snapshot.uploadTicketId},
            {"blas_build_ticket_id", snapshot.blasBuildTicketId},
            {"tlas_patch_ticket_id", snapshot.tlasPatchTicketId},
            {"last_touched_frame", snapshot.lastTouchedFrame},
            {"mip_count", snapshot.mipCount},
            {"resident_mip_count", snapshot.residentMipCount},
            {"lowest_resident_mip", snapshot.lowestResidentMip == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(snapshot.lowestResidentMip)},
            {"highest_resident_mip", snapshot.highestResidentMip == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(snapshot.highestResidentMip)},
            {"descriptor_slot", snapshot.descriptorSlot},
            {"descriptor_patch_ticket_id", snapshot.descriptorPatchTicketId},
            {"descriptor_dependency_count", snapshot.descriptorDependencyCount},
            {"descriptor_resident_dependency_count", snapshot.descriptorResidentDependencyCount},
            {"generation", snapshot.generation},
            {"ref_count", snapshot.refCount},
            {"fallback_descriptor_bound", snapshot.fallbackDescriptorBound},
            {"blas_build_pending", snapshot.blasBuildPending},
            {"blas_ready", snapshot.blasReady},
            {"tlas_patch_pending", snapshot.tlasPatchPending},
            {"tlas_visible", snapshot.tlasVisible},
            {"descriptor_patch_pending", snapshot.descriptorPatchPending},
            {"descriptor_patch_complete", snapshot.descriptorPatchComplete},
            {"restir_light_candidate", snapshot.restirLightCandidate},
            {"pinned", snapshot.pinned},
            {"selected", snapshot.selected},
            {"evictable", snapshot.evictable},
            {"owns_gpu_buffer", snapshot.ownsGpuBuffer},
            {"owns_gpu_image", snapshot.ownsGpuImage},
        });
    }
    return out;
}

int simulateNativeGpuAssetCacheCommand(
    uint32_t assetCount,
    const NativeGpuAssetCacheBudget& budget,
    const std::filesystem::path& jsonOut) {
    NativeGpuAssetCache cache;
    for (uint32_t i = 0; i < assetCount; ++i) {
        NativeGpuAssetDesc desc;
        desc.kind = static_cast<NativeGpuAssetKind>(i % 4u);
        desc.guid = "native-gpu-asset-" + std::to_string(i);
        desc.label = std::string("native GPU asset ") + std::to_string(i);
        desc.cpuBytes = (1ull + (i % 3u)) * 1024ull * 1024ull;
        desc.gpuBytes = (8ull + (i % 7u) * 4ull) * 1024ull * 1024ull;
        desc.uploadBytes = desc.gpuBytes;
        desc.restirLightCandidate = desc.kind == NativeGpuAssetKind::Material && (i % 8u) == 2u;
        if (desc.kind == NativeGpuAssetKind::Texture) {
            desc.mipCount = 6;
            desc.residentMipCount = 1;
            desc.lowestResidentMip = 5;
            desc.highestResidentMip = 5;
        } else if (desc.kind == NativeGpuAssetKind::Mesh || desc.kind == NativeGpuAssetKind::Blas) {
            desc.blasBuildPending = true;
            desc.tlasPatchPending = true;
        } else if (desc.kind == NativeGpuAssetKind::Material) {
            desc.descriptorDependencyCount = 3u + (i % 4u);
            desc.descriptorResidentDependencyCount = 0;
            desc.descriptorPatchPending = true;
        }
        desc.descriptorSlot = i;
        desc.generation = 1u + (i % 3u);
        desc.pinned = i == 0 || i == 4;
        desc.selected = i == 1;
        cache.upsert(std::move(desc));
        const AssetGuid guid = "native-gpu-asset-" + std::to_string(i);
        if ((i % 5u) == 0u) {
            (void)cache.markUploading(guid, 1000ull + i);
        } else {
            (void)cache.markResident(guid);
        }
        if (desc.kind == NativeGpuAssetKind::Mesh || desc.kind == NativeGpuAssetKind::Blas) {
            (void)cache.markBlasBuildQueued(guid, 3000ull + i);
            (void)cache.markTlasPatchQueued(guid, 4000ull + i);
            if ((i % 8u) != 0u) {
                (void)cache.markBlasReady(guid);
            }
            if ((i % 12u) != 0u) {
                (void)cache.markTlasVisible(guid);
            }
        }
        if (desc.kind == NativeGpuAssetKind::Material) {
            (void)cache.markDescriptorPatchQueued(guid, 2000ull + i);
            if ((i % 8u) == 2u) {
                (void)cache.markDescriptorPatchComplete(guid);
            }
        }
        if ((i % 6u) == 0u) {
            (void)cache.addRef(guid);
        }
        if ((i % 2u) == 0u) {
            (void)cache.touch(guid);
        }
    }

    const NativeGpuAssetCacheStats before = cache.stats();
    const NativeGpuAssetEvictionResult eviction = cache.evictToBudget(budget);
    const NativeGpuAssetCacheStats after = cache.stats();
    const nlohmann::json report = {
        {"schema", "NativeGpuAssetCacheSimulationV1"},
        {"ok", after.residentGpuBytes <= budget.maxGpuBytes && after.residentCpuBytes <= budget.maxCpuBytes},
        {"asset_count", assetCount},
        {"budget", {
            {"max_gpu_bytes", budget.maxGpuBytes},
            {"max_cpu_bytes", budget.maxCpuBytes},
            {"allow_selected_eviction", budget.allowSelectedEviction},
            {"allow_pinned_eviction", budget.allowPinnedEviction},
        }},
        {"before", nativeGpuAssetCacheStatsJson(before)},
        {"after", nativeGpuAssetCacheStatsJson(after)},
        {"eviction", nativeGpuAssetEvictionResultJson(eviction)},
        {"assets", nativeGpuAssetCacheSnapshotsJson(cache.snapshots())},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Could not create native GPU asset cache report directory: " << parent.string() << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Could not write native GPU asset cache report: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return report.value("ok", false) ? 0 : 1;
}

} // namespace rtv
