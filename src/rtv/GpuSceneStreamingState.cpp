#include "rtv/GpuSceneStreamingState.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <unordered_map>

namespace rtv {
namespace {

using CacheSnapshotMap = std::unordered_map<AssetGuid, NativeGpuAssetSnapshot>;

CacheSnapshotMap makeCacheSnapshotMap(const NativeGpuAssetCache* cache) {
    CacheSnapshotMap out;
    if (cache == nullptr) {
        return out;
    }
    for (const NativeGpuAssetSnapshot& snapshot : cache->snapshots()) {
        if (!snapshot.guid.empty()) {
            out[snapshot.guid] = snapshot;
        }
    }
    return out;
}

const NativeGpuAssetSnapshot* cacheSnapshotFor(const CacheSnapshotMap& cache, const AssetGuid& guid) {
    if (guid.empty()) {
        return nullptr;
    }
    const auto it = cache.find(guid);
    return it == cache.end() ? nullptr : &it->second;
}

bool cacheEntryResident(const CacheSnapshotMap& cache, const AssetGuid& guid) {
    const NativeGpuAssetSnapshot* snapshot = cacheSnapshotFor(cache, guid);
    return snapshot != nullptr && snapshot->residency == NativeGpuAssetResidency::Resident;
}

bool cacheEntryMeshPayloadReady(const CacheSnapshotMap& cache, const AssetGuid& guid) {
    const NativeGpuAssetSnapshot* snapshot = cacheSnapshotFor(cache, guid);
    return snapshot != nullptr &&
        snapshot->kind == NativeGpuAssetKind::Mesh &&
        (snapshot->residency == NativeGpuAssetResidency::Resident ||
         snapshot->blasReady ||
         snapshot->tlasVisible);
}

bool cacheEntryBlasReady(const CacheSnapshotMap& cache, const AssetGuid& guid) {
    const NativeGpuAssetSnapshot* snapshot = cacheSnapshotFor(cache, guid);
    return snapshot != nullptr && snapshot->blasReady;
}

bool cacheEntryTlasVisible(const CacheSnapshotMap& cache, const AssetGuid& guid) {
    const NativeGpuAssetSnapshot* snapshot = cacheSnapshotFor(cache, guid);
    return snapshot != nullptr && snapshot->tlasVisible;
}

bool cacheEntryFailed(const CacheSnapshotMap& cache, const AssetGuid& guid) {
    const NativeGpuAssetSnapshot* snapshot = cacheSnapshotFor(cache, guid);
    return snapshot != nullptr && snapshot->residency == NativeGpuAssetResidency::Failed;
}

bool cacheEntryUsesFallback(const CacheSnapshotMap& cache, const AssetGuid& guid) {
    const NativeGpuAssetSnapshot* snapshot = cacheSnapshotFor(cache, guid);
    return snapshot == nullptr || snapshot->fallbackDescriptorBound;
}

uint32_t countPendingMaterialSlots(const MeshRenderer& renderer, const CacheSnapshotMap& cache) {
    uint32_t pending = 0;
    for (const MaterialSlot& slot : renderer.materialSlots) {
        const AssetGuid& guid = slot.overrideMaterialGuid.value_or(slot.materialGuid);
        if (guid.empty()) {
            continue;
        }
        if (!slot.resolvedMaterial().valid() || !cacheEntryResident(cache, guid)) {
            ++pending;
        }
    }
    return pending;
}

bool anyMaterialFailed(const MeshRenderer& renderer, const CacheSnapshotMap& cache) {
    for (const MaterialSlot& slot : renderer.materialSlots) {
        const AssetGuid& guid = slot.overrideMaterialGuid.value_or(slot.materialGuid);
        if (cacheEntryFailed(cache, guid)) {
            return true;
        }
    }
    return false;
}

bool anyMaterialFallback(const MeshRenderer& renderer, const CacheSnapshotMap& cache) {
    for (const MaterialSlot& slot : renderer.materialSlots) {
        const AssetGuid& guid = slot.overrideMaterialGuid.value_or(slot.materialGuid);
        if (!guid.empty() && cacheEntryUsesFallback(cache, guid)) {
            return true;
        }
    }
    return false;
}

bool anyMaterialRestirLightCandidate(const MeshRenderer& renderer, const CacheSnapshotMap& cache) {
    for (const MaterialSlot& slot : renderer.materialSlots) {
        const AssetGuid& guid = slot.overrideMaterialGuid.value_or(slot.materialGuid);
        const NativeGpuAssetSnapshot* snapshot = cacheSnapshotFor(cache, guid);
        if (snapshot != nullptr &&
            snapshot->kind == NativeGpuAssetKind::Material &&
            snapshot->residency == NativeGpuAssetResidency::Resident &&
            snapshot->restirLightCandidate) {
            return true;
        }
    }
    return false;
}

GpuSceneStreamingInstanceSnapshot makeSnapshot(
    uint32_t gpuInstanceIndex,
    const Entity& entity,
    const MeshRenderer& renderer,
    const CacheSnapshotMap& cache) {
    GpuSceneStreamingInstanceSnapshot snapshot;
    snapshot.gpuInstanceIndex = gpuInstanceIndex;
    snapshot.entity = entity.id;
    snapshot.entityUuid = entity.uuid;
    snapshot.entityName = entity.name;
    snapshot.meshGuid = renderer.meshGuid;
    snapshot.hasLiveMeshHandle = renderer.mesh.valid();
    snapshot.visible = entity.visible && renderer.visible;
    snapshot.castShadow = renderer.castShadow;
    snapshot.visibleToCamera = renderer.visibleToCamera;
    snapshot.cacheResident = cacheEntryMeshPayloadReady(cache, renderer.meshGuid);
    snapshot.blasReady = cacheEntryBlasReady(cache, renderer.meshGuid);
    snapshot.tlasVisible = cacheEntryTlasVisible(cache, renderer.meshGuid);
    snapshot.fallbackDescriptors = cacheEntryUsesFallback(cache, renderer.meshGuid) || anyMaterialFallback(renderer, cache);
    snapshot.restirLightCandidate = anyMaterialRestirLightCandidate(renderer, cache);
    snapshot.materialSlotCount = static_cast<uint32_t>(std::min<size_t>(renderer.materialSlots.size(), UINT32_MAX));
    snapshot.pendingMaterialSlots = countPendingMaterialSlots(renderer, cache);

    if (!snapshot.visible) {
        snapshot.state = GpuSceneStreamingInstanceState::Disabled;
    } else if (cacheEntryFailed(cache, renderer.meshGuid) || anyMaterialFailed(renderer, cache)) {
        snapshot.state = GpuSceneStreamingInstanceState::Failed;
    } else if (!renderer.meshGuid.empty() && !renderer.mesh.valid()) {
        snapshot.state = GpuSceneStreamingInstanceState::PendingMesh;
    } else if (!renderer.mesh.valid()) {
        snapshot.state = GpuSceneStreamingInstanceState::MetadataOnly;
    } else if (!snapshot.cacheResident) {
        snapshot.state = GpuSceneStreamingInstanceState::PendingMesh;
    } else if (!snapshot.blasReady) {
        snapshot.state = GpuSceneStreamingInstanceState::PendingBlas;
    } else if (!snapshot.tlasVisible) {
        snapshot.state = GpuSceneStreamingInstanceState::PendingTlas;
    } else if (snapshot.pendingMaterialSlots > 0) {
        snapshot.state = GpuSceneStreamingInstanceState::PendingMaterials;
    } else {
        snapshot.state = GpuSceneStreamingInstanceState::Resident;
        snapshot.fallbackDescriptors = false;
    }
    snapshot.renderable = snapshot.state == GpuSceneStreamingInstanceState::Resident;
    return snapshot;
}

nlohmann::json instanceJson(const GpuSceneStreamingInstanceSnapshot& snapshot) {
    return {
        {"gpu_instance_index", snapshot.gpuInstanceIndex},
        {"entity_index", snapshot.entity.index},
        {"entity_generation", snapshot.entity.generation},
        {"entity_uuid", snapshot.entityUuid},
        {"entity_name", snapshot.entityName},
        {"mesh_guid", snapshot.meshGuid},
        {"state", gpuSceneStreamingInstanceStateName(snapshot.state)},
        {"has_live_mesh_handle", snapshot.hasLiveMeshHandle},
        {"visible", snapshot.visible},
        {"cast_shadow", snapshot.castShadow},
        {"visible_to_camera", snapshot.visibleToCamera},
        {"cache_resident", snapshot.cacheResident},
        {"blas_ready", snapshot.blasReady},
        {"tlas_visible", snapshot.tlasVisible},
        {"renderable", snapshot.renderable},
        {"fallback_descriptors", snapshot.fallbackDescriptors},
        {"restir_light_candidate", snapshot.restirLightCandidate},
        {"material_slot_count", snapshot.materialSlotCount},
        {"pending_material_slots", snapshot.pendingMaterialSlots},
    };
}

} // namespace

const char* gpuSceneStreamingInstanceStateName(GpuSceneStreamingInstanceState state) {
    switch (state) {
    case GpuSceneStreamingInstanceState::MetadataOnly: return "metadata_only";
    case GpuSceneStreamingInstanceState::PendingMesh: return "pending_mesh";
    case GpuSceneStreamingInstanceState::PendingBlas: return "pending_blas";
    case GpuSceneStreamingInstanceState::PendingTlas: return "pending_tlas";
    case GpuSceneStreamingInstanceState::PendingMaterials: return "pending_materials";
    case GpuSceneStreamingInstanceState::Resident: return "resident";
    case GpuSceneStreamingInstanceState::Disabled: return "disabled";
    case GpuSceneStreamingInstanceState::Failed: return "failed";
    }
    return "metadata_only";
}

GpuSceneStreamingUpdatePlan buildGpuSceneStreamingUpdatePlan(
    const std::vector<GpuSceneStreamingInstanceSnapshot>& previous,
    const std::vector<GpuSceneStreamingInstanceSnapshot>& current) {
    std::unordered_map<uint64_t, const GpuSceneStreamingInstanceSnapshot*> previousByUuid;
    previousByUuid.reserve(previous.size());
    for (const GpuSceneStreamingInstanceSnapshot& snapshot : previous) {
        if (snapshot.entityUuid != 0) {
            previousByUuid[snapshot.entityUuid] = &snapshot;
        }
    }

    std::unordered_map<uint64_t, const GpuSceneStreamingInstanceSnapshot*> currentByUuid;
    currentByUuid.reserve(current.size());
    for (const GpuSceneStreamingInstanceSnapshot& snapshot : current) {
        if (snapshot.entityUuid != 0) {
            currentByUuid[snapshot.entityUuid] = &snapshot;
        }
    }

    GpuSceneStreamingUpdatePlan plan;
    auto appendEntry = [&](GpuSceneStreamingUpdatePlanEntry entry) {
        if (entry.added) {
            ++plan.addedInstances;
        }
        if (entry.removed) {
            ++plan.removedInstances;
        }
        if (!entry.added && !entry.removed) {
            ++plan.changedInstances;
        }
        if (entry.becameRenderable) {
            ++plan.becameRenderableInstances;
        }
        if (entry.becameNonRenderable) {
            ++plan.becameNonRenderableInstances;
        }
        if (entry.resetTemporalHistory) {
            ++plan.temporalResetInstances;
        }
        if (entry.resetRestirReservoir) {
            ++plan.restirResetInstances;
        }
        if (entry.updateRestirLightCandidates) {
            ++plan.restirLightCandidateUpdateInstances;
        }
        plan.entries.push_back(std::move(entry));
    };

    for (const GpuSceneStreamingInstanceSnapshot& snapshot : current) {
        const auto prevIt = previousByUuid.find(snapshot.entityUuid);
        const GpuSceneStreamingInstanceSnapshot* prev = prevIt == previousByUuid.end() ? nullptr : prevIt->second;
        if (prev == nullptr) {
            appendEntry(GpuSceneStreamingUpdatePlanEntry{
                .entityUuid = snapshot.entityUuid,
                .currentGpuInstanceIndex = snapshot.gpuInstanceIndex,
                .entityName = snapshot.entityName,
                .meshGuid = snapshot.meshGuid,
                .currentState = snapshot.state,
                .added = true,
                .renderabilityChanged = snapshot.renderable,
                .becameRenderable = snapshot.renderable,
                .resetTemporalHistory = snapshot.renderable,
                .resetRestirReservoir = snapshot.renderable,
                .updateRestirLightCandidates = snapshot.renderable && snapshot.restirLightCandidate,
            });
            continue;
        }
        const bool stateChanged = prev->state != snapshot.state;
        const bool renderabilityChanged = prev->renderable != snapshot.renderable;
        const bool blasTlasChanged = prev->blasReady != snapshot.blasReady || prev->tlasVisible != snapshot.tlasVisible;
        const bool materialsChanged = prev->pendingMaterialSlots != snapshot.pendingMaterialSlots ||
            prev->fallbackDescriptors != snapshot.fallbackDescriptors;
        const bool restirLightCandidateChanged = prev->restirLightCandidate != snapshot.restirLightCandidate;
        const bool materialSignalChanged = materialsChanged || restirLightCandidateChanged;
        if (!stateChanged && !renderabilityChanged && !blasTlasChanged && !materialSignalChanged) {
            continue;
        }
        appendEntry(GpuSceneStreamingUpdatePlanEntry{
            .entityUuid = snapshot.entityUuid,
            .previousGpuInstanceIndex = prev->gpuInstanceIndex,
            .currentGpuInstanceIndex = snapshot.gpuInstanceIndex,
            .entityName = snapshot.entityName,
            .meshGuid = snapshot.meshGuid,
            .previousState = prev->state,
            .currentState = snapshot.state,
            .renderabilityChanged = renderabilityChanged,
            .becameRenderable = !prev->renderable && snapshot.renderable,
            .becameNonRenderable = prev->renderable && !snapshot.renderable,
            .resetTemporalHistory = (!prev->renderable && snapshot.renderable) || materialSignalChanged,
            .resetRestirReservoir = (!prev->renderable && snapshot.renderable) || materialSignalChanged,
            .updateRestirLightCandidates = snapshot.renderable &&
                (restirLightCandidateChanged || (!prev->renderable && snapshot.restirLightCandidate)),
        });
    }

    for (const GpuSceneStreamingInstanceSnapshot& snapshot : previous) {
        if (currentByUuid.find(snapshot.entityUuid) != currentByUuid.end()) {
            continue;
        }
        appendEntry(GpuSceneStreamingUpdatePlanEntry{
            .entityUuid = snapshot.entityUuid,
            .previousGpuInstanceIndex = snapshot.gpuInstanceIndex,
            .entityName = snapshot.entityName,
            .meshGuid = snapshot.meshGuid,
            .previousState = snapshot.state,
            .removed = true,
            .renderabilityChanged = snapshot.renderable,
            .becameNonRenderable = snapshot.renderable,
            .resetTemporalHistory = snapshot.renderable,
            .resetRestirReservoir = snapshot.renderable,
            .updateRestirLightCandidates = snapshot.renderable && snapshot.restirLightCandidate,
        });
    }

    std::sort(plan.entries.begin(), plan.entries.end(), [](const GpuSceneStreamingUpdatePlanEntry& a, const GpuSceneStreamingUpdatePlanEntry& b) {
        return a.entityUuid < b.entityUuid;
    });
    return plan;
}

void GpuSceneStreamingState::rebuild(
    const SceneDocument& document,
    const std::vector<EntityId>& gpuInstanceEntities,
    const NativeGpuAssetCache* cache) {
    instances_.clear();
    instances_.reserve(gpuInstanceEntities.size());
    const CacheSnapshotMap cacheSnapshots = makeCacheSnapshotMap(cache);
    for (uint32_t i = 0; i < gpuInstanceEntities.size(); ++i) {
        const Entity* entity = document.registry().entity(gpuInstanceEntities[i]);
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            continue;
        }
        instances_.push_back(makeSnapshot(i, *entity, *entity->meshRenderer, cacheSnapshots));
    }
}

