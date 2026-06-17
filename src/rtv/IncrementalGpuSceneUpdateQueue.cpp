#include "rtv/IncrementalGpuSceneUpdateQueue.h"

#include "rtv/NativeGpuAssetCache.h"
#include "rtv/SceneDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace rtv {
namespace {

bool terminal(IncrementalGpuSceneOperationState state) {
    return state == IncrementalGpuSceneOperationState::Applied ||
        state == IncrementalGpuSceneOperationState::Cancelled;
}

bool isTlasPatch(IncrementalGpuSceneOperationKind kind) {
    return kind == IncrementalGpuSceneOperationKind::AddInstance ||
        kind == IncrementalGpuSceneOperationKind::RemoveInstance ||
        kind == IncrementalGpuSceneOperationKind::PatchTlas;
}

bool isDescriptorPatch(IncrementalGpuSceneOperationKind kind) {
    return kind == IncrementalGpuSceneOperationKind::PatchDescriptors ||
        kind == IncrementalGpuSceneOperationKind::UpdateRestirLightCandidates;
}

bool isResetMask(IncrementalGpuSceneOperationKind kind) {
    return kind == IncrementalGpuSceneOperationKind::ResetTemporalHistory ||
        kind == IncrementalGpuSceneOperationKind::ResetRestirReservoir;
}

IncrementalGpuSceneOperationDesc makeOperation(
    IncrementalGpuSceneOperationKind kind,
    const GpuSceneStreamingUpdatePlanEntry& entry,
    std::string label,
    double estimatedApplyMs) {
    return {
        .kind = kind,
        .entityUuid = entry.entityUuid,
        .gpuInstanceIndex = entry.currentGpuInstanceIndex != UINT32_MAX ? entry.currentGpuInstanceIndex : entry.previousGpuInstanceIndex,
        .meshGuid = entry.meshGuid,
        .label = std::move(label),
        .estimatedApplyMs = estimatedApplyMs,
        .requiresRenderThread = true,
    };
}

void writeReportOrStdout(const nlohmann::json& report, const std::filesystem::path& jsonOut) {
    if (jsonOut.empty()) {
        std::cout << report.dump(2) << '\n';
        return;
    }
    std::error_code ec;
    const std::filesystem::path parent = jsonOut.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("Could not create incremental GpuScene update report directory: " + parent.string() + " (" + ec.message() + ")");
        }
    }
    std::ofstream file(jsonOut);
    if (!file.is_open()) {
        throw std::runtime_error("Could not write incremental GpuScene update report: " + jsonOut.string());
    }
    file << report.dump(2);
}

} // namespace

const char* incrementalGpuSceneOperationKindName(IncrementalGpuSceneOperationKind kind) {
    switch (kind) {
    case IncrementalGpuSceneOperationKind::AddInstance: return "add_instance";
    case IncrementalGpuSceneOperationKind::RemoveInstance: return "remove_instance";
    case IncrementalGpuSceneOperationKind::PatchBlas: return "patch_blas";
    case IncrementalGpuSceneOperationKind::PatchTlas: return "patch_tlas";
    case IncrementalGpuSceneOperationKind::PatchDescriptors: return "patch_descriptors";
    case IncrementalGpuSceneOperationKind::ResetTemporalHistory: return "reset_temporal_history";
    case IncrementalGpuSceneOperationKind::ResetRestirReservoir: return "reset_restir_reservoir";
    case IncrementalGpuSceneOperationKind::UpdateRestirLightCandidates: return "update_restir_light_candidates";
    }
    return "add_instance";
}

const char* incrementalGpuSceneOperationStateName(IncrementalGpuSceneOperationState state) {
    switch (state) {
    case IncrementalGpuSceneOperationState::Queued: return "queued";
    case IncrementalGpuSceneOperationState::Applied: return "applied";
    case IncrementalGpuSceneOperationState::Cancelled: return "cancelled";
    }
    return "queued";
}

uint64_t IncrementalGpuSceneUpdateQueue::enqueue(IncrementalGpuSceneOperationDesc desc) {
    if (desc.entityUuid == 0) {
        return 0;
    }
    if (desc.label.empty()) {
        desc.label = std::string(incrementalGpuSceneOperationKindName(desc.kind)) + " " + std::to_string(desc.entityUuid);
    }
    Operation operation;
    operation.id = nextOperationId_++;
    operation.desc = std::move(desc);
    operations_.push_back(std::move(operation));
    return operations_.back().id;
}

