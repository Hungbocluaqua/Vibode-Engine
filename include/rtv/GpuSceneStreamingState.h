#pragma once

#include "rtv/EntityId.h"
#include "rtv/NativeGpuAssetCache.h"
#include "rtv/SceneDocument.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

enum class GpuSceneStreamingInstanceState : uint8_t {
    MetadataOnly,
    PendingMesh,
    PendingBlas,
    PendingTlas,
    PendingMaterials,
    Resident,
    Disabled,
    Failed,
};

struct GpuSceneStreamingInstanceSnapshot {
    uint32_t gpuInstanceIndex = UINT32_MAX;
    EntityId entity{};
    uint64_t entityUuid = 0;
    std::string entityName;
    AssetGuid meshGuid;
    GpuSceneStreamingInstanceState state = GpuSceneStreamingInstanceState::MetadataOnly;
    bool hasLiveMeshHandle = false;
    bool visible = true;
    bool castShadow = true;
    bool visibleToCamera = true;
    bool cacheResident = false;
    bool blasReady = false;
    bool tlasVisible = false;
    bool renderable = false;
    bool fallbackDescriptors = true;
    bool restirLightCandidate = false;
    uint32_t materialSlotCount = 0;
    uint32_t pendingMaterialSlots = 0;
};

struct GpuSceneStreamingStats {
    uint32_t instanceCount = 0;
    uint32_t metadataOnlyInstances = 0;
    uint32_t pendingMeshInstances = 0;
    uint32_t pendingBlasInstances = 0;
    uint32_t pendingTlasInstances = 0;
    uint32_t pendingMaterialInstances = 0;
    uint32_t residentInstances = 0;
    uint32_t disabledInstances = 0;
    uint32_t failedInstances = 0;
    uint32_t renderableInstances = 0;
    uint32_t fallbackDescriptorInstances = 0;
    uint32_t restirLightCandidateInstances = 0;
};

struct GpuSceneStreamingUpdatePlanEntry {
    uint64_t entityUuid = 0;
    uint32_t previousGpuInstanceIndex = UINT32_MAX;
    uint32_t currentGpuInstanceIndex = UINT32_MAX;
    std::string entityName;
    AssetGuid meshGuid;
    GpuSceneStreamingInstanceState previousState = GpuSceneStreamingInstanceState::MetadataOnly;
    GpuSceneStreamingInstanceState currentState = GpuSceneStreamingInstanceState::MetadataOnly;
    bool added = false;
    bool removed = false;
    bool renderabilityChanged = false;
    bool becameRenderable = false;
    bool becameNonRenderable = false;
    bool resetTemporalHistory = false;
    bool resetRestirReservoir = false;
    bool updateRestirLightCandidates = false;
};

struct GpuSceneStreamingUpdatePlan {
    uint32_t addedInstances = 0;
    uint32_t removedInstances = 0;
    uint32_t changedInstances = 0;
    uint32_t becameRenderableInstances = 0;
    uint32_t becameNonRenderableInstances = 0;
    uint32_t temporalResetInstances = 0;
    uint32_t restirResetInstances = 0;
    uint32_t restirLightCandidateUpdateInstances = 0;
    std::vector<GpuSceneStreamingUpdatePlanEntry> entries;
};

class GpuSceneStreamingState {
public:
    void rebuild(
        const SceneDocument& document,
        const std::vector<EntityId>& gpuInstanceEntities,
        const NativeGpuAssetCache* cache = nullptr);

    [[nodiscard]] const std::vector<GpuSceneStreamingInstanceSnapshot>& instances() const { return instances_; }
    [[nodiscard]] GpuSceneStreamingStats stats() const;

private:
    std::vector<GpuSceneStreamingInstanceSnapshot> instances_;
};

[[nodiscard]] const char* gpuSceneStreamingInstanceStateName(GpuSceneStreamingInstanceState state);
[[nodiscard]] GpuSceneStreamingUpdatePlan buildGpuSceneStreamingUpdatePlan(
    const std::vector<GpuSceneStreamingInstanceSnapshot>& previous,
    const std::vector<GpuSceneStreamingInstanceSnapshot>& current);
[[nodiscard]] nlohmann::json gpuSceneStreamingStatsJson(const GpuSceneStreamingStats& stats);
[[nodiscard]] nlohmann::json gpuSceneStreamingInstancesJson(const std::vector<GpuSceneStreamingInstanceSnapshot>& instances);
[[nodiscard]] nlohmann::json gpuSceneStreamingUpdatePlanJson(const GpuSceneStreamingUpdatePlan& plan);
[[nodiscard]] int simulateGpuSceneStreamingStateCommand(
    uint32_t instanceCount,
    const std::filesystem::path& jsonOut = {});

} // namespace rtv