GpuSceneStreamingStats GpuSceneStreamingState::stats() const {
    GpuSceneStreamingStats out;
    out.instanceCount = static_cast<uint32_t>(std::min<size_t>(instances_.size(), UINT32_MAX));
    for (const GpuSceneStreamingInstanceSnapshot& instance : instances_) {
        if (instance.fallbackDescriptors) {
            ++out.fallbackDescriptorInstances;
        }
        if (instance.restirLightCandidate) {
            ++out.restirLightCandidateInstances;
        }
        if (instance.renderable) {
            ++out.renderableInstances;
        }
        switch (instance.state) {
        case GpuSceneStreamingInstanceState::MetadataOnly: ++out.metadataOnlyInstances; break;
        case GpuSceneStreamingInstanceState::PendingMesh: ++out.pendingMeshInstances; break;
        case GpuSceneStreamingInstanceState::PendingBlas: ++out.pendingBlasInstances; break;
        case GpuSceneStreamingInstanceState::PendingTlas: ++out.pendingTlasInstances; break;
        case GpuSceneStreamingInstanceState::PendingMaterials: ++out.pendingMaterialInstances; break;
        case GpuSceneStreamingInstanceState::Resident: ++out.residentInstances; break;
        case GpuSceneStreamingInstanceState::Disabled: ++out.disabledInstances; break;
        case GpuSceneStreamingInstanceState::Failed: ++out.failedInstances; break;
        }
    }
    return out;
}