void IncrementalGpuSceneUpdateQueue::enqueueUpdatePlan(const GpuSceneStreamingUpdatePlan& plan) {
    for (const GpuSceneStreamingUpdatePlanEntry& entry : plan.entries) {
        const bool needsTlasPatch =
            (entry.renderabilityChanged && !entry.becameRenderable && !entry.removed) ||
            entry.currentState == GpuSceneStreamingInstanceState::PendingTlas;
        const bool needsDescriptorPatch =
            entry.becameRenderable ||
            entry.currentState == GpuSceneStreamingInstanceState::PendingMaterials;
        if (entry.removed && entry.becameNonRenderable) {
            (void)enqueue(makeOperation(
                IncrementalGpuSceneOperationKind::RemoveInstance,
                entry,
                "remove streamed instance " + std::to_string(entry.entityUuid),
                0.08));
        } else if (entry.becameRenderable) {
            (void)enqueue(makeOperation(
                IncrementalGpuSceneOperationKind::AddInstance,
                entry,
                "add streamed instance " + std::to_string(entry.entityUuid),
                0.10));
        }
        if (entry.becameRenderable) {
            (void)enqueue(makeOperation(
                IncrementalGpuSceneOperationKind::PatchBlas,
                entry,
                "patch streamed BLAS handle " + std::to_string(entry.entityUuid),
                0.05));
        }
        if (needsTlasPatch) {
            (void)enqueue(makeOperation(
                IncrementalGpuSceneOperationKind::PatchTlas,
                entry,
                "patch streamed TLAS instance " + std::to_string(entry.entityUuid),
                0.08));
        }
        if (needsDescriptorPatch) {
            (void)enqueue(makeOperation(
                IncrementalGpuSceneOperationKind::PatchDescriptors,
                entry,
                "patch streamed descriptors " + std::to_string(entry.entityUuid),
                0.04));
        }
        if (entry.resetTemporalHistory) {
            (void)enqueue(makeOperation(
                IncrementalGpuSceneOperationKind::ResetTemporalHistory,
                entry,
                "mark temporal reset " + std::to_string(entry.entityUuid),
                0.02));
        }
        if (entry.resetRestirReservoir) {
            (void)enqueue(makeOperation(
                IncrementalGpuSceneOperationKind::ResetRestirReservoir,
                entry,
                "mark ReSTIR reset " + std::to_string(entry.entityUuid),
                0.02));
        }
        if (entry.updateRestirLightCandidates) {
            (void)enqueue(makeOperation(
                IncrementalGpuSceneOperationKind::UpdateRestirLightCandidates,
                entry,
                "refresh ReSTIR light candidates " + std::to_string(entry.entityUuid),
                0.03));
        }
    }
}

bool IncrementalGpuSceneUpdateQueue::canApply(
    const Operation& operation,
    const IncrementalGpuSceneApplyBudget& budget,
    const IncrementalGpuSceneApplyFrameResult& frame,
    IncrementalGpuSceneApplyFrameResult& exhausted) const {
    if (frame.appliedOperations + 1u > budget.maxOperations) {
        exhausted.operationBudgetExhausted = true;
        return false;
    }
    if (frame.appliedMs + operation.desc.estimatedApplyMs > budget.maxApplyMs) {
        exhausted.applyBudgetExhausted = true;
        return false;
    }
    if (isTlasPatch(operation.desc.kind) && frame.appliedTlasPatches + 1u > budget.maxTlasPatches) {
        exhausted.tlasBudgetExhausted = true;
        return false;
    }
    if (isDescriptorPatch(operation.desc.kind) && frame.appliedDescriptorPatches + 1u > budget.maxDescriptorPatches) {
        exhausted.descriptorBudgetExhausted = true;
        return false;
    }
    if (isResetMask(operation.desc.kind) && frame.appliedResetMasks + 1u > budget.maxResetMasks) {
        exhausted.resetMaskBudgetExhausted = true;
        return false;
    }
    return true;
}

