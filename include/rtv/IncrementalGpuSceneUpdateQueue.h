#pragma once

#include "rtv/GpuSceneStreamingState.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

enum class IncrementalGpuSceneOperationKind : uint8_t {
    AddInstance,
    RemoveInstance,
    PatchBlas,
    PatchTlas,
    PatchDescriptors,
    ResetTemporalHistory,
    ResetRestirReservoir,
    UpdateRestirLightCandidates,
};

enum class IncrementalGpuSceneOperationState : uint8_t {
    Queued,
    Applied,
    Cancelled,
};

struct IncrementalGpuSceneOperationDesc {
    IncrementalGpuSceneOperationKind kind = IncrementalGpuSceneOperationKind::AddInstance;
    uint64_t entityUuid = 0;
    uint32_t gpuInstanceIndex = UINT32_MAX;
    AssetGuid meshGuid;
    std::string label;
    double estimatedApplyMs = 0.05;
    bool requiresRenderThread = true;
};

struct IncrementalGpuSceneApplyBudget {
    double maxApplyMs = 1.0;
    uint32_t maxOperations = 16;
    uint32_t maxTlasPatches = 4;
    uint32_t maxDescriptorPatches = 64;
    uint32_t maxResetMasks = 64;
};

struct IncrementalGpuSceneApplyFrameResult {
    uint32_t frameIndex = 0;
    uint32_t appliedOperations = 0;
    uint32_t appliedTlasPatches = 0;
    uint32_t appliedDescriptorPatches = 0;
    uint32_t appliedResetMasks = 0;
    double appliedMs = 0.0;
    std::vector<uint64_t> temporalResetEntityUuids;
    std::vector<uint64_t> restirResetEntityUuids;
    std::vector<uint64_t> restirLightCandidateEntityUuids;
    bool applyBudgetExhausted = false;
    bool operationBudgetExhausted = false;
    bool tlasBudgetExhausted = false;
    bool descriptorBudgetExhausted = false;
    bool resetMaskBudgetExhausted = false;
};

struct IncrementalGpuSceneUpdateSnapshot {
    uint64_t id = 0;
    IncrementalGpuSceneOperationKind kind = IncrementalGpuSceneOperationKind::AddInstance;
    IncrementalGpuSceneOperationState state = IncrementalGpuSceneOperationState::Queued;
    uint64_t entityUuid = 0;
    uint32_t gpuInstanceIndex = UINT32_MAX;
    AssetGuid meshGuid;
    std::string label;
    double estimatedApplyMs = 0.0;
    bool requiresRenderThread = true;
};

struct IncrementalGpuSceneUpdateStats {
    uint32_t queued = 0;
    uint32_t applied = 0;
    uint32_t cancelled = 0;
    uint32_t tlasPatchesQueued = 0;
    uint32_t descriptorPatchesQueued = 0;
    uint32_t resetMasksQueued = 0;
    uint32_t restirLightCandidateUpdatesQueued = 0;
    uint32_t temporalResetsApplied = 0;
    uint32_t restirResetsApplied = 0;
    uint32_t restirLightCandidateUpdatesApplied = 0;
};

class IncrementalGpuSceneUpdateQueue {
public:
    [[nodiscard]] uint64_t enqueue(IncrementalGpuSceneOperationDesc desc);
    void enqueueUpdatePlan(const GpuSceneStreamingUpdatePlan& plan);
    [[nodiscard]] IncrementalGpuSceneApplyFrameResult applyFrame(const IncrementalGpuSceneApplyBudget& budget);
    [[nodiscard]] bool cancelByEntity(uint64_t entityUuid);
    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::vector<IncrementalGpuSceneUpdateSnapshot> snapshots() const;
    [[nodiscard]] IncrementalGpuSceneUpdateStats stats() const;

private:
    struct Operation {
        uint64_t id = 0;
        IncrementalGpuSceneOperationDesc desc{};
        IncrementalGpuSceneOperationState state = IncrementalGpuSceneOperationState::Queued;
    };

    [[nodiscard]] bool canApply(
        const Operation& operation,
        const IncrementalGpuSceneApplyBudget& budget,
        const IncrementalGpuSceneApplyFrameResult& frame,
        IncrementalGpuSceneApplyFrameResult& exhausted) const;

    uint64_t nextOperationId_ = 1;
    uint32_t frameIndex_ = 0;
    std::vector<Operation> operations_;
};

[[nodiscard]] const char* incrementalGpuSceneOperationKindName(IncrementalGpuSceneOperationKind kind);
[[nodiscard]] const char* incrementalGpuSceneOperationStateName(IncrementalGpuSceneOperationState state);
[[nodiscard]] nlohmann::json incrementalGpuSceneApplyFrameResultJson(const IncrementalGpuSceneApplyFrameResult& frame);
[[nodiscard]] nlohmann::json incrementalGpuSceneUpdateStatsJson(const IncrementalGpuSceneUpdateStats& stats);
[[nodiscard]] nlohmann::json incrementalGpuSceneUpdateSnapshotsJson(const std::vector<IncrementalGpuSceneUpdateSnapshot>& snapshots);
[[nodiscard]] int simulateIncrementalGpuSceneUpdateQueueCommand(
    uint32_t instanceCount,
    const IncrementalGpuSceneApplyBudget& budget,
    uint32_t cancelAfterFrame,
    const std::filesystem::path& jsonOut = {});

} // namespace rtv