nlohmann::json gpuSceneStreamingStatsJson(const GpuSceneStreamingStats& stats) {
    return {
        {"instance_count", stats.instanceCount},
        {"metadata_only_instances", stats.metadataOnlyInstances},
        {"pending_mesh_instances", stats.pendingMeshInstances},
        {"pending_blas_instances", stats.pendingBlasInstances},
        {"pending_tlas_instances", stats.pendingTlasInstances},
        {"pending_material_instances", stats.pendingMaterialInstances},
        {"resident_instances", stats.residentInstances},
        {"disabled_instances", stats.disabledInstances},
        {"failed_instances", stats.failedInstances},
        {"renderable_instances", stats.renderableInstances},
        {"fallback_descriptor_instances", stats.fallbackDescriptorInstances},
        {"restir_light_candidate_instances", stats.restirLightCandidateInstances},
    };
}

nlohmann::json gpuSceneStreamingInstancesJson(const std::vector<GpuSceneStreamingInstanceSnapshot>& instances) {
    nlohmann::json out = nlohmann::json::array();
    for (const GpuSceneStreamingInstanceSnapshot& instance : instances) {
        out.push_back(instanceJson(instance));
    }
    return out;
}

nlohmann::json gpuSceneStreamingUpdatePlanJson(const GpuSceneStreamingUpdatePlan& plan) {
    nlohmann::json entries = nlohmann::json::array();
    for (const GpuSceneStreamingUpdatePlanEntry& entry : plan.entries) {
        entries.push_back({
            {"entity_uuid", entry.entityUuid},
            {"previous_gpu_instance_index", entry.previousGpuInstanceIndex == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(entry.previousGpuInstanceIndex)},
            {"current_gpu_instance_index", entry.currentGpuInstanceIndex == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(entry.currentGpuInstanceIndex)},
            {"entity_name", entry.entityName},
            {"mesh_guid", entry.meshGuid},
            {"previous_state", gpuSceneStreamingInstanceStateName(entry.previousState)},
            {"current_state", gpuSceneStreamingInstanceStateName(entry.currentState)},
            {"added", entry.added},
            {"removed", entry.removed},
            {"renderability_changed", entry.renderabilityChanged},
            {"became_renderable", entry.becameRenderable},
            {"became_non_renderable", entry.becameNonRenderable},
            {"reset_temporal_history", entry.resetTemporalHistory},
            {"reset_restir_reservoir", entry.resetRestirReservoir},
            {"update_restir_light_candidates", entry.updateRestirLightCandidates},
        });
    }
    return {
        {"added_instances", plan.addedInstances},
        {"removed_instances", plan.removedInstances},
        {"changed_instances", plan.changedInstances},
        {"became_renderable_instances", plan.becameRenderableInstances},
        {"became_non_renderable_instances", plan.becameNonRenderableInstances},
        {"temporal_reset_instances", plan.temporalResetInstances},
        {"restir_reset_instances", plan.restirResetInstances},
        {"restir_light_candidate_update_instances", plan.restirLightCandidateUpdateInstances},
        {"entries", entries},
    };
}