IncrementalGpuSceneApplyFrameResult IncrementalGpuSceneUpdateQueue::applyFrame(const IncrementalGpuSceneApplyBudget& budget) {
    IncrementalGpuSceneApplyFrameResult frame;
    frame.frameIndex = frameIndex_++;
    for (Operation& operation : operations_) {
        if (operation.state != IncrementalGpuSceneOperationState::Queued) {
            continue;
        }
        IncrementalGpuSceneApplyFrameResult exhausted = frame;
        if (!canApply(operation, budget, frame, exhausted)) {
            frame.applyBudgetExhausted = frame.applyBudgetExhausted || exhausted.applyBudgetExhausted;
            frame.operationBudgetExhausted = frame.operationBudgetExhausted || exhausted.operationBudgetExhausted;
            frame.tlasBudgetExhausted = frame.tlasBudgetExhausted || exhausted.tlasBudgetExhausted;
            frame.descriptorBudgetExhausted = frame.descriptorBudgetExhausted || exhausted.descriptorBudgetExhausted;
            frame.resetMaskBudgetExhausted = frame.resetMaskBudgetExhausted || exhausted.resetMaskBudgetExhausted;
            continue;
        }
        operation.state = IncrementalGpuSceneOperationState::Applied;
        ++frame.appliedOperations;
        frame.appliedMs += operation.desc.estimatedApplyMs;
        if (isTlasPatch(operation.desc.kind)) {
            ++frame.appliedTlasPatches;
            frame.tlasPatchEntityUuids.push_back(operation.desc.entityUuid);
            if (!operation.desc.meshGuid.empty()) {
                frame.tlasPatchMeshGuids.push_back(operation.desc.meshGuid);
            }
        }
        if (isDescriptorPatch(operation.desc.kind)) {
            ++frame.appliedDescriptorPatches;
            frame.descriptorPatchEntityUuids.push_back(operation.desc.entityUuid);
            if (!operation.desc.meshGuid.empty()) {
                frame.descriptorPatchMeshGuids.push_back(operation.desc.meshGuid);
            }
            if (operation.desc.kind == IncrementalGpuSceneOperationKind::UpdateRestirLightCandidates) {
                frame.restirLightCandidateEntityUuids.push_back(operation.desc.entityUuid);
            }
        }
        if (isResetMask(operation.desc.kind)) {
            ++frame.appliedResetMasks;
            if (operation.desc.kind == IncrementalGpuSceneOperationKind::ResetTemporalHistory) {
                frame.temporalResetEntityUuids.push_back(operation.desc.entityUuid);
            } else if (operation.desc.kind == IncrementalGpuSceneOperationKind::ResetRestirReservoir) {
                frame.restirResetEntityUuids.push_back(operation.desc.entityUuid);
            }
        }
    }
    return frame;
}

bool IncrementalGpuSceneUpdateQueue::cancelByEntity(uint64_t entityUuid) {
    bool cancelled = false;
    for (Operation& operation : operations_) {
        if (operation.desc.entityUuid == entityUuid && operation.state == IncrementalGpuSceneOperationState::Queued) {
            operation.state = IncrementalGpuSceneOperationState::Cancelled;
            cancelled = true;
        }
    }
    return cancelled;
}

bool IncrementalGpuSceneUpdateQueue::empty() const {
    return std::all_of(operations_.begin(), operations_.end(), [](const Operation& operation) {
        return terminal(operation.state);
    });
}

std::vector<IncrementalGpuSceneUpdateSnapshot> IncrementalGpuSceneUpdateQueue::snapshots() const {
    std::vector<IncrementalGpuSceneUpdateSnapshot> out;
    out.reserve(operations_.size());
    for (const Operation& operation : operations_) {
        out.push_back({
            .id = operation.id,
            .kind = operation.desc.kind,
            .state = operation.state,
            .entityUuid = operation.desc.entityUuid,
            .gpuInstanceIndex = operation.desc.gpuInstanceIndex,
            .meshGuid = operation.desc.meshGuid,
            .label = operation.desc.label,
            .estimatedApplyMs = operation.desc.estimatedApplyMs,
            .requiresRenderThread = operation.desc.requiresRenderThread,
        });
    }
    return out;
}

IncrementalGpuSceneUpdateStats IncrementalGpuSceneUpdateQueue::stats() const {
    IncrementalGpuSceneUpdateStats out;
    for (const Operation& operation : operations_) {
        if (operation.state == IncrementalGpuSceneOperationState::Queued) {
            ++out.queued;
            if (isTlasPatch(operation.desc.kind)) {
                ++out.tlasPatchesQueued;
            }
            if (isDescriptorPatch(operation.desc.kind)) {
                ++out.descriptorPatchesQueued;
            }
            if (operation.desc.kind == IncrementalGpuSceneOperationKind::UpdateRestirLightCandidates) {
                ++out.restirLightCandidateUpdatesQueued;
            }
            if (isResetMask(operation.desc.kind)) {
                ++out.resetMasksQueued;
            }
        } else if (operation.state == IncrementalGpuSceneOperationState::Applied) {
            ++out.applied;
            if (operation.desc.kind == IncrementalGpuSceneOperationKind::ResetTemporalHistory) {
                ++out.temporalResetsApplied;
            }
            if (operation.desc.kind == IncrementalGpuSceneOperationKind::ResetRestirReservoir) {
                ++out.restirResetsApplied;
            }
            if (operation.desc.kind == IncrementalGpuSceneOperationKind::UpdateRestirLightCandidates) {
                ++out.restirLightCandidateUpdatesApplied;
            }
        } else if (operation.state == IncrementalGpuSceneOperationState::Cancelled) {
            ++out.cancelled;
        }
    }
    return out;
}

nlohmann::json incrementalGpuSceneApplyFrameResultJson(const IncrementalGpuSceneApplyFrameResult& frame) {
    return {
        {"frame_index", frame.frameIndex},
        {"applied_operations", frame.appliedOperations},
        {"applied_tlas_patches", frame.appliedTlasPatches},
        {"applied_descriptor_patches", frame.appliedDescriptorPatches},
        {"applied_reset_masks", frame.appliedResetMasks},
        {"applied_ms", frame.appliedMs},
        {"temporal_reset_entity_uuids", frame.temporalResetEntityUuids},
        {"restir_reset_entity_uuids", frame.restirResetEntityUuids},
        {"restir_light_candidate_entity_uuids", frame.restirLightCandidateEntityUuids},
        {"tlas_patch_entity_uuids", frame.tlasPatchEntityUuids},
        {"tlas_patch_mesh_guids", frame.tlasPatchMeshGuids},
        {"descriptor_patch_entity_uuids", frame.descriptorPatchEntityUuids},
        {"descriptor_patch_mesh_guids", frame.descriptorPatchMeshGuids},
        {"apply_budget_exhausted", frame.applyBudgetExhausted},
        {"operation_budget_exhausted", frame.operationBudgetExhausted},
        {"tlas_budget_exhausted", frame.tlasBudgetExhausted},
        {"descriptor_budget_exhausted", frame.descriptorBudgetExhausted},
        {"reset_mask_budget_exhausted", frame.resetMaskBudgetExhausted},
    };
}

nlohmann::json incrementalGpuSceneUpdateStatsJson(const IncrementalGpuSceneUpdateStats& stats) {
    return {
        {"queued", stats.queued},
        {"applied", stats.applied},
        {"cancelled", stats.cancelled},
        {"tlas_patches_queued", stats.tlasPatchesQueued},
        {"descriptor_patches_queued", stats.descriptorPatchesQueued},
        {"reset_masks_queued", stats.resetMasksQueued},
        {"restir_light_candidate_updates_queued", stats.restirLightCandidateUpdatesQueued},
        {"temporal_resets_applied", stats.temporalResetsApplied},
        {"restir_resets_applied", stats.restirResetsApplied},
        {"restir_light_candidate_updates_applied", stats.restirLightCandidateUpdatesApplied},
    };
}

nlohmann::json incrementalGpuSceneUpdateSnapshotsJson(const std::vector<IncrementalGpuSceneUpdateSnapshot>& snapshots) {
    nlohmann::json out = nlohmann::json::array();
    for (const IncrementalGpuSceneUpdateSnapshot& snapshot : snapshots) {
        out.push_back({
            {"id", snapshot.id},
            {"kind", incrementalGpuSceneOperationKindName(snapshot.kind)},
            {"state", incrementalGpuSceneOperationStateName(snapshot.state)},
            {"entity_uuid", snapshot.entityUuid},
            {"gpu_instance_index", snapshot.gpuInstanceIndex == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(snapshot.gpuInstanceIndex)},
            {"mesh_guid", snapshot.meshGuid},
            {"label", snapshot.label},
            {"estimated_apply_ms", snapshot.estimatedApplyMs},
            {"requires_render_thread", snapshot.requiresRenderThread},
        });
    }
    return out;
}