int simulateGpuSceneStreamingStateCommand(uint32_t instanceCount, const std::filesystem::path& jsonOut) {
    SceneDocument document;
    NativeGpuAssetCache cache;
    std::vector<EntityId> instanceEntities;
    instanceEntities.reserve(instanceCount);
    for (uint32_t i = 0; i < instanceCount; ++i) {
        EntityId entityId = document.registry().createEntity("streaming instance " + std::to_string(i));
        Entity* entity = document.registry().entity(entityId);
        if (entity == nullptr) {
            continue;
        }
        MeshRenderer renderer;
        renderer.meshGuid = "mesh-guid-" + std::to_string(i);
        renderer.mesh = (i % 4u) == 0u ? MeshAssetHandle{} : MeshAssetHandle{i};
        renderer.visible = (i % 7u) != 0u;
        MaterialSlot slot;
        slot.materialGuid = "material-guid-" + std::to_string(i);
        slot.material = (i % 5u) == 0u ? MaterialAssetHandle{} : MaterialAssetHandle{i};
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
        if ((i % 6u) == 0u) {
            (void)cache.markFailed(renderer.meshGuid);
        } else if ((i % 3u) != 0u) {
            (void)cache.markResident(renderer.meshGuid);
            (void)cache.markBlasBuildQueued(renderer.meshGuid, 1000ull + i);
            (void)cache.markTlasPatchQueued(renderer.meshGuid, 2000ull + i);
            if ((i % 8u) != 1u) {
                (void)cache.markBlasReady(renderer.meshGuid);
            }
            if ((i % 8u) != 2u) {
                (void)cache.markTlasVisible(renderer.meshGuid);
            }
        }
        if ((i % 5u) != 0u) {
            (void)cache.markResident(slot.materialGuid);
        }
    }

    GpuSceneStreamingState state;
    state.rebuild(document, instanceEntities, &cache);
    const std::vector<GpuSceneStreamingInstanceSnapshot> beforeInstances = state.instances();
    for (uint32_t i = 0; i < instanceCount; ++i) {
        if ((i % 11u) == 0u && i != 0u) {
            const AssetGuid materialGuid = "material-guid-" + std::to_string(i);
            cache.upsert(NativeGpuAssetDesc{
                .kind = NativeGpuAssetKind::Material,
                .guid = materialGuid,
                .label = materialGuid,
                .gpuBytes = 4ull * 1024ull,
                .fallbackDescriptorBound = true,
                .restirLightCandidate = true,
            });
            (void)cache.markResident(materialGuid);
        }
        if ((i % 6u) == 0u || (i % 3u) == 0u) {
            continue;
        }
        const AssetGuid guid = "mesh-guid-" + std::to_string(i);
        (void)cache.markBlasReady(guid);
        (void)cache.markTlasVisible(guid);
        (void)cache.markResident(guid);
        const AssetGuid materialGuid = "material-guid-" + std::to_string(i);
        (void)cache.markResident(materialGuid);
    }
    state.rebuild(document, instanceEntities, &cache);
    const GpuSceneStreamingUpdatePlan updatePlan = buildGpuSceneStreamingUpdatePlan(beforeInstances, state.instances());
    const GpuSceneStreamingStats stats = state.stats();
    const nlohmann::json report = {
        {"schema", "GpuSceneStreamingStateSimulationV1"},
        {"ok", stats.instanceCount == instanceEntities.size()},
        {"stats", gpuSceneStreamingStatsJson(stats)},
        {"update_plan", gpuSceneStreamingUpdatePlanJson(updatePlan)},
        {"instances", gpuSceneStreamingInstancesJson(state.instances())},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Could not create GPU scene streaming state report directory: " << parent.string() << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Could not write GPU scene streaming state report: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return report.value("ok", false) ? 0 : 1;
}

} // namespace rtv