int simulateIncrementalGpuSceneUpdateQueueCommand(
    uint32_t instanceCount,
    const IncrementalGpuSceneApplyBudget& budget,
    uint32_t cancelAfterFrame,
    const std::filesystem::path& jsonOut) {
    SceneDocument document;
    NativeGpuAssetCache cache;
    std::vector<EntityId> instanceEntities;
    instanceEntities.reserve(instanceCount);
    for (uint32_t i = 0; i < instanceCount; ++i) {
        EntityId entityId = document.registry().createEntity("incremental gpu instance " + std::to_string(i));
        Entity* entity = document.registry().entity(entityId);
        if (entity == nullptr) {
            continue;
        }
        MeshRenderer renderer;
        renderer.meshGuid = "incremental-mesh-" + std::to_string(i);
        renderer.mesh = MeshAssetHandle{i};
        MaterialSlot slot;
        slot.materialGuid = "incremental-material-" + std::to_string(i);
        slot.material = MaterialAssetHandle{i};
        renderer.materialSlots.push_back(slot);
        entity->meshRenderer = renderer;
        instanceEntities.push_back(entityId);

        cache.upsert(NativeGpuAssetDesc{
            .kind = NativeGpuAssetKind::Mesh,
            .guid = renderer.meshGuid,
            .label = renderer.meshGuid,
            .gpuBytes = 8ull * 1024ull * 1024ull,
            .fallbackDescriptorBound = true,
        });
        cache.upsert(NativeGpuAssetDesc{
            .kind = NativeGpuAssetKind::Material,
            .guid = slot.materialGuid,
            .label = slot.materialGuid,
            .gpuBytes = 4ull * 1024ull,
            .fallbackDescriptorBound = true,
            .restirLightCandidate = (i % 4u) == 1u,
        });
        if ((i % 7u) != 0u) {
            (void)cache.markResident(renderer.meshGuid);
            (void)cache.markBlasBuildQueued(renderer.meshGuid, 1000ull + i);
            (void)cache.markTlasPatchQueued(renderer.meshGuid, 2000ull + i);
        }
        if ((i % 5u) != 0u) {
            (void)cache.markResident(slot.materialGuid);
        }
    }

    GpuSceneStreamingState state;
    state.rebuild(document, instanceEntities, &cache);
    const std::vector<GpuSceneStreamingInstanceSnapshot> before = state.instances();
    for (uint32_t i = 0; i < instanceCount; ++i) {
        const AssetGuid meshGuid = "incremental-mesh-" + std::to_string(i);
        const AssetGuid materialGuid = "incremental-material-" + std::to_string(i);
        (void)cache.markResident(meshGuid);
        (void)cache.markBlasReady(meshGuid);
        (void)cache.markTlasVisible(meshGuid);
        (void)cache.markResident(materialGuid);
    }
    state.rebuild(document, instanceEntities, &cache);
    const GpuSceneStreamingUpdatePlan plan = buildGpuSceneStreamingUpdatePlan(before, state.instances());

    IncrementalGpuSceneUpdateQueue queue;
    queue.enqueueUpdatePlan(plan);
    nlohmann::json frames = nlohmann::json::array();
    constexpr uint32_t kMaxFrames = 512;
    for (uint32_t frame = 0; frame < kMaxFrames && !queue.empty(); ++frame) {
        if (cancelAfterFrame != 0u && frame == cancelAfterFrame && !plan.entries.empty()) {
            (void)queue.cancelByEntity(plan.entries.front().entityUuid);
        }
        frames.push_back(incrementalGpuSceneApplyFrameResultJson(queue.applyFrame(budget)));
    }

    const IncrementalGpuSceneUpdateStats stats = queue.stats();
    const nlohmann::json report = {
        {"schema", "IncrementalGpuSceneUpdateQueueSimulationV1"},
        {"ok", queue.empty()},
        {"instance_count", instanceCount},
        {"budget", {
            {"max_apply_ms", budget.maxApplyMs},
            {"max_operations", budget.maxOperations},
            {"max_tlas_patches", budget.maxTlasPatches},
            {"max_descriptor_patches", budget.maxDescriptorPatches},
            {"max_reset_masks", budget.maxResetMasks},
        }},
        {"cancel_after_frame", cancelAfterFrame},
        {"source_update_plan", gpuSceneStreamingUpdatePlanJson(plan)},
        {"frames", frames},
        {"stats", incrementalGpuSceneUpdateStatsJson(stats)},
        {"operations", incrementalGpuSceneUpdateSnapshotsJson(queue.snapshots())},
    };

    try {
        writeReportOrStdout(report, jsonOut);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return report.value("ok", false) ? 0 : 1;
}

} // namespace rtv
