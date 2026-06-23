#include "rtv/HeadlessDiagnostics.h"

#include "rtv/Application.h"
#include "rtv/BufferUploader.h"
#include "rtv/DiagnosticImageExport.h"
#include "rtv/GpuProfiler.h"
#include "rtv/GpuValidation.h"
#include "rtv/OpacityMicromapPreprocess.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/RenderGraphDump.h"
#include "rtv/RenderGraph.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/Swapchain.h"
#include "rtv/UiOverlay.h"
#include "rtv/VulkanContext.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rtv {

namespace {

bool isRestirGiExportView(RendererDebugView view) {
    return view == RendererDebugView::RestirGiValidity ||
        view == RendererDebugView::RestirGiAge ||
        view == RendererDebugView::RestirGiInitial ||
        view == RendererDebugView::RestirGiTemporal ||
        view == RendererDebugView::RestirGiSpatial ||
        view == RendererDebugView::RestirGiFinal ||
        view == RendererDebugView::RestirGiNormal ||
        view == RendererDebugView::RestirGiHitDistance ||
        view == RendererDebugView::RestirGiGrid ||
        view == RendererDebugView::RestirGiPathClass ||
        view == RendererDebugView::WavefrontRestirGi;
}

bool isRestirDiExportView(RendererDebugView view) {
    return view == RendererDebugView::RestirDiSelectedLight ||
        view == RendererDebugView::RestirDiTarget ||
        view == RendererDebugView::RestirDiSourcePdf ||
        view == RendererDebugView::RestirDiVisibility ||
        view == RendererDebugView::RestirDiRejectionReason ||
        view == RendererDebugView::RestirDiTemporalAcceptance ||
        view == RendererDebugView::RestirDiSpatialAcceptance ||
        view == RendererDebugView::RestirDiFinalContribution ||
        view == RendererDebugView::RestirDiReceiverPosition ||
        view == RendererDebugView::RestirDiReceiverNormal ||
        view == RendererDebugView::RestirDiLightVersion ||
        view == RendererDebugView::RestirDiInitialReservoir ||
        view == RendererDebugView::RestirDiTemporalReservoir ||
        view == RendererDebugView::RestirDiSpatialReservoir ||
        view == RendererDebugView::RestirDiFinalReservoir ||
        view == RendererDebugView::RestirDiWeightSum ||
        view == RendererDebugView::RestirDiM ||
        view == RendererDebugView::RestirDiLightClass ||
        view == RendererDebugView::RestirDiAge ||
        view == RendererDebugView::RestirDiConfidence ||
        view == RendererDebugView::RestirDiReferenceDiff ||
        view == RendererDebugView::WavefrontRestirDi;
}

bool shouldExportDebugViewForSettings(const RendererSettings& settings, RendererDebugView view) {
    if (settings.restirGiMode == RestirGiMode::Off && isRestirGiExportView(view)) {
        return false;
    }
    if (settings.restirDiMode == RestirDiMode::Off && isRestirDiExportView(view)) {
        return false;
    }
    return true;
}

} // namespace

void to_json(nlohmann::json& j, const ProfileReport::Resolution& r) {
    j["render_extent"] = {{"width", r.renderWidth}, {"height", r.renderHeight}};
    j["display_extent"] = {{"width", r.displayWidth}, {"height", r.displayHeight}};
    j["render_scale"] = r.renderScale;
}

void to_json(nlohmann::json& j, const ProfileReport::MinMaxAvg& m) {
    j["min"] = m.min;
    j["avg"] = m.avg;
    j["max"] = m.max;
    j["p95"] = m.p95;
    j["p99"] = m.p99;
}

void to_json(nlohmann::json& j, const ProfileReport::PerPassGpuMs& p) {
    j["path_trace"] = p.pathTrace;
    j["restir_history_clear"] = p.restirHistoryClear;
    j["restir_gi_clear"] = p.restirGiClear;
    j["restir_gi_temporal"] = p.restirGiTemporal;
    j["restir_spatial"] = p.restirSpatial;
    j["restir_spatial_copy"] = p.restirSpatialCopy;
    j["restir_gi_spatial"] = p.restirGiSpatial;
    j["restir_gi_upsample"] = p.restirGiUpsample;
    j["restir_gi_final"] = p.restirGiFinal;
    j["restir_gi_counters_readback"] = p.restirGiCountersReadback;
    j["restir_di_temporal"] = p.restirDiTemporal;
    j["restir_di_spatial"] = p.restirDiSpatial;
    j["restir_di_final"] = p.restirDiFinal;
    j["restir_di_history_copy"] = p.restirDiHistoryCopy;
    j["restir_di_counters_readback"] = p.restirDiCountersReadback;
    j["fog_integrate"] = p.fogIntegrate;
    j["atmosphere"] = p.atmosphere;
    j["atmosphere_transmittance"] = p.atmosphereTransmittance;
    j["atmosphere_multi_scatter"] = p.atmosphereMultiScatter;
    j["atmosphere_sky_view"] = p.atmosphereSkyView;
    j["atmosphere_sky_reproject"] = p.atmosphereSkyReproject;
    j["atmosphere_sky_cdf"] = p.atmosphereSkyCdf;
    j["atmosphere_aerial_perspective"] = p.atmosphereAerialPerspective;
    j["denoiser"] = p.denoiser;
    j["moment_update"] = p.momentUpdate;
    j["history_copy"] = p.historyCopy;
    j["skip_denoiser_copy"] = p.skipDenoiserCopy;
    j["taa"] = p.taa;
    j["taa_history_copy"] = p.taaHistoryCopy;
    j["auto_exposure_histogram_clear"] = p.autoExposureHistogramClear;
    j["auto_exposure_histogram"] = p.autoExposureHistogram;
    j["auto_exposure_reduce"] = p.autoExposureReduce;
    j["tone_map"] = p.toneMap;
    j["selection_outline"] = p.selectionOutline;
    j["fullscreen"] = p.fullscreen;
    j["editor_presentation"] = p.editorPresentation;
    j["dynamic_blas_update"] = p.dynamicBlasUpdate;
    j["wavefront_trace"] = p.wavefrontTrace;
    j["wavefront_secondary_trace"] = p.wavefrontSecondaryTrace;
    j["wavefront_sorted_trace"] = p.wavefrontSortedTrace;
    j["wavefront_shadow_trace"] = p.wavefrontShadowTrace;
    j["wavefront_shade"] = p.wavefrontShade;
    j["wavefront_secondary_shade"] = p.wavefrontSecondaryShade;
    j["wavefront_sorted_shade"] = p.wavefrontSortedShade;
    j["wavefront_compact"] = p.wavefrontCompact;
    j["wavefront_sort"] = p.wavefrontSort;
}

void to_json(nlohmann::json& j, const ProfileReport::QueueLaneMs& q) {
    j["graphics"] = q.graphics;
    j["ray_tracing"] = q.rayTracing;
    j["compute"] = q.compute;
    j["queue_wait"] = q.queueWait;
}

void to_json(nlohmann::json& j, const ProfileReport::AsyncComputeReport& a) {
    j["enabled"] = a.enabled;
    j["disabled_by_cli"] = a.disabledByCli;
    j["single_queue_fallback"] = a.singleQueueFallback;
    j["timeline_semaphore"] = a.timelineSemaphore;
    j["independent_queue"] = a.independentQueue;
    j["dedicated_compute_family"] = a.dedicatedComputeFamily;
    j["cross_family"] = a.crossFamily;
    j["graphics_family"] = a.graphicsFamily.has_value()
        ? nlohmann::json(*a.graphicsFamily)
        : nlohmann::json(nullptr);
    j["compute_family"] = a.computeFamily.has_value()
        ? nlohmann::json(*a.computeFamily)
        : nlohmann::json(nullptr);
    j["compute_queue_index"] = a.computeQueueIndex;
    j["resource_sharing_mode"] = a.resourceSharingMode;
    j["resource_sharing_queue_family_count"] = a.resourceSharingQueueFamilyCount;
    j["resource_sharing_queue_families"] = a.resourceSharingQueueFamilies;
}

void to_json(nlohmann::json& j, const ProfileReport::OpacityMicromapReport& o) {
    j["supported"] = o.supported;
    j["extension_supported"] = o.extensionSupported;
    j["micromap_feature"] = o.micromapFeature;
    j["capture_replay"] = o.captureReplay;
    j["host_commands"] = o.hostCommands;
    j["max_opacity_2_state_subdivision_level"] = o.maxOpacity2StateSubdivisionLevel;
    j["max_opacity_4_state_subdivision_level"] = o.maxOpacity4StateSubdivisionLevel;
    j["disabled_reason"] = o.disabledReason;
    j["preprocess"] = {
        {"subdivision_level", o.preprocess.subdivisionLevel},
        {"eligible_primitive_count", o.preprocess.eligiblePrimitiveCount},
        {"generated_primitive_count", o.preprocess.generatedPrimitiveCount},
        {"alpha_texture_primitive_count", o.preprocess.alphaTexturePrimitiveCount},
        {"constant_alpha_primitive_count", o.preprocess.constantAlphaPrimitiveCount},
        {"cache_entry_count", o.preprocess.cacheEntryCount},
        {"cache_hit_count", o.preprocess.cacheHitCount},
        {"total_triangle_count", o.preprocess.totalTriangleCount},
        {"micro_triangle_count", o.preprocess.microTriangleCount},
        {"opaque_count", o.preprocess.opaqueCount},
        {"transparent_count", o.preprocess.transparentCount},
        {"unknown_count", o.preprocess.unknownCount},
        {"mixed_count", o.preprocess.mixedCount},
        {"data_bytes", o.preprocess.dataBytes},
        {"preprocessing_ms", o.preprocess.preprocessingMs},
        {"validation_error_count", o.preprocess.validationErrorCount},
        {"warnings", o.preprocess.warnings},
    };
    j["build"] = {
        {"requested", o.build.requested},
        {"supported", o.build.supported},
        {"active", o.build.active},
        {"micromap_count", o.build.micromapCount},
        {"mesh_count", o.build.meshCount},
        {"triangle_array_count", o.build.triangleArrayCount},
        {"indexed_triangle_count", o.build.indexedTriangleCount},
        {"packed_micro_triangle_count", o.build.packedMicroTriangleCount},
        {"micromap_bytes", o.build.micromapBytes},
        {"build_input_bytes", o.build.buildInputBytes},
        {"build_scratch_bytes", o.build.buildScratchBytes},
        {"build_ms", o.build.buildMs},
        {"fallback_reason", o.build.fallbackReason},
    };
}

void to_json(nlohmann::json& j, const ProfileReport::SerReport& s) {
    j["supported"] = s.supported;
    j["enabled"] = s.enabled;
    j["extension_supported"] = s.extensionSupported;
    j["invocation_reorder_feature"] = s.invocationReorderFeature;
    j["dedicated_ser_pipeline"] = s.dedicatedSerPipeline;
    j["pipeline_create_flag_required"] = s.pipelineCreateFlagRequired;
    j["max_invocation_reorder_depth_reported"] = s.maxInvocationReorderDepthReported;
    j["max_ray_tracing_invocation_reorder_depth"] = s.maxRayTracingInvocationReorderDepth;
    j["performance_evidence_available"] = s.performanceEvidenceAvailable;
    j["performance_target_passed"] = s.performanceTargetPassed;
    j["performance_target_min_percent"] = s.performanceTargetMinPercent;
    j["performance_target_max_percent"] = s.performanceTargetMaxPercent;
    j["observed_improvement_percent"] = s.observedImprovementPercent;
    j["reordering_hint"] = s.reorderingHint;
    j["disabled_reason"] = s.disabledReason;
}

void to_json(nlohmann::json& j, const ProfileReport::RayTracingMotionBlurReport& m) {
    j["supported"] = m.supported;
    j["enabled"] = m.enabled;
    j["extension_supported"] = m.extensionSupported;
    j["ray_tracing_motion_blur_feature"] = m.rayTracingMotionBlurFeature;
    j["ray_tracing_motion_blur_pipeline_trace_rays_indirect"] = m.rayTracingMotionBlurPipelineTraceRaysIndirect;
    j["motion_instances_active"] = m.motionInstancesActive;
    j["motion_instance_count"] = m.motionInstanceCount;
    j["moving_instance_count"] = m.movingInstanceCount;
    j["static_instance_count"] = m.staticInstanceCount;
    j["tlas_refit_count"] = m.tlasRefitCount;
    j["max_transform_delta"] = m.maxTransformDelta;
    j["has_moving_and_static_instances"] = m.hasMovingAndStaticInstances;
    j["disabled_reason"] = m.disabledReason;
}

void to_json(nlohmann::json& j, const ProfileReport::SceneUpdateRouteReport& r) {
    j = nlohmann::json{
        {"kind", r.kind},
        {"action", r.action},
        {"count", r.count},
        {"total_cpu_ms", r.totalCpuMs},
        {"last_cpu_ms", r.lastCpuMs},
        {"avg_cpu_ms", r.averageCpuMs},
        {"min_cpu_ms", r.minCpuMs},
        {"max_cpu_ms", r.maxCpuMs},
    };
}

void to_json(nlohmann::json& j, const ProfileReport::SchedulerQueueReport& r) {
    j = nlohmann::json{
        {"queue", r.queue},
        {"job", r.job},
        {"status", r.status},
        {"generation", r.generation},
        {"count", r.count},
        {"total_cpu_ms", r.totalCpuMs},
        {"last_cpu_ms", r.lastCpuMs},
        {"avg_cpu_ms", r.averageCpuMs},
        {"min_cpu_ms", r.minCpuMs},
        {"max_cpu_ms", r.maxCpuMs},
        {"frame_budget_violation_count", r.frameBudgetViolationCount},
        {"submitted_bytes", r.submittedBytes},
        {"completed_bytes", r.completedBytes},
    };
}

void to_json(nlohmann::json& j, const GpuUploadChunkSnapshot& c) {
    j = nlohmann::json{
        {"index", c.index},
        {"offset", c.offset},
        {"bytes", c.bytes},
        {"timeline_value", c.timelineValue},
        {"state", gpuUploadChunkStateName(c.state)},
        {"staging_retained", c.stagingRetained},
    };
}

void to_json(nlohmann::json& j, const GpuUploadTicketSnapshot& t) {
    j = nlohmann::json{
        {"id", t.id},
        {"kind", gpuUploadResourceKindName(t.kind)},
        {"state", gpuUploadTicketStateName(t.state)},
        {"label", t.label},
        {"total_bytes", t.totalBytes},
        {"submitted_bytes", t.submittedBytes},
        {"completed_bytes", t.completedBytes},
        {"retained_staging_bytes", t.retainedStagingBytes},
        {"chunk_count", t.chunkCount},
        {"pending_chunks", t.pendingChunks},
        {"submitted_chunks", t.submittedChunks},
        {"completed_chunks", t.completedChunks},
        {"cancellation_requested", t.cancellationRequested},
        {"can_cancel", t.canCancel},
        {"can_retire", t.canRetire},
        {"chunks", t.chunks},
    };
}

void to_json(nlohmann::json& j, const MainThreadApplyOperationSnapshot& o) {
    j = nlohmann::json{
        {"index", o.index},
        {"kind", mainThreadApplyOperationKindName(o.kind)},
        {"state", mainThreadApplyOperationStateName(o.state)},
        {"entity", o.entity},
        {"estimated_cost_ms", o.estimatedCostMs},
        {"label", o.label},
    };
}

void to_json(nlohmann::json& j, const MainThreadApplyTicketSnapshot& t) {
    j = nlohmann::json{
        {"id", t.id},
        {"state", mainThreadApplyTicketStateName(t.state)},
        {"label", t.label},
        {"operation_count", t.operationCount},
        {"pending_operations", t.pendingOperations},
        {"applied_operations", t.appliedOperations},
        {"cancelled_operations", t.cancelledOperations},
        {"progress", t.progress},
        {"undo_snapshot_open", t.undoSnapshotOpen},
        {"undo_snapshot_committed", t.undoSnapshotCommitted},
        {"cancellation_requested", t.cancellationRequested},
        {"can_cancel", t.canCancel},
        {"locked_entities", t.lockedEntities},
        {"operations", t.operations},
    };
}

void to_json(nlohmann::json& j, const TopologyRebuildStageSnapshot& s) {
    j = nlohmann::json{
        {"index", s.index},
        {"stage", topologyRebuildStageName(s.stage)},
        {"state", topologyRebuildStageStateName(s.state)},
        {"estimated_cost_ms", s.estimatedCostMs},
        {"label", s.label},
    };
}

void to_json(nlohmann::json& j, const TopologyRebuildTicketSnapshot& t) {
    j = nlohmann::json{
        {"id", t.id},
        {"generation", t.generation},
        {"state", topologyRebuildTicketStateName(t.state)},
        {"label", t.label},
        {"stage_count", t.stageCount},
        {"pending_stages", t.pendingStages},
        {"completed_stages", t.completedStages},
        {"cancelled_stages", t.cancelledStages},
        {"progress", t.progress},
        {"previous_renderer_visible", t.previousRendererVisible},
        {"final_renderer_swapped", t.finalRendererSwapped},
        {"old_renderer_retained", t.oldRendererRetained},
        {"old_renderer_retired", t.oldRendererRetired},
        {"retirement_timeline_value", t.retirementTimelineValue},
        {"cancellation_requested", t.cancellationRequested},
        {"stale_generation", t.staleGeneration},
        {"stages", t.stages},
    };
}

void appendCompletedUploaderSchedulerReport(ProfileReport& report) {
    uint64_t submittedBytes = 0;
    uint64_t completedBytes = 0;
    uint64_t completedTicketCount = 0;
    for (const GpuUploadTicketSnapshot& ticket : report.gpuUploadTickets) {
        if (ticket.state != GpuUploadTicketState::Complete || ticket.label.rfind("Vulkan ", 0) != 0) {
            continue;
        }
        submittedBytes += ticket.submittedBytes;
        completedBytes += ticket.completedBytes;
        ++completedTicketCount;
    }
    if (completedTicketCount == 0) {
        return;
    }

    ProfileReport::SchedulerQueueReport schedulerReport;
    schedulerReport.queue = "GpuUploadSubmit";
    schedulerReport.job = "BufferUploader";
    schedulerReport.status = "completed_sync";
    schedulerReport.count = completedTicketCount;
    schedulerReport.submittedBytes = submittedBytes;
    schedulerReport.completedBytes = completedBytes;
    report.schedulerQueues.push_back(std::move(schedulerReport));
}

void appendCompletedMainThreadApplySchedulerReport(ProfileReport& report) {
    uint64_t completedOperationCount = 0;
    double totalCpuMs = 0.0;
    double minCpuMs = 0.0;
    double maxCpuMs = 0.0;
    for (const MainThreadApplyTicketSnapshot& ticket : report.mainThreadApplyTickets) {
        if (ticket.state != MainThreadApplyTicketState::Complete) {
            continue;
        }
        for (const MainThreadApplyOperationSnapshot& operation : ticket.operations) {
            if (operation.state != MainThreadApplyOperationState::Applied) {
                continue;
            }
            const double costMs = std::max(0.0, operation.estimatedCostMs);
            if (completedOperationCount == 0) {
                minCpuMs = costMs;
                maxCpuMs = costMs;
            } else {
                minCpuMs = std::min(minCpuMs, costMs);
                maxCpuMs = std::max(maxCpuMs, costMs);
            }
            totalCpuMs += costMs;
            ++completedOperationCount;
        }
    }
    if (completedOperationCount == 0) {
        return;
    }

    ProfileReport::SchedulerQueueReport schedulerReport;
    schedulerReport.queue = "MainThreadApply";
    schedulerReport.job = "MainThreadApplyTicket";
    schedulerReport.status = "completed_operations";
    schedulerReport.count = completedOperationCount;
    schedulerReport.totalCpuMs = totalCpuMs;
    schedulerReport.lastCpuMs = maxCpuMs;
    schedulerReport.averageCpuMs = totalCpuMs / static_cast<double>(completedOperationCount);
    schedulerReport.minCpuMs = minCpuMs;
    schedulerReport.maxCpuMs = maxCpuMs;
    report.schedulerQueues.push_back(std::move(schedulerReport));
}

void appendCompletedTopologySchedulerReport(ProfileReport& report) {
    uint64_t completedStageCount = 0;
    double totalCpuMs = 0.0;
    double minCpuMs = 0.0;
    double maxCpuMs = 0.0;
    uint64_t latestGeneration = 0;
    for (const TopologyRebuildTicketSnapshot& ticket : report.topologyRebuildTickets) {
        if (ticket.state != TopologyRebuildTicketState::Complete) {
            continue;
        }
        latestGeneration = std::max(latestGeneration, ticket.generation);
        for (const TopologyRebuildStageSnapshot& stage : ticket.stages) {
            if (stage.state != TopologyRebuildStageState::Complete) {
                continue;
            }
            const double costMs = std::max(0.0, stage.estimatedCostMs);
            if (completedStageCount == 0) {
                minCpuMs = costMs;
                maxCpuMs = costMs;
            } else {
                minCpuMs = std::min(minCpuMs, costMs);
                maxCpuMs = std::max(maxCpuMs, costMs);
            }
            totalCpuMs += costMs;
            ++completedStageCount;
        }
    }
    if (completedStageCount == 0) {
        return;
    }

    ProfileReport::SchedulerQueueReport schedulerReport;
    schedulerReport.queue = "GpuSceneBuild";
    schedulerReport.job = "TopologyRebuildTicket";
    schedulerReport.status = "completed_stages";
    schedulerReport.generation = latestGeneration;
    schedulerReport.count = completedStageCount;
    schedulerReport.totalCpuMs = totalCpuMs;
    schedulerReport.lastCpuMs = maxCpuMs;
    schedulerReport.averageCpuMs = totalCpuMs / static_cast<double>(completedStageCount);
    schedulerReport.minCpuMs = minCpuMs;
    schedulerReport.maxCpuMs = maxCpuMs;
    report.schedulerQueues.push_back(std::move(schedulerReport));
}

void to_json(nlohmann::json& j, const ProfileReport::PipelineStatistics& s) {
    j["ray_invocations"] = s.rayInvocations;
    j["triangle_hits"] = s.triangleHits;
    j["aabb_hits"] = s.aabbHits;
}

void to_json(nlohmann::json& j, const ProfileReport::RayTracingDiagnosticCounterReport& s) {
    j["camera_any_hit_invocations"] = s.cameraAnyHitInvocations;
    j["camera_any_hit_ignored"] = s.cameraAnyHitIgnored;
    j["camera_any_hit_accepted"] = s.cameraAnyHitAccepted;
    j["shadow_any_hit_invocations"] = s.shadowAnyHitInvocations;
    j["shadow_any_hit_ignored"] = s.shadowAnyHitIgnored;
    j["shadow_any_hit_accepted"] = s.shadowAnyHitAccepted;
    j["surface_trace_rays"] = s.surfaceTraceRays;
    j["shadow_trace_rays"] = s.shadowTraceRays;
    j["closest_hit_invocations"] = s.closestHitInvocations;
    j["closest_hit_alpha_materials"] = s.closestHitAlphaMaterials;
    j["caustic_shadow_attempts"] = s.causticShadowAttempts;
    j["caustic_transmissive_hits"] = s.causticTransmissiveHits;
    j["caustic_transmissive_visible"] = s.causticTransmissiveVisible;
    j["caustic_shadow_blocked"] = s.causticShadowBlocked;
}

void to_json(nlohmann::json& j, const ProfileReport::RayTracingGeometryReport& s) {
    j["opaque_primitive_count"] = s.opaquePrimitiveCount;
    j["alpha_tested_primitive_count"] = s.alphaTestedPrimitiveCount;
    j["blended_primitive_count"] = s.blendedPrimitiveCount;
    j["opaque_triangle_count"] = s.opaqueTriangleCount;
    j["alpha_tested_triangle_count"] = s.alphaTestedTriangleCount;
    j["blended_triangle_count"] = s.blendedTriangleCount;
    j["mesh_count_with_only_opaque_geometry"] = s.meshCountWithOnlyOpaqueGeometry;
    j["mesh_count_with_alpha_tested_geometry"] = s.meshCountWithAlphaTestedGeometry;
    j["mesh_count_with_blended_geometry"] = s.meshCountWithBlendedGeometry;
    j["blas_geometry_count"] = s.blasGeometryCount;
    j["blas_opaque_geometry_count"] = s.blasOpaqueGeometryCount;
    j["blas_alpha_tested_geometry_count"] = s.blasAlphaTestedGeometryCount;
    j["blas_blended_geometry_count"] = s.blasBlendedGeometryCount;
    j["blas_opacity_micromap_geometry_count"] = s.blasOpacityMicromapGeometryCount;
    j["blas_build_batch_count"] = s.blasBuildBatchCount;
}

void to_json(nlohmann::json& j, const ProfileReport::AnimatedGeometryReport& s) {
    j["mesh_instance_count"] = s.meshInstanceCount;
    j["static_mesh_instance_count"] = s.staticMeshInstanceCount;
    j["transform_only_candidate_instance_count"] = s.transformOnlyCandidateInstanceCount;
    j["deforming_instance_count"] = s.deformingInstanceCount;
    j["morph_deforming_instance_count"] = s.morphDeformingInstanceCount;
    j["skinned_deforming_instance_count"] = s.skinnedDeformingInstanceCount;
    j["combined_morph_skinned_instance_count"] = s.combinedMorphSkinnedInstanceCount;
    j["runtime_mesh_instance_count"] = s.runtimeMeshInstanceCount;
    j["material_override_runtime_mesh_instance_count"] = s.materialOverrideRuntimeMeshInstanceCount;
    j["missing_mesh_instance_count"] = s.missingMeshInstanceCount;
    j["total_primitive_count"] = s.totalPrimitiveCount;
    j["total_triangle_count"] = s.totalTriangleCount;
    j["gpu_skinning_candidate_instance_count"] = s.gpuSkinningCandidateInstanceCount;
    j["gpu_skinning_morph_pre_skin_instance_count"] = s.gpuSkinningMorphPreSkinInstanceCount;
    j["gpu_skinning_joint_matrix_count"] = s.gpuSkinningJointMatrixCount;
    j["gpu_skinning_joint_upload_bytes"] = s.gpuSkinningJointUploadBytes;
    j["gpu_skinning_previous_joint_upload_bytes"] = s.gpuSkinningPreviousJointUploadBytes;
    j["gpu_skinning_source_vertex_upload_bytes"] = s.gpuSkinningSourceVertexUploadBytes;
    j["gpu_skinning_morph_delta_upload_bytes"] = s.gpuSkinningMorphDeltaUploadBytes;
    j["gpu_skinning_current_vertex_count"] = s.gpuSkinningCurrentVertexCount;
    j["gpu_skinning_previous_vertex_count"] = s.gpuSkinningPreviousVertexCount;
    j["gpu_skinning_current_vertex_buffer_bytes"] = s.gpuSkinningCurrentVertexBufferBytes;
    j["gpu_skinning_previous_vertex_buffer_bytes"] = s.gpuSkinningPreviousVertexBufferBytes;
    j["gpu_skinning_cpu_fallback_instance_count"] = s.gpuSkinningCpuFallbackInstanceCount;
    j["gpu_skinning_dispatch_record_count"] = s.gpuSkinningDispatchRecordCount;
    j["gpu_skinning_renderer_plan_active"] = s.gpuSkinningRendererPlanActive;
    j["gpu_skinning_renderer_joint_upload_bytes"] = s.gpuSkinningRendererJointUploadBytes;
    j["gpu_skinning_renderer_previous_joint_upload_bytes"] = s.gpuSkinningRendererPreviousJointUploadBytes;
    j["gpu_skinning_renderer_source_vertex_upload_bytes"] = s.gpuSkinningRendererSourceVertexUploadBytes;
    j["gpu_skinning_renderer_morph_delta_upload_bytes"] = s.gpuSkinningRendererMorphDeltaUploadBytes;
    j["gpu_skinning_renderer_current_vertex_buffer_bytes"] = s.gpuSkinningRendererCurrentVertexBufferBytes;
    j["gpu_skinning_renderer_previous_vertex_buffer_bytes"] = s.gpuSkinningRendererPreviousVertexBufferBytes;
    j["gpu_skinning_renderer_buffers_allocated"] = s.gpuSkinningRendererBuffersAllocated;
    j["gpu_skinning_renderer_source_vertex_buffer_uploaded"] = s.gpuSkinningRendererSourceVertexBufferUploaded;
    j["gpu_skinning_renderer_morph_delta_buffer_uploaded"] = s.gpuSkinningRendererMorphDeltaBufferUploaded;
    j["gpu_skinning_renderer_joint_buffer_uploaded"] = s.gpuSkinningRendererJointBufferUploaded;
    j["gpu_skinning_renderer_previous_joint_buffer_uploaded"] = s.gpuSkinningRendererPreviousJointBufferUploaded;
    j["gpu_skinning_renderer_joint_payload_refresh_uploaded"] = s.gpuSkinningRendererJointPayloadRefreshUploaded;
    j["gpu_skinning_renderer_joint_payload_refresh_staged"] = s.gpuSkinningRendererJointPayloadRefreshStaged;
    j["gpu_skinning_renderer_joint_payload_refresh_copy_recorded"] = s.gpuSkinningRendererJointPayloadRefreshCopyRecorded;
    j["gpu_skinning_renderer_joint_payload_refresh_count"] = s.gpuSkinningRendererJointPayloadRefreshCount;
    j["gpu_skinning_renderer_joint_payload_refresh_copy_record_count"] = s.gpuSkinningRendererJointPayloadRefreshCopyRecordCount;
    j["gpu_skinning_renderer_joint_payload_refresh_bytes"] = s.gpuSkinningRendererJointPayloadRefreshBytes;
    j["gpu_skinning_renderer_joint_payload_refresh_staged_bytes"] = s.gpuSkinningRendererJointPayloadRefreshStagedBytes;
    j["gpu_skinning_renderer_joint_payload_refresh_copy_bytes"] = s.gpuSkinningRendererJointPayloadRefreshCopyBytes;
    j["gpu_skinning_renderer_compute_pipeline_created"] = s.gpuSkinningRendererComputePipelineCreated;
    j["gpu_skinning_renderer_descriptors_bound"] = s.gpuSkinningRendererDescriptorsBound;
    j["gpu_skinning_renderer_compute_dispatch_enabled"] = s.gpuSkinningRendererComputeDispatchEnabled;
    j["gpu_skinning_renderer_compute_dispatch_record_count"] = s.gpuSkinningRendererComputeDispatchRecordCount;
    j["gpu_skinning_renderer_compute_morph_dispatch_record_count"] = s.gpuSkinningRendererComputeMorphDispatchRecordCount;
    j["gpu_skinning_renderer_output_readback_buffer_allocated"] = s.gpuSkinningRendererOutputReadbackBufferAllocated;
    j["gpu_skinning_renderer_output_readback_copy_recorded"] = s.gpuSkinningRendererOutputReadbackCopyRecorded;
    j["gpu_skinning_renderer_output_readback_validation_passed"] = s.gpuSkinningRendererOutputReadbackValidationPassed;
    j["gpu_skinning_renderer_output_readback_vertex_count"] = s.gpuSkinningRendererOutputReadbackVertexCount;
    j["gpu_skinning_renderer_output_readback_max_position_error"] = s.gpuSkinningRendererOutputReadbackMaxPositionError;
    j["gpu_skinning_renderer_initial_compute_dispatch_submitted"] = s.gpuSkinningRendererInitialComputeDispatchSubmitted;
    j["gpu_skinning_ray_tracing_geometry_input_enabled"] = s.gpuSkinningRayTracingGeometryInputEnabled;
    j["gpu_skinning_ray_tracing_geometry_input_mesh_count"] = s.gpuSkinningRayTracingGeometryInputMeshCount;
    j["gpu_skinning_ray_tracing_geometry_input_geometry_count"] = s.gpuSkinningRayTracingGeometryInputGeometryCount;
    j["gpu_skinning_ray_tracing_descriptor_input_enabled"] = s.gpuSkinningRayTracingDescriptorInputEnabled;
    j["gpu_skinning_ray_tracing_descriptor_input_mixed_scene_enabled"] = s.gpuSkinningRayTracingDescriptorInputMixedSceneEnabled;
    j["gpu_skinning_ray_tracing_descriptor_input_mesh_count"] = s.gpuSkinningRayTracingDescriptorInputMeshCount;
    j["gpu_skinning_ray_tracing_descriptor_input_binding_count"] = s.gpuSkinningRayTracingDescriptorInputBindingCount;
    j["gpu_skinning_skinned_motion_vectors_enabled"] = s.gpuSkinningSkinnedMotionVectorsEnabled;
    j["gpu_skinning_skinned_motion_vectors_previous_vertex_input_enabled"] = s.gpuSkinningSkinnedMotionVectorsPreviousVertexInputEnabled;
    j["gpu_skinning_skinned_motion_vectors_descriptor_bound"] = s.gpuSkinningSkinnedMotionVectorsDescriptorBound;
    j["ray_tracing_blas_count"] = s.rayTracingBlasCount;
    j["ray_tracing_instance_count"] = s.rayTracingInstanceCount;
    j["tlas_refit_count"] = s.tlasRefitCount;
    j["last_tlas_refit_ms"] = s.lastTlasRefitMs;
    j["dynamic_blas_update_count"] = s.dynamicBlasUpdateCount;
    j["dynamic_blas_update_record_ms"] = s.dynamicBlasUpdateRecordMs;
    j["dynamic_blas_update_ms"] = s.dynamicBlasUpdateMs;
    j["dynamic_blas_update_supported"] = s.dynamicBlasUpdateSupported;
    j["dynamic_blas_unsupported_instance_count"] = s.dynamicBlasUnsupportedInstanceCount;
    j["dynamic_blas_cpu_fallback_instance_count"] = s.dynamicBlasCpuFallbackInstanceCount;
    j["dynamic_blas_fallback_policy"] = s.dynamicBlasFallbackPolicy;
    j["dynamic_blas_fallback_warnings"] = s.dynamicBlasFallbackWarnings;
    j["dynamic_blas_budget_ms"] = s.dynamicBlasBudgetMs;
    j["dynamic_blas_budget_exceeded"] = s.dynamicBlasBudgetExceeded;
    j["dynamic_blas_over_budget_frame_count"] = s.dynamicBlasOverBudgetFrameCount;
    j["dynamic_blas_budget_policy"] = s.dynamicBlasBudgetPolicy;
    j["transform_only_policy"] = s.transformOnlyPolicy;
    j["deforming_policy"] = s.deformingPolicy;
    j["morph_policy"] = s.morphPolicy;
    j["skinning_policy"] = s.skinningPolicy;
    j["gpu_skinning_data_policy"] = s.gpuSkinningDataPolicy;
    j["gpu_skinning_joint_upload_policy"] = s.gpuSkinningJointUploadPolicy;
    j["gpu_skinning_buffer_policy"] = s.gpuSkinningBufferPolicy;
    j["gpu_skinning_compute_shader"] = s.gpuSkinningComputeShader;
    j["tlas_refit_policy"] = s.tlasRefitPolicy;
    j["dynamic_blas_timing_policy"] = s.dynamicBlasTimingPolicy;
}

void to_json(nlohmann::json& j, const ProfileReport::SceneLightReport::Sample& s) {
    j["record_index"] = s.recordIndex;
    j["type"] = s.type;
    j["source_index"] = s.sourceIndex;
    j["position_or_direction"] = {s.positionOrDirection[0], s.positionOrDirection[1], s.positionOrDirection[2]};
    j["radius_or_size"] = s.radiusOrSize;
    j["weight"] = s.weight;
}

void to_json(nlohmann::json& j, const ProfileReport::SceneLightReport& s) {
    j["record_count"] = s.recordCount;
    j["emissive_count"] = s.emissiveCount;
    j["authored_count"] = s.authoredCount;
    j["directional_count"] = s.directionalCount;
    j["point_count"] = s.pointCount;
    j["area_count"] = s.areaCount;
    j["spot_count"] = s.spotCount;
    j["authored_positional_count"] = s.authoredPositionalCount;
    j["authored_distinct_position_count"] = s.authoredDistinctPositionCount;
    j["authored_positions_collapsed"] = s.authoredPositionsCollapsed;
    j["emissive_samples"] = s.emissiveSamples;
    j["authored_samples"] = s.authoredSamples;
}

void to_json(nlohmann::json& j, const ProfileReport::WavefrontQueueReport& s) {
    j["buffers_allocated"] = s.buffersAllocated;
    j["clear_validation_passed"] = s.clearValidationPassed;
    j["max_path_depth"] = s.maxPathDepth;
    j["ray_queue_capacity"] = s.rayQueueCapacity;
    j["compacted_ray_queue_capacity"] = s.compactedRayQueueCapacity;
    j["sorted_ray_queue_capacity"] = s.sortedRayQueueCapacity;
    j["hit_queue_capacity"] = s.hitQueueCapacity;
    j["shadow_queue_capacity"] = s.shadowQueueCapacity;
    j["pixel_state_capacity"] = s.pixelStateCapacity;
    j["ray_queue_count"] = s.rayQueueCount;
    j["hit_queue_count"] = s.hitQueueCount;
    j["shadow_queue_count"] = s.shadowQueueCount;
    j["pixel_state_count"] = s.pixelStateCount;
    j["clear_validation_counter"] = s.clearValidationCounter;
    j["primary_generation_enabled"] = s.primaryGenerationEnabled;
    j["primary_generation_validation_passed"] = s.primaryGenerationValidationPassed;
    j["expected_primary_ray_count"] = s.expectedPrimaryRayCount;
    j["sampled_primary_ray_count"] = s.sampledPrimaryRayCount;
    j["first_ray_direction_error"] = s.firstRayDirectionError;
    j["center_ray_direction_error"] = s.centerRayDirectionError;
    j["corner_ray_direction_error"] = s.cornerRayDirectionError;
    j["max_ray_direction_error"] = s.maxRayDirectionError;
    j["trace_enabled"] = s.traceEnabled;
    j["trace_validation_passed"] = s.traceValidationPassed;
    j["trace_rays_indirect_supported"] = s.traceRaysIndirectSupported;
    j["secondary_trace_indirect_enabled"] = s.secondaryTraceIndirectEnabled;
    j["trace_checked_pixels"] = s.traceCheckedPixels;
    j["trace_hit_mismatch_count"] = s.traceHitMismatchCount;
    j["trace_instance_mismatch_count"] = s.traceInstanceMismatchCount;
    j["trace_depth_mismatch_count"] = s.traceDepthMismatchCount;
    j["trace_normal_mismatch_count"] = s.traceNormalMismatchCount;
    j["shade_enabled"] = s.shadeEnabled;
    j["shade_validation_passed"] = s.shadeValidationPassed;
    j["shade_checked_pixels"] = s.shadeCheckedPixels;
    j["shade_hit_count"] = s.shadeHitCount;
    j["shade_miss_count"] = s.shadeMissCount;
    j["shade_terminated_count"] = s.shadeTerminatedCount;
    j["shade_shadow_ray_count"] = s.shadeShadowRayCount;
    j["shade_secondary_ray_count"] = s.shadeSecondaryRayCount;
    j["shade_material_count"] = s.shadeMaterialCount;
    j["shade_restir_reservoir_write_count"] = s.shadeRestirReservoirWriteCount;
    j["shade_restir_valid_candidate_count"] = s.shadeRestirValidCandidateCount;
    j["shade_restir_temporal_merge_count"] = s.shadeRestirTemporalMergeCount;
    j["shade_restir_invalid_candidate_count"] = s.shadeRestirInvalidCandidateCount;
    j["shade_restir_gi_reservoir_write_count"] = s.shadeRestirGiReservoirWriteCount;
    j["shade_restir_gi_valid_candidate_count"] = s.shadeRestirGiValidCandidateCount;
    j["shade_restir_gi_temporal_merge_count"] = s.shadeRestirGiTemporalMergeCount;
    j["shade_restir_gi_invalid_candidate_count"] = s.shadeRestirGiInvalidCandidateCount;
    j["secondary_shade_enabled"] = s.secondaryShadeEnabled;
    j["secondary_shade_validation_passed"] = s.secondaryShadeValidationPassed;
    j["secondary_shade_checked_rays"] = s.secondaryShadeCheckedRays;
    j["secondary_shade_hit_count"] = s.secondaryShadeHitCount;
    j["secondary_shade_miss_count"] = s.secondaryShadeMissCount;
    j["secondary_shade_terminated_count"] = s.secondaryShadeTerminatedCount;
    j["secondary_shade_shadow_ray_count"] = s.secondaryShadeShadowRayCount;
    j["secondary_shade_secondary_ray_count"] = s.secondaryShadeSecondaryRayCount;
    j["secondary_shade_material_count"] = s.secondaryShadeMaterialCount;
    j["secondary_shade_cost_ms"] = s.secondaryShadeCostMs;
    j["secondary_shade_microseconds_per_ray"] = s.secondaryShadeMicrosecondsPerRay;
    j["secondary_path_cost_ms"] = s.secondaryPathCostMs;
    j["secondary_path_microseconds_per_ray"] = s.secondaryPathMicrosecondsPerRay;
    j["sorted_shade_enabled"] = s.sortedShadeEnabled;
    j["sorted_shade_validation_passed"] = s.sortedShadeValidationPassed;
    j["sorted_shade_checked_rays"] = s.sortedShadeCheckedRays;
    j["sorted_shade_hit_count"] = s.sortedShadeHitCount;
    j["sorted_shade_miss_count"] = s.sortedShadeMissCount;
    j["sorted_shade_terminated_count"] = s.sortedShadeTerminatedCount;
    j["sorted_shade_shadow_ray_count"] = s.sortedShadeShadowRayCount;
    j["sorted_shade_secondary_ray_count"] = s.sortedShadeSecondaryRayCount;
    j["sorted_shade_material_count"] = s.sortedShadeMaterialCount;
    j["sorted_shade_cost_ms"] = s.sortedShadeCostMs;
    j["sorted_shade_microseconds_per_ray"] = s.sortedShadeMicrosecondsPerRay;
    j["sorted_path_cost_ms"] = s.sortedPathCostMs;
    j["sorted_path_microseconds_per_ray"] = s.sortedPathMicrosecondsPerRay;
    j["sort_net_benefit_evidence_available"] = s.sortNetBenefitEvidenceAvailable;
    j["sort_net_benefit_passed"] = s.sortNetBenefitPassed;
    j["sort_net_benefit_ms"] = s.sortNetBenefitMs;
    j["compact_enabled"] = s.compactEnabled;
    j["compact_validation_passed"] = s.compactValidationPassed;
    j["compact_input_ray_count"] = s.compactInputRayCount;
    j["compact_scanned_ray_count"] = s.compactScannedRayCount;
    j["compact_live_ray_count"] = s.compactLiveRayCount;
    j["compact_output_ray_count"] = s.compactOutputRayCount;
    j["compact_dropped_invalid_count"] = s.compactDroppedInvalidCount;
    j["compact_overflow_count"] = s.compactOverflowCount;
    j["compact_invalid_pixel_count"] = s.compactInvalidPixelCount;
    j["compact_mapping_mismatch_count"] = s.compactMappingMismatchCount;
    j["compact_queue_occupancy"] = s.compactQueueOccupancy;
    j["compact_survival_ratio"] = s.compactSurvivalRatio;
    j["compact_cost_ms"] = s.compactCostMs;
    j["compact_microseconds_per_ray"] = s.compactMicrosecondsPerRay;
    j["primary_queue_occupancy"] = s.primaryQueueOccupancy;
    j["trace_hit_queue_occupancy"] = s.traceHitQueueOccupancy;
    j["shade_secondary_queue_occupancy"] = s.shadeSecondaryQueueOccupancy;
    j["sort_output_queue_occupancy"] = s.sortOutputQueueOccupancy;
    j["secondary_shade_shadow_queue_occupancy"] = s.secondaryShadeShadowQueueOccupancy;
    j["secondary_shade_secondary_queue_occupancy"] = s.secondaryShadeSecondaryQueueOccupancy;
    j["sorted_shade_shadow_queue_occupancy"] = s.sortedShadeShadowQueueOccupancy;
    j["sorted_shade_secondary_queue_occupancy"] = s.sortedShadeSecondaryQueueOccupancy;
    j["queue_balance_validation_passed"] = s.queueBalanceValidationPassed;
    j["queue_starvation_detected"] = s.queueStarvationDetected;
    j["queue_overflow_count"] = s.queueOverflowCount;
    j["sort_enabled"] = s.sortEnabled;
    j["final_output_enabled"] = s.finalOutputEnabled;
    j["sort_validation_passed"] = s.sortValidationPassed;
    j["sort_input_ray_count"] = s.sortInputRayCount;
    j["sort_output_ray_count"] = s.sortOutputRayCount;
    j["sort_active_bucket_count"] = s.sortActiveBucketCount;
    j["sort_verified_ray_count"] = s.sortVerifiedRayCount;
    j["sort_bucket_count"] = s.sortBucketCount;
    j["sort_overflow_count"] = s.sortOverflowCount;
    j["sort_invalid_pixel_count"] = s.sortInvalidPixelCount;
    j["sort_order_violation_count"] = s.sortOrderViolationCount;
    j["sort_cost_ms"] = s.sortCostMs;
    j["sort_microseconds_per_ray"] = s.sortMicrosecondsPerRay;
    j["shadow_trace_enabled"] = s.shadowTraceEnabled;
    j["shadow_trace_validation_passed"] = s.shadowTraceValidationPassed;
    j["shadow_trace_checked_rays"] = s.shadowTraceCheckedRays;
    j["shadow_trace_visible_count"] = s.shadowTraceVisibleCount;
    j["shadow_trace_occluded_count"] = s.shadowTraceOccludedCount;
    j["shadow_trace_applied_count"] = s.shadowTraceAppliedCount;
    j["direct_lighting_parity_passed"] = s.directLightingParityPassed;
    j["direct_lighting_checked_pixels"] = s.directLightingCheckedPixels;
    j["direct_lighting_mismatch_count"] = s.directLightingMismatchCount;
    j["direct_lighting_max_abs_error"] = s.directLightingMaxAbsError;
    j["direct_lighting_max_relative_error"] = s.directLightingMaxRelativeError;
    j["shadow_queue_occupancy"] = s.shadowQueueOccupancy;
    j["shadow_trace_rays_per_pixel"] = s.shadowTraceRaysPerPixel;
    j["shadow_trace_visible_ratio"] = s.shadowTraceVisibleRatio;
    j["shadow_trace_occluded_ratio"] = s.shadowTraceOccludedRatio;
    j["shadow_trace_cost_ms"] = s.shadowTraceCostMs;
    j["shadow_trace_microseconds_per_ray"] = s.shadowTraceMicrosecondsPerRay;
    j["ray_queue_bytes"] = s.rayQueueBytes;
    j["compacted_ray_queue_bytes"] = s.compactedRayQueueBytes;
    j["sorted_ray_queue_bytes"] = s.sortedRayQueueBytes;
    j["hit_queue_bytes"] = s.hitQueueBytes;
    j["shadow_queue_bytes"] = s.shadowQueueBytes;
    j["pixel_state_bytes"] = s.pixelStateBytes;
    j["total_bytes"] = s.totalBytes;
    j["transient_arena_used_bytes"] = s.transientArenaUsedBytes;
    j["transient_arena_high_water_bytes"] = s.transientArenaHighWaterBytes;
    j["transient_arena_capacity_bytes"] = s.transientArenaCapacityBytes;
}

void to_json(nlohmann::json& j, const ProfileReport::WavefrontValidationReport& s) {
    j["enabled"] = s.enabled;
    j["mode"] = s.mode;
    j["all_required_passed"] = s.allRequiredPassed;
    j["primary_generation_passed"] = s.primaryGenerationPassed;
    j["trace_passed"] = s.tracePassed;
    j["shade_passed"] = s.shadePassed;
    j["compact_passed"] = s.compactPassed;
    j["secondary_shade_passed"] = s.secondaryShadePassed;
    j["shadow_trace_passed"] = s.shadowTracePassed;
    j["direct_lighting_parity_passed"] = s.directLightingParityPassed;
    j["queue_balance_passed"] = s.queueBalancePassed;
    j["checked_pixels"] = s.checkedPixels;
    j["checked_secondary_rays"] = s.checkedSecondaryRays;
    j["checked_shadow_rays"] = s.checkedShadowRays;
    j["direct_lighting_mismatch_count"] = s.directLightingMismatchCount;
    j["direct_lighting_max_abs_error"] = s.directLightingMaxAbsError;
    j["direct_lighting_max_relative_error"] = s.directLightingMaxRelativeError;
    j["wavefront_probe_gpu_ms"] = s.wavefrontProbeGpuMs;
    j["secondary_shade_gpu_ms"] = s.secondaryShadeGpuMs;
    j["shadow_trace_gpu_ms"] = s.shadowTraceGpuMs;
}

void to_json(nlohmann::json& j, const ProfileReport::MemoryReport::HeapBudget& h) {
    j["heap_index"] = h.heapIndex;
    j["usage_bytes"] = h.usageBytes;
    j["budget_bytes"] = h.budgetBytes;
    j["allocation_bytes"] = h.allocationBytes;
    j["block_bytes"] = h.blockBytes;
    j["allocation_count"] = h.allocationCount;
    j["block_count"] = h.blockCount;
    j["usage_ratio"] = h.usageRatio;
    j["pressure"] = h.pressure;
}

void to_json(nlohmann::json& j, const ProfileReport::MemoryReport::VmaBudgetReport& b) {
    j["supported"] = b.supported;
    j["total_usage_bytes"] = b.totalUsageBytes;
    j["total_budget_bytes"] = b.totalBudgetBytes;
    j["total_allocation_bytes"] = b.totalAllocationBytes;
    j["total_block_bytes"] = b.totalBlockBytes;
    j["peak_usage_bytes"] = b.peakUsageBytes;
    j["usage_delta_bytes"] = b.usageDeltaBytes;
    j["allocation_count"] = b.allocationCount;
    j["block_count"] = b.blockCount;
    j["max_usage_ratio"] = b.maxUsageRatio;
    j["pressure"] = b.pressure;
    j["override_active"] = b.overrideActive;
    j["heaps"] = b.heaps;
    j["warnings"] = b.warnings;
}

void to_json(nlohmann::json& j, const ProfileReport::MemoryReport::DescriptorPoolReport& d) {
    j["sets_per_pool"] = d.setsPerPool;
    j["max_pools"] = d.maxPools;
    j["used_pools"] = d.usedPools;
    j["free_pools"] = d.freePools;
    j["pool_count"] = d.poolCount;
    j["capacity_sets"] = d.capacitySets;
    j["allocated_sets"] = d.allocatedSets;
    j["peak_allocated_sets"] = d.peakAllocatedSets;
    j["failed_allocations"] = d.failedAllocations;
    j["fragmented_pool_failures"] = d.fragmentedPoolFailures;
    j["pool_growth_count"] = d.poolGrowthCount;
}

void to_json(nlohmann::json& j, const ProfileReport::MemoryReport::UiReport& u) {
    j["present"] = u.present;
    j["descriptor_max_sets"] = u.descriptorMaxSets;
    j["combined_image_sampler_descriptors"] = u.combinedImageSamplerDescriptors;
    j["sampled_image_descriptors"] = u.sampledImageDescriptors;
    j["sampler_descriptors"] = u.samplerDescriptors;
    j["viewport_descriptor_allocated"] = u.viewportDescriptorAllocated;
}

void to_json(nlohmann::json& j, const ProfileReport::MemoryReport& m) {
    j["textures_bytes"] = m.texturesBytes;
    j["buffers_bytes"] = m.buffersBytes;
    j["acceleration_structure_bytes"] = m.accelerationStructureBytes;
    j["temporal_history_bytes"] = m.temporalHistoryBytes;
    j["restir_reservoir_bytes"] = m.restirReservoirBytes;
    j["restir_di_current_bytes"] = m.restirDiCurrentBytes;
    j["restir_di_initial_bytes"] = m.restirDiInitialBytes;
    j["restir_di_temporal_bytes"] = m.restirDiTemporalBytes;
    j["restir_di_previous_bytes"] = m.restirDiPreviousBytes;
    j["restir_di_spatial_bytes"] = m.restirDiSpatialBytes;
    j["restir_di_final_bytes"] = m.restirDiFinalBytes;
    j["restir_di_receiver_bytes"] = m.restirDiReceiverBytes;
    j["restir_di_previous_receiver_bytes"] = m.restirDiPreviousReceiverBytes;
    j["restir_di_counters_bytes"] = m.restirDiCountersBytes;
    j["restir_di_physical_bytes"] = m.restirDiPhysicalBytes;
    j["restir_di_alias_savings_bytes"] = m.restirDiAliasSavingsBytes;
    j["restir_gi_current_bytes"] = m.restirGiCurrentBytes;
    j["restir_gi_previous_bytes"] = m.restirGiPreviousBytes;
    j["restir_gi_spatial_bytes"] = m.restirGiSpatialBytes;
    j["restir_gi_production_temporal_bytes"] = m.restirGiProductionTemporalBytes;
    j["restir_gi_production_spatial_bytes"] = m.restirGiProductionSpatialBytes;
    j["restir_gi_production_previous_bytes"] = m.restirGiProductionPreviousBytes;
    j["restir_gi_production_upsampled_bytes"] = m.restirGiProductionUpsampledBytes;
    j["restir_gi_active_tile_mask_bytes"] = m.restirGiActiveTileMaskBytes;
    j["restir_gi_counters_bytes"] = m.restirGiCountersBytes;
    j["restir_gi_receiver_bytes"] = m.restirGiReceiverBytes;
    j["restir_gi_previous_receiver_bytes"] = m.restirGiPreviousReceiverBytes;
    j["staging_upload_total_bytes"] = m.stagingUploadTotalBytes;
    j["staging_upload_peak_bytes"] = m.stagingUploadPeakBytes;
    j["staging_upload_last_bytes"] = m.stagingUploadLastBytes;
    j["staging_upload_count"] = m.stagingUploadCount;
    j["staging_buffer_upload_count"] = m.stagingBufferUploadCount;
    j["staging_image_upload_count"] = m.stagingImageUploadCount;
    j["staging_batch_upload_count"] = m.stagingBatchUploadCount;
    j["vma_budget"] = m.vmaBudget;
    j["descriptors"] = m.descriptors;
    j["ui"] = m.ui;
}

void to_json(nlohmann::json& j, const ProfileReport::AdaptiveQualityReport& a) {
    j["smoothed_gpu_ms"] = a.smoothedGpuMs;
    j["tier"] = a.tier;
    j["over_budget_frames"] = a.overBudgetFrames;
    j["effective_max_bounces"] = a.effectiveMaxBounces;
    j["effective_environment_samples"] = a.effectiveEnvironmentSamples;
    j["effective_atrous_iterations"] = a.effectiveAtrousIterations;
    j["skip_restir_spatial"] = a.skipRestirSpatial;
    j["skip_denoiser"] = a.skipDenoiser;
}

void to_json(nlohmann::json& j, const ProfileReport::MemoryPressureQualityReport& m) {
    j["active"] = m.active;
    j["override_active"] = m.overrideActive;
    j["tier"] = m.tier;
    j["usage_ratio"] = m.usageRatio;
    j["pressure"] = m.pressure;
    j["effective_render_scale"] = m.effectiveRenderScale;
    j["limit_samples_per_pixel"] = m.limitSamplesPerPixel;
    j["restir_gi_half_resolution"] = m.restirGiHalfResolution;
    j["denoiser_max_history_length"] = m.denoiserMaxHistoryLength;
}

void to_json(nlohmann::json& j, const ProfileReport::NvidiaIntegrationReport::StreamlineFeatureReport& f) {
    j["requestable"] = f.requestable;
    j["supported"] = f.supported;
    j["unavailable_reason"] = f.unavailableReason;
    j["requirements"] = f.requirements;
}

void to_json(nlohmann::json& j, const ProfileReport::NvidiaIntegrationReport::StreamlineTagReport& t) {
    j["expected"] = t.expected;
    j["tagged"] = t.tagged;
    j["failed"] = t.failed;
    j["missing_frame_token"] = t.missingFrameToken;
    j["missing_image"] = t.missingImage;
    j["invalid_layout"] = t.invalidLayout;
    j["invalid_format"] = t.invalidFormat;
    j["invalid_extent"] = t.invalidExtent;
    j["empty_role"] = t.emptyRole;
    j["runtime_rejected"] = t.runtimeRejected;
    j["frame_index"] = t.frameIndex;
    j["attempted"] = t.attempted;
}

void to_json(nlohmann::json& j, const ProfileReport::NvidiaIntegrationReport::StreamlineEvaluationReport& e) {
    j["attempted"] = e.attempted;
    j["succeeded"] = e.succeeded;
    j["failed"] = e.failed;
    j["skipped_unsupported"] = e.skippedUnsupported;
    j["skipped_missing_tags"] = e.skippedMissingTags;
    j["frame_index"] = e.frameIndex;
}

void to_json(nlohmann::json& j, const ProfileReport::NvidiaIntegrationReport::StreamlineReflexMarkerReport& m) {
    j["attempted"] = m.attempted;
    j["succeeded"] = m.succeeded;
    j["failed"] = m.failed;
    j["skipped_unavailable"] = m.skippedUnavailable;
    j["frame_index"] = m.frameIndex;
}

void to_json(nlohmann::json& j, const ProfileReport::NvidiaIntegrationReport& n) {
    j["nrd_sdk_configured"] = n.nrdSdkConfigured;
    j["nrd_requestable"] = n.nrdRequestable;
    j["nrd_available"] = n.nrdAvailable;
    j["nrd_unavailable_reason"] = n.nrdUnavailableReason;
    j["nrd_backend_policy"] = n.nrdBackendPolicy;
    j["nrd_backend_policy_reason"] = n.nrdBackendPolicyReason;
    j["nrd_backends_mutually_exclusive"] = n.nrdBackendsMutuallyExclusive;
    j["requested_denoiser_backend"] = n.requestedDenoiserBackend;
    j["effective_denoiser_backend"] = n.effectiveDenoiserBackend;
    j["dlss_sdk_configured"] = n.dlssSdkConfigured;
    j["dlss_available"] = n.dlssAvailable;
    j["dlss_unavailable_reason"] = n.dlssUnavailableReason;
    j["dlss_ray_reconstruction_available"] = n.dlssRayReconstructionAvailable;
    j["dlss_ray_reconstruction_unavailable_reason"] = n.dlssRayReconstructionUnavailableReason;
    j["requested_dlss_ray_reconstruction"] = n.requestedDlssRayReconstruction;
    j["effective_dlss_ray_reconstruction"] = n.effectiveDlssRayReconstruction;
    j["dlss_frame_generation_available"] = n.dlssFrameGenerationAvailable;
    j["dlss_frame_generation_unavailable_reason"] = n.dlssFrameGenerationUnavailableReason;
    j["requested_dlss_frame_generation"] = n.requestedDlssFrameGeneration;
    j["effective_dlss_frame_generation"] = n.effectiveDlssFrameGeneration;
    j["dlss_sharpening_strength"] = n.dlssSharpeningStrength;
    j["requested_temporal_upscaler"] = n.requestedTemporalUpscaler;
    j["effective_temporal_upscaler"] = n.effectiveTemporalUpscaler;
    j["streamline_sdk_configured"] = n.streamlineSdkConfigured;
    j["streamline_runtime_configured"] = n.streamlineRuntimeConfigured;
    j["streamline_initialized"] = n.streamlineInitialized;
    j["streamline_vulkan_info_set"] = n.streamlineVulkanInfoSet;
    j["streamline_runtime_directory"] = n.streamlineRuntimeDirectory;
    j["streamline_unavailable_reason"] = n.streamlineUnavailableReason;
    j["requested_streamline_reflex"] = n.requestedStreamlineReflex;
    j["effective_streamline_reflex"] = n.effectiveStreamlineReflex;
    j["streamline_dlss"] = n.streamlineDlss;
    j["streamline_dlss_ray_reconstruction"] = n.streamlineDlssRayReconstruction;
    j["streamline_dlss_frame_generation"] = n.streamlineDlssFrameGeneration;
    j["streamline_reflex"] = n.streamlineReflex;
    j["streamline_nis"] = n.streamlineNis;
    j["streamline_nrd"] = n.streamlineNrd;
    j["streamline_dlss_tags"] = n.streamlineDlssTags;
    j["streamline_dlss_ray_reconstruction_tags"] = n.streamlineDlssRayReconstructionTags;
    j["streamline_dlss_evaluation"] = n.streamlineDlssEvaluation;
    j["streamline_dlss_ray_reconstruction_evaluation"] = n.streamlineDlssRayReconstructionEvaluation;
    j["streamline_reflex_markers"] = n.streamlineReflexMarkers;
}

void to_json(nlohmann::json& j, const RendererSettings& s) {
    j["render_preset"] = renderPresetName(s.renderPreset);
    j["path_tracing_enabled"] = s.pathTracingEnabled;
    j["denoiser_enabled"] = s.denoiserEnabled;
    j["denoiser_backend"] = denoiserBackendName(s.denoiserBackend);
    j["max_bounces"] = s.maxBounces;
    j["atrous_iterations"] = s.atrousIterations;
    j["restir_mode"] = restirModeName(s.restirMode);
    j["restir_di_mode"] = restirDiModeName(s.restirDiMode);
    j["restir_di_layout"] = restirDiReservoirLayoutName(s.restirDiReservoirLayout);
    j["restir_di_temporal_enabled"] = s.restirDiTemporalEnabled;
    j["restir_di_spatial_enabled"] = s.restirDiSpatialEnabled;
    j["restir_di_final_visibility_enabled"] = s.restirDiFinalVisibilityEnabled;
    j["restir_di_spatial_rounds"] = s.restirDiSpatialRounds;
    j["restir_di_spatial_radius"] = s.restirDiSpatialRadius;
    j["restir_di_temporal_max_age"] = s.restirDiTemporalMaxAge;
    j["restir_di_max_m"] = s.restirDiMaxM;
    j["restir_di_visibility_ray_budget"] = s.restirDiVisibilityRayBudget;
    j["restir_di_stabilization_enabled"] = s.restirDiProductionStabilizationEnabled;
    j["restir_di_clamp_luminance"] = s.restirDiClampLuminance;
    j["restir_di_include_sun"] = s.restirDiIncludeSun;
    j["restir_di_include_environment"] = s.restirDiIncludeEnvironment;
    j["restir_gi_enabled"] = s.restirGiEnabled;
    j["restir_gi_mode"] = restirGiModeName(s.restirGiMode);
    j["restir_gi_layout"] = restirGiReservoirLayoutName(s.restirGiReservoirLayout);
    j["tone_mapper"] = toneMapperName(s.toneMapper);
    j["exposure"] = s.exposure;
    j["render_resolution_scale"] = s.renderResolutionScale;
    j["specular_aa_enabled"] = s.specularAaEnabled;
    j["opacity_micromaps_enabled"] = s.opacityMicromapsEnabled;
    j["opacity_micromap_subdivision_level"] = s.opacityMicromapSubdivisionLevel;
    j["wavefront_queues_enabled"] = s.wavefrontQueuesEnabled;
    j["wavefront_primary_generate_enabled"] = s.wavefrontPrimaryGenerateEnabled;
    j["wavefront_trace_enabled"] = s.wavefrontTraceEnabled;
    j["wavefront_shade_enabled"] = s.wavefrontShadeEnabled;
    j["wavefront_shadow_trace_enabled"] = s.wavefrontShadowTraceEnabled;
    j["wavefront_compact_enabled"] = s.wavefrontCompactEnabled;
    j["wavefront_sort_enabled"] = s.wavefrontSortEnabled;
    j["wavefront_final_output_enabled"] = s.wavefrontFinalOutputEnabled;
    j["shader_execution_reordering_enabled"] = s.shaderExecutionReorderingEnabled;
    j["dof_aperture_radius"] = s.dofApertureRadius;
    j["dof_focus_distance"] = s.dofFocusDistance;
    j["dof_blade_count"] = s.dofBladeCount;
    j["dof_bokeh_rotation"] = s.dofBokehRotation;
    j["motion_blur_enabled"] = s.motionBlurEnabled;
    j["motion_blur_shutter_open"] = s.motionBlurShutterOpen;
    j["motion_blur_shutter_close"] = s.motionBlurShutterClose;
    j["homogeneous_volume_enabled"] = s.homogeneousVolumeEnabled;
    j["homogeneous_volume_scattering"] = s.homogeneousVolumeScattering;
    j["homogeneous_volume_absorption"] = s.homogeneousVolumeAbsorption;
    j["homogeneous_volume_anisotropy"] = s.homogeneousVolumeAnisotropy;
    j["height_fog_enabled"] = s.heightFogEnabled;
    j["height_fog_density"] = s.heightFogDensity;
    j["height_fog_height_falloff"] = s.heightFogHeightFalloff;
    j["height_fog_color"] = {s.heightFogColor.r, s.heightFogColor.g, s.heightFogColor.b};
    j["mnee_caustics_enabled"] = s.mneeCausticsEnabled;
    j["camera_jitter_enabled"] = s.cameraJitterEnabled;
    j["denoise_while_moving"] = s.denoiseWhileMoving;
    j["samples_per_pixel"] = s.samplesPerPixel;
    j["limit_samples_per_pixel"] = s.limitSamplesPerPixel;
    j["effective_samples_per_pixel"] = s.limitSamplesPerPixel ? 1u : s.samplesPerPixel;
    j["taa_enabled"] = s.taaEnabled;
    j["temporal_upscaler"] = temporalUpscalerName(s.temporalUpscaler);
    j["dlss_frame_generation_enabled"] = s.dlssFrameGenerationEnabled;
    j["dlss_ray_reconstruction_enabled"] = s.dlssRayReconstructionEnabled;
    j["streamline_reflex_enabled"] = s.streamlineReflexEnabled;
    j["dlss_sharpening_strength"] = s.dlssSharpeningStrength;
    j["taa_feedback"] = s.taaFeedback;
    j["taa_motion_feedback"] = s.taaMotionFeedback;
    j["taa_reactive_feedback"] = s.taaReactiveFeedback;
    j["taa_sharpening_strength"] = s.taaSharpeningStrength;
    j["sunlight_enabled"] = s.sunlightEnabled;
    j["direct_lighting_enabled"] = s.directLightingEnabled;
    j["environment_enabled"] = s.environmentEnabled;
    j["environment_direct_samples"] = s.environmentDirectSamples;
    j["denoiser_strength"] = s.denoiserStrength;
    j["denoiser_max_history_length"] = s.denoiserMaxHistoryLength;
    j["moment_validity_threshold"] = s.momentValidityThreshold;
    j["sun_intensity"] = s.sunIntensity;
    j["sun_elevation"] = s.sunElevation;
    j["sun_azimuth"] = s.sunAzimuth;
    j["gamma"] = s.gamma;
    j["contrast"] = s.contrast;
    j["saturation"] = s.saturation;
    j["brightness"] = s.brightness;
    j["white_point"] = s.whitePoint;
    j["auto_exposure_enabled"] = s.autoExposureEnabled;
    j["debug_view"] = rendererDebugViewName(s.debugView);
    j["restir_gi_temporal_max_age"] = s.restirGiTemporalMaxAge;
    j["restir_gi_spatial_rounds"] = s.restirGiSpatialRounds;
    j["restir_gi_spatial_radius"] = s.restirGiSpatialRadius;
    j["restir_gi_depth_threshold_scale"] = s.restirGiDepthThresholdScale;
    j["restir_gi_spatial_compatibility_threshold"] = s.restirGiSpatialCompatibilityThreshold;
    j["restir_gi_half_resolution"] = s.restirGiHalfResolution;
    j["restir_gi_visibility_ray_budget"] = s.restirGiVisibilityRayBudget;
    j["restir_gi_final_stabilization_enabled"] = s.restirGiFinalStabilizationEnabled;
    j["restir_gi_min_final_blend_strength"] = s.restirGiMinFinalBlendStrength;
    j["restir_gi_active_tile_mask_mode"] = restirGiActiveTileMaskModeName(s.restirGiActiveTileMaskMode);
    j["restir_history_copy_mode_requested"] = restirHistoryCopyModeName(s.restirHistoryCopyMode);
    j["restir_counter_mode"] = s.restirCounterMode == RestirCounterMode::On ? "on"
        : s.restirCounterMode == RestirCounterMode::Off ? "off" : "auto";
    j["adaptive_quality_mode"] = s.adaptiveQualityMode == AdaptiveQualityMode::Off ? "off"
        : s.adaptiveQualityMode == AdaptiveQualityMode::Conservative ? "conservative"
        : s.adaptiveQualityMode == AdaptiveQualityMode::Balanced ? "balanced" : "aggressive";
    j["adaptive_gpu_frame_target_ms"] = s.adaptiveGpuFrameTargetMs;
}

namespace {

std::string formatVulkanVersion(uint32_t version) {
    std::ostringstream ss;
    ss << VK_VERSION_MAJOR(version) << "." << VK_VERSION_MINOR(version) << "." << VK_VERSION_PATCH(version);
    return ss.str();
}

std::string sharingModeName(VkSharingMode mode) {
    return mode == VK_SHARING_MODE_CONCURRENT ? "concurrent" : "exclusive";
}

std::string serReorderingHintName(VkRayTracingInvocationReorderModeNV hint) {
    switch (hint) {
        case VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_NV: return "reorder";
        case VK_RAY_TRACING_INVOCATION_REORDER_MODE_NONE_NV: return "none";
        default: return "unknown";
    }
}

float percentileOfSorted(const std::vector<float>& sorted, float p) {
    if (sorted.empty()) {
        return 0.0f;
    }
    const float scaled = std::clamp(p, 0.0f, 1.0f) * static_cast<float>(sorted.size() - 1u);
    const size_t lower = static_cast<size_t>(std::floor(scaled));
    const size_t upper = std::min(sorted.size() - 1u, lower + 1u);
    const float t = scaled - static_cast<float>(lower);
    return sorted[lower] * (1.0f - t) + sorted[upper] * t;
}

ProfileReport::MinMaxAvg computeMinMaxAvg(const std::vector<float>& values, uint32_t warmupFrames) {
    ProfileReport::MinMaxAvg result{};
    if (values.empty()) return result;
    size_t startIdx = std::min(static_cast<size_t>(warmupFrames), values.size());
    if (startIdx >= values.size()) { startIdx = 0; }
    size_t count = values.size() - startIdx;
    if (count == 0) { count = values.size(); startIdx = 0; }
    result.min = *std::min_element(values.begin() + startIdx, values.end());
    result.max = *std::max_element(values.begin() + startIdx, values.end());
    double sum = 0.0;
    for (size_t i = startIdx; i < values.size(); ++i) sum += values[i];
    result.avg = static_cast<float>(sum / count);

    std::vector<float> sorted(values.begin() + startIdx, values.end());
    std::sort(sorted.begin(), sorted.end());
    result.p95 = percentileOfSorted(sorted, 0.95f);
    result.p99 = percentileOfSorted(sorted, 0.99f);
    return result;
}

float percentileGpuTiming(
    const std::vector<GpuFrameTimings>& values,
    uint32_t warmupFrames,
    float GpuFrameTimings::*member,
    float percentile) {
    if (values.empty()) {
        return 0.0f;
    }

    size_t startIdx = std::min(static_cast<size_t>(warmupFrames), values.size());
    if (startIdx >= values.size()) {
        startIdx = 0;
    }
    if (startIdx >= values.size()) {
        return 0.0f;
    }

    std::vector<float> sorted;
    sorted.reserve(values.size() - startIdx);
    for (size_t i = startIdx; i < values.size(); ++i) {
        sorted.push_back(values[i].*member);
    }
    std::sort(sorted.begin(), sorted.end());
    return percentileOfSorted(sorted, percentile);
}

GpuFrameTimings percentileGpuTimings(
    const std::vector<GpuFrameTimings>& values,
    uint32_t warmupFrames,
    float percentile) {
    GpuFrameTimings result{};
    result.pathTraceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::pathTraceMs, percentile);
    result.restirHistoryClearMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirHistoryClearMs, percentile);
    result.restirGiClearMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiClearMs, percentile);
    result.restirGiTemporalMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiTemporalMs, percentile);
    result.restirSpatialMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirSpatialMs, percentile);
    result.restirSpatialCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirSpatialCopyMs, percentile);
    result.restirGiSpatialMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiSpatialMs, percentile);
    result.restirGiUpsampleMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiUpsampleMs, percentile);
    result.restirGiFinalMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiFinalMs, percentile);
    result.restirGiCountersReadbackMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiCountersReadbackMs, percentile);
    result.restirDiTemporalMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirDiTemporalMs, percentile);
    result.restirDiSpatialMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirDiSpatialMs, percentile);
    result.restirDiFinalMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirDiFinalMs, percentile);
    result.restirDiHistoryCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirDiHistoryCopyMs, percentile);
    result.restirDiCountersReadbackMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirDiCountersReadbackMs, percentile);
    result.fogIntegrateMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::fogIntegrateMs, percentile);
    result.atmosphereMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereMs, percentile);
    result.atmosphereTransmittanceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereTransmittanceMs, percentile);
    result.atmosphereMultiScatterMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereMultiScatterMs, percentile);
    result.atmosphereSkyViewMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereSkyViewMs, percentile);
    result.atmosphereSkyReprojectMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereSkyReprojectMs, percentile);
    result.atmosphereSkyCdfMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereSkyCdfMs, percentile);
    result.atmosphereAerialPerspectiveMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereAerialPerspectiveMs, percentile);
    result.denoiserMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::denoiserMs, percentile);
    result.momentUpdateMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::momentUpdateMs, percentile);
    result.historyCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::historyCopyMs, percentile);
    result.skipDenoiserCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::skipDenoiserCopyMs, percentile);
    result.taaMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::taaMs, percentile);
    result.taaHistoryCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::taaHistoryCopyMs, percentile);
    result.autoExposureMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::autoExposureMs, percentile);
    result.autoExposureHistogramClearMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::autoExposureHistogramClearMs, percentile);
    result.autoExposureHistogramMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::autoExposureHistogramMs, percentile);
    result.autoExposureReduceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::autoExposureReduceMs, percentile);
    result.toneMapMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::toneMapMs, percentile);
    result.selectionOutlineMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::selectionOutlineMs, percentile);
    result.fullscreenMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::fullscreenMs, percentile);
    result.editorPresentationMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::editorPresentationMs, percentile);
    result.dynamicBlasUpdateMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::dynamicBlasUpdateMs, percentile);
    result.wavefrontTraceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontTraceMs, percentile);
    result.wavefrontSecondaryTraceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontSecondaryTraceMs, percentile);
    result.wavefrontSortedTraceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontSortedTraceMs, percentile);
    result.wavefrontShadowTraceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontShadowTraceMs, percentile);
    result.wavefrontShadeMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontShadeMs, percentile);
    result.wavefrontSecondaryShadeMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontSecondaryShadeMs, percentile);
    result.wavefrontSortedShadeMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontSortedShadeMs, percentile);
    result.wavefrontCompactMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontCompactMs, percentile);
    result.wavefrontSortMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::wavefrontSortMs, percentile);
    result.graphicsLaneMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::graphicsLaneMs, percentile);
    result.rayTracingLaneMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::rayTracingLaneMs, percentile);
    result.computeLaneMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::computeLaneMs, percentile);
    result.queueWaitMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::queueWaitMs, percentile);
    return result;
}

void assignQueueLaneMs(ProfileReport::QueueLaneMs& out, const GpuFrameTimings& timings) {
    out.graphics = timings.graphicsLaneMs;
    out.rayTracing = timings.rayTracingLaneMs;
    out.compute = timings.computeLaneMs;
    out.queueWait = timings.queueWaitMs;
}

void assignPerPassGpuMs(ProfileReport::PerPassGpuMs& out, const GpuFrameTimings& timings) {
    out.pathTrace = timings.pathTraceMs;
    out.restirHistoryClear = timings.restirHistoryClearMs;
    out.restirGiClear = timings.restirGiClearMs;
    out.restirGiTemporal = timings.restirGiTemporalMs;
    out.restirSpatial = timings.restirSpatialMs;
    out.restirSpatialCopy = timings.restirSpatialCopyMs;
    out.restirGiSpatial = timings.restirGiSpatialMs;
    out.restirGiUpsample = timings.restirGiUpsampleMs;
    out.restirGiFinal = timings.restirGiFinalMs;
    out.restirGiCountersReadback = timings.restirGiCountersReadbackMs;
    out.restirDiTemporal = timings.restirDiTemporalMs;
    out.restirDiSpatial = timings.restirDiSpatialMs;
    out.restirDiFinal = timings.restirDiFinalMs;
    out.restirDiHistoryCopy = timings.restirDiHistoryCopyMs;
    out.restirDiCountersReadback = timings.restirDiCountersReadbackMs;
    out.fogIntegrate = timings.fogIntegrateMs;
    out.atmosphere = timings.atmosphereMs;
    out.atmosphereTransmittance = timings.atmosphereTransmittanceMs;
    out.atmosphereMultiScatter = timings.atmosphereMultiScatterMs;
    out.atmosphereSkyView = timings.atmosphereSkyViewMs;
    out.atmosphereSkyReproject = timings.atmosphereSkyReprojectMs;
    out.atmosphereSkyCdf = timings.atmosphereSkyCdfMs;
    out.atmosphereAerialPerspective = timings.atmosphereAerialPerspectiveMs;
    out.denoiser = timings.denoiserMs;
    out.momentUpdate = timings.momentUpdateMs;
    out.historyCopy = timings.historyCopyMs;
    out.skipDenoiserCopy = timings.skipDenoiserCopyMs;
    out.taa = timings.taaMs;
    out.taaHistoryCopy = timings.taaHistoryCopyMs;
    out.autoExposureHistogramClear = timings.autoExposureHistogramClearMs;
    out.autoExposureHistogram = timings.autoExposureHistogramMs;
    out.autoExposureReduce = timings.autoExposureReduceMs;
    out.toneMap = timings.toneMapMs;
    out.selectionOutline = timings.selectionOutlineMs;
    out.fullscreen = timings.fullscreenMs;
    out.editorPresentation = timings.editorPresentationMs;
    out.dynamicBlasUpdate = timings.dynamicBlasUpdateMs;
    out.wavefrontTrace = timings.wavefrontTraceMs;
    out.wavefrontSecondaryTrace = timings.wavefrontSecondaryTraceMs;
    out.wavefrontSortedTrace = timings.wavefrontSortedTraceMs;
    out.wavefrontShadowTrace = timings.wavefrontShadowTraceMs;
    out.wavefrontShade = timings.wavefrontShadeMs;
    out.wavefrontSecondaryShade = timings.wavefrontSecondaryShadeMs;
    out.wavefrontSortedShade = timings.wavefrontSortedShadeMs;
    out.wavefrontCompact = timings.wavefrontCompactMs;
    out.wavefrontSort = timings.wavefrontSortMs;
}

GpuFrameTimings averageGpuTimings(const std::vector<GpuFrameTimings>& values, uint32_t warmupFrames) {
    GpuFrameTimings result{};
    if (values.empty()) {
        return result;
    }

    size_t startIdx = std::min(static_cast<size_t>(warmupFrames), values.size());
    if (startIdx >= values.size()) {
        startIdx = 0;
    }
    const size_t count = values.size() - startIdx;
    if (count == 0) {
        return result;
    }

    for (size_t i = startIdx; i < values.size(); ++i) {
        result.pathTraceMs += values[i].pathTraceMs;
        result.restirHistoryClearMs += values[i].restirHistoryClearMs;
        result.restirGiClearMs += values[i].restirGiClearMs;
        result.restirGiTemporalMs += values[i].restirGiTemporalMs;
        result.restirSpatialMs += values[i].restirSpatialMs;
        result.restirSpatialCopyMs += values[i].restirSpatialCopyMs;
        result.restirGiSpatialMs += values[i].restirGiSpatialMs;
        result.restirGiUpsampleMs += values[i].restirGiUpsampleMs;
        result.restirGiFinalMs += values[i].restirGiFinalMs;
        result.restirGiCountersReadbackMs += values[i].restirGiCountersReadbackMs;
        result.restirDiTemporalMs += values[i].restirDiTemporalMs;
        result.restirDiSpatialMs += values[i].restirDiSpatialMs;
        result.restirDiFinalMs += values[i].restirDiFinalMs;
        result.restirDiHistoryCopyMs += values[i].restirDiHistoryCopyMs;
        result.restirDiCountersReadbackMs += values[i].restirDiCountersReadbackMs;
        result.fogIntegrateMs += values[i].fogIntegrateMs;
        result.atmosphereMs += values[i].atmosphereMs;
        result.atmosphereTransmittanceMs += values[i].atmosphereTransmittanceMs;
        result.atmosphereMultiScatterMs += values[i].atmosphereMultiScatterMs;
        result.atmosphereSkyViewMs += values[i].atmosphereSkyViewMs;
        result.atmosphereSkyReprojectMs += values[i].atmosphereSkyReprojectMs;
        result.atmosphereSkyCdfMs += values[i].atmosphereSkyCdfMs;
        result.atmosphereAerialPerspectiveMs += values[i].atmosphereAerialPerspectiveMs;
        result.denoiserMs += values[i].denoiserMs;
        result.momentUpdateMs += values[i].momentUpdateMs;
        result.historyCopyMs += values[i].historyCopyMs;
        result.skipDenoiserCopyMs += values[i].skipDenoiserCopyMs;
        result.taaMs += values[i].taaMs;
        result.taaHistoryCopyMs += values[i].taaHistoryCopyMs;
        result.autoExposureMs += values[i].autoExposureMs;
        result.autoExposureHistogramClearMs += values[i].autoExposureHistogramClearMs;
        result.autoExposureHistogramMs += values[i].autoExposureHistogramMs;
        result.autoExposureReduceMs += values[i].autoExposureReduceMs;
        result.toneMapMs += values[i].toneMapMs;
        result.selectionOutlineMs += values[i].selectionOutlineMs;
        result.fullscreenMs += values[i].fullscreenMs;
        result.editorPresentationMs += values[i].editorPresentationMs;
        result.dynamicBlasUpdateMs += values[i].dynamicBlasUpdateMs;
        result.wavefrontTraceMs += values[i].wavefrontTraceMs;
        result.wavefrontSecondaryTraceMs += values[i].wavefrontSecondaryTraceMs;
        result.wavefrontSortedTraceMs += values[i].wavefrontSortedTraceMs;
        result.wavefrontShadowTraceMs += values[i].wavefrontShadowTraceMs;
        result.wavefrontShadeMs += values[i].wavefrontShadeMs;
        result.wavefrontSecondaryShadeMs += values[i].wavefrontSecondaryShadeMs;
        result.wavefrontSortedShadeMs += values[i].wavefrontSortedShadeMs;
        result.wavefrontCompactMs += values[i].wavefrontCompactMs;
        result.wavefrontSortMs += values[i].wavefrontSortMs;
        result.graphicsLaneMs += values[i].graphicsLaneMs;
        result.rayTracingLaneMs += values[i].rayTracingLaneMs;
        result.computeLaneMs += values[i].computeLaneMs;
        result.queueWaitMs += values[i].queueWaitMs;
    }

    const float invCount = 1.0f / static_cast<float>(count);
    result.pathTraceMs *= invCount;
    result.restirHistoryClearMs *= invCount;
    result.restirGiClearMs *= invCount;
    result.restirGiTemporalMs *= invCount;
    result.restirSpatialMs *= invCount;
    result.restirSpatialCopyMs *= invCount;
    result.restirGiSpatialMs *= invCount;
    result.restirGiUpsampleMs *= invCount;
    result.restirGiFinalMs *= invCount;
    result.restirGiCountersReadbackMs *= invCount;
    result.restirDiTemporalMs *= invCount;
    result.restirDiSpatialMs *= invCount;
    result.restirDiFinalMs *= invCount;
    result.restirDiHistoryCopyMs *= invCount;
    result.restirDiCountersReadbackMs *= invCount;
    result.fogIntegrateMs *= invCount;
    result.atmosphereMs *= invCount;
    result.atmosphereTransmittanceMs *= invCount;
    result.atmosphereMultiScatterMs *= invCount;
    result.atmosphereSkyViewMs *= invCount;
    result.atmosphereSkyReprojectMs *= invCount;
    result.atmosphereSkyCdfMs *= invCount;
    result.atmosphereAerialPerspectiveMs *= invCount;
    result.denoiserMs *= invCount;
    result.momentUpdateMs *= invCount;
    result.historyCopyMs *= invCount;
    result.skipDenoiserCopyMs *= invCount;
    result.taaMs *= invCount;
    result.taaHistoryCopyMs *= invCount;
    result.autoExposureMs *= invCount;
    result.autoExposureHistogramClearMs *= invCount;
    result.autoExposureHistogramMs *= invCount;
    result.autoExposureReduceMs *= invCount;
    result.toneMapMs *= invCount;
    result.selectionOutlineMs *= invCount;
    result.fullscreenMs *= invCount;
    result.editorPresentationMs *= invCount;
    result.dynamicBlasUpdateMs *= invCount;
    result.wavefrontTraceMs *= invCount;
    result.wavefrontSecondaryTraceMs *= invCount;
    result.wavefrontSortedTraceMs *= invCount;
    result.wavefrontShadowTraceMs *= invCount;
    result.wavefrontShadeMs *= invCount;
    result.wavefrontSecondaryShadeMs *= invCount;
    result.wavefrontSortedShadeMs *= invCount;
    result.wavefrontCompactMs *= invCount;
    result.wavefrontSortMs *= invCount;
    result.graphicsLaneMs *= invCount;
    result.rayTracingLaneMs *= invCount;
    result.computeLaneMs *= invCount;
    result.queueWaitMs *= invCount;
    return result;
}

uint32_t countDynamicBlasOverBudgetFrames(
    const std::vector<GpuFrameTimings>& values,
    uint32_t warmupFrames,
    float budgetMs) {
    if (values.empty() || budgetMs <= 0.0f) {
        return 0;
    }

    size_t startIdx = std::min(static_cast<size_t>(warmupFrames), values.size());
    if (startIdx >= values.size()) {
        startIdx = 0;
    }

    uint32_t count = 0;
    for (size_t i = startIdx; i < values.size(); ++i) {
        if (values[i].dynamicBlasUpdateMs > budgetMs) {
            ++count;
        }
    }
    return count;
}

void writeValidationLog(const RendererValidationLog& log, const std::filesystem::path& path) {
    std::ofstream file(path);
    if (!file.is_open()) return;
    file << "=== Validation Log ===\n\n";
    file << "--- Pass Events (" << log.passEvents().size() << ") ---\n";
    for (const auto& ev : log.passEvents()) { file << "  " << ev << "\n"; }
    file << "\n--- Barrier Events (" << log.barrierEvents().size() << ") ---\n";
    for (const auto& ev : log.barrierEvents()) { file << "  " << ev << "\n"; }
    file << "\n--- Accumulation Invalidations (" << log.invalidations().size() << ") ---\n";
    for (const auto& ev : log.invalidations()) { file << "  frame=" << ev.frame << " reason=" << ev.reason << "\n"; }
    file << "\n--- Scene Update Routes (" << log.sceneUpdateRoutes().size() << ") ---\n";
    for (const auto& ev : log.sceneUpdateRoutes()) {
        const double avgCpuMs = ev.count > 0 ? ev.totalCpuMs / static_cast<double>(ev.count) : 0.0;
        file << "  " << ev.kind << ": " << ev.action
             << " (count=" << ev.count
             << ", cpu_ms last=" << ev.lastCpuMs
             << " avg=" << avgCpuMs
             << " min=" << ev.minCpuMs
             << " max=" << ev.maxCpuMs
             << ")\n";
    }
    file << "\n--- Resource States (" << log.resourceStateEvents().size() << ") ---\n";
    for (const auto& ev : log.resourceStateEvents()) {
        file << "  " << ev.resource << " " << ev.beforePass << " -> " << ev.afterPass
             << " [layout: " << static_cast<int>(ev.beforeLayout) << " -> " << static_cast<int>(ev.afterLayout)
             << ", stage: " << static_cast<uint64_t>(ev.beforeStage) << " -> " << static_cast<uint64_t>(ev.afterStage)
             << ", access: " << static_cast<uint64_t>(ev.beforeAccess) << " -> " << static_cast<uint64_t>(ev.afterAccess) << "]\n";
    }
}

void writeSettingsJson(const RendererSettings& settings, const std::filesystem::path& path) {
    nlohmann::json j;
    to_json(j, settings);
    std::ofstream file(path);
    if (file.is_open()) { file << j.dump(2); }
}

std::string sanitizeSceneName(std::string name) {
    for (char& ch : name) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        } else {
            ch = '_';
        }
    }
    return name;
}

std::string sequenceFrameFileName(uint32_t frameIndex) {
    std::ostringstream stream;
    stream << "frame_" << std::setw(4) << std::setfill('0') << frameIndex << ".png";
    return stream.str();
}

} // namespace

HeadlessDiagnostics::HeadlessDiagnostics(const HeadlessDiagnosticsConfig& config)
    : config_(config) {
    if (config.profileJsonPath.has_value()) {
        profileJsonPath_ = *config.profileJsonPath;
    }
}

HeadlessDiagnostics::~HeadlessDiagnostics() {
    if (logCapture_) {
        (void)releaseStdout();
    }
}

ProfileReport HeadlessDiagnostics::run(Application& app) {
    auto* renderer = app.pathTracer();
    auto* context = app.vulkanContext();

    if (renderer == nullptr || context == nullptr) { return profileReport_; }
    profileReport_ = ProfileReport{};

    VkPhysicalDeviceProperties props = context->physicalDeviceProperties();
    profileReport_.gpuName = props.deviceName;
    profileReport_.vulkanVersion = formatVulkanVersion(props.apiVersion);
    profileReport_.driverVersion = std::to_string(props.driverVersion);
    profileReport_.resolution.renderWidth = renderer->renderExtent().width;
    profileReport_.resolution.renderHeight = renderer->renderExtent().height;
    profileReport_.resolution.displayWidth = renderer->displayExtent().width;
    profileReport_.resolution.displayHeight = renderer->displayExtent().height;
    profileReport_.resolution.renderScale = renderer->effectiveRenderResolutionScale();
    profileReport_.warmupFrames = config_.warmupFrames;
    profileReport_.frameCount = config_.totalFrames;
    profileReport_.profiledFrames = config_.totalFrames > config_.warmupFrames
        ? config_.totalFrames - config_.warmupFrames : 0;

    const auto& cpuTimings = app.cpuFrameTimings();
    const auto& gpuTimingsVec = app.gpuFrameTimings();
    uint32_t warmup = app.warmupFrameCount();
    profileReport_.cpuFrameMs = computeMinMaxAvg(cpuTimings, warmup);
    profileReport_.gpuFrameMs = computeMinMaxAvg(gpuTimingsVec, warmup);

    const auto timings = averageGpuTimings(app.perFrameGpuTimings(), warmup);
    const auto frameWorkScheduler = app.frameWorkSchedulerSnapshot();
    assignPerPassGpuMs(profileReport_.perPassGpuMs, timings);
    assignQueueLaneMs(profileReport_.queueLaneMs, timings);
    assignPerPassGpuMs(profileReport_.perPassGpuMsP95, percentileGpuTimings(app.perFrameGpuTimings(), warmup, 0.95f));
    assignPerPassGpuMs(profileReport_.perPassGpuMsP99, percentileGpuTimings(app.perFrameGpuTimings(), warmup, 0.99f));

    const auto& queueFamilies = context->queueFamilies();
    profileReport_.asyncCompute.disabledByCli = config_.disableAsyncCompute;
    profileReport_.asyncCompute.singleQueueFallback = config_.singleQueueFallback;
    profileReport_.asyncCompute.timelineSemaphore = context->supportsTimelineSemaphore();
    profileReport_.asyncCompute.independentQueue = context->hasIndependentComputeQueue();
    profileReport_.asyncCompute.dedicatedComputeFamily = queueFamilies.hasDedicatedCompute();
    profileReport_.asyncCompute.graphicsFamily = queueFamilies.graphics;
    profileReport_.asyncCompute.computeFamily = queueFamilies.compute;
    profileReport_.asyncCompute.computeQueueIndex = queueFamilies.computeQueueIndex;
    profileReport_.asyncCompute.crossFamily = queueFamilies.graphics.has_value() &&
        queueFamilies.compute.has_value() &&
        queueFamilies.graphics.value() != queueFamilies.compute.value();
    profileReport_.asyncCompute.enabled = !profileReport_.asyncCompute.disabledByCli &&
        !profileReport_.asyncCompute.singleQueueFallback &&
        context->computeQueue() != VK_NULL_HANDLE &&
        profileReport_.asyncCompute.independentQueue &&
        queueFamilies.compute.has_value() &&
        profileReport_.asyncCompute.timelineSemaphore;
    const auto& ommInfo = context->opacityMicromapInfo();
    profileReport_.opacityMicromap.supported = ommInfo.supported;
    profileReport_.opacityMicromap.extensionSupported = ommInfo.extensionSupported;
    profileReport_.opacityMicromap.micromapFeature = ommInfo.micromapFeature;
    profileReport_.opacityMicromap.captureReplay = ommInfo.captureReplay;
    profileReport_.opacityMicromap.hostCommands = ommInfo.hostCommands;
    profileReport_.opacityMicromap.maxOpacity2StateSubdivisionLevel = ommInfo.maxOpacity2StateSubdivisionLevel;
    profileReport_.opacityMicromap.maxOpacity4StateSubdivisionLevel = ommInfo.maxOpacity4StateSubdivisionLevel;
    profileReport_.opacityMicromap.disabledReason = ommInfo.disabledReason;
    const auto& serInfo = context->serInfo();
    profileReport_.shaderExecutionReordering.supported = serInfo.supported;
    profileReport_.shaderExecutionReordering.enabled = renderer->settings().shaderExecutionReorderingEnabled && serInfo.supported;
    profileReport_.shaderExecutionReordering.extensionSupported = serInfo.extensionSupported;
    profileReport_.shaderExecutionReordering.invocationReorderFeature = serInfo.invocationReorderFeature;
    profileReport_.shaderExecutionReordering.dedicatedSerPipeline = profileReport_.shaderExecutionReordering.enabled;
    profileReport_.shaderExecutionReordering.pipelineCreateFlagRequired = false;
    profileReport_.shaderExecutionReordering.maxInvocationReorderDepthReported = serInfo.maxInvocationReorderDepthReported;
    profileReport_.shaderExecutionReordering.maxRayTracingInvocationReorderDepth = serInfo.maxRayTracingInvocationReorderDepth;
    profileReport_.shaderExecutionReordering.performanceEvidenceAvailable = false;
    profileReport_.shaderExecutionReordering.performanceTargetPassed = false;
    profileReport_.shaderExecutionReordering.performanceTargetMinPercent = 20.0f;
    profileReport_.shaderExecutionReordering.performanceTargetMaxPercent = 40.0f;
    profileReport_.shaderExecutionReordering.observedImprovementPercent = 0.0f;
    profileReport_.shaderExecutionReordering.reorderingHint = serReorderingHintName(serInfo.reorderingHint);
    profileReport_.shaderExecutionReordering.disabledReason = serInfo.disabledReason;
    const auto& motionBlurInfo = context->rayTracingMotionBlurInfo();
    profileReport_.rayTracingMotionBlur.supported = motionBlurInfo.supported;
    profileReport_.rayTracingMotionBlur.enabled = renderer->settings().motionBlurEnabled && motionBlurInfo.supported;
    profileReport_.rayTracingMotionBlur.extensionSupported = motionBlurInfo.extensionSupported;
    profileReport_.rayTracingMotionBlur.rayTracingMotionBlurFeature = motionBlurInfo.rayTracingMotionBlurFeature;
    profileReport_.rayTracingMotionBlur.rayTracingMotionBlurPipelineTraceRaysIndirect = motionBlurInfo.rayTracingMotionBlurPipelineTraceRaysIndirect;
    profileReport_.rayTracingMotionBlur.disabledReason = motionBlurInfo.disabledReason;
    if (auto* allocator = app.resourceAllocator()) {
        profileReport_.asyncCompute.resourceSharingMode = sharingModeName(allocator->graphicsComputeSharingMode());
        profileReport_.asyncCompute.resourceSharingQueueFamilyCount = allocator->graphicsComputeQueueFamilyCount();
        const uint32_t* families = allocator->graphicsComputeQueueFamilies();
        profileReport_.asyncCompute.resourceSharingQueueFamilies.assign(
            families,
            families + profileReport_.asyncCompute.resourceSharingQueueFamilyCount);

        const auto budget = allocator->memoryBudgetReport();
        profileReport_.memory.vmaBudget.supported = budget.supported;
        profileReport_.memory.vmaBudget.totalUsageBytes = budget.totalUsageBytes;
        profileReport_.memory.vmaBudget.totalBudgetBytes = budget.totalBudgetBytes;
        profileReport_.memory.vmaBudget.totalAllocationBytes = budget.totalAllocationBytes;
        profileReport_.memory.vmaBudget.totalBlockBytes = budget.totalBlockBytes;
        profileReport_.memory.vmaBudget.peakUsageBytes = budget.peakUsageBytes;
        profileReport_.memory.vmaBudget.usageDeltaBytes = budget.usageDeltaBytes;
        profileReport_.memory.vmaBudget.allocationCount = budget.allocationCount;
        profileReport_.memory.vmaBudget.blockCount = budget.blockCount;
        profileReport_.memory.vmaBudget.maxUsageRatio = budget.maxUsageRatio;
        profileReport_.memory.vmaBudget.pressure = budget.pressure;
        profileReport_.memory.vmaBudget.overrideActive = budget.overrideActive;
        profileReport_.memory.vmaBudget.warnings = budget.warnings;
        profileReport_.memory.vmaBudget.heaps.reserve(budget.heaps.size());
        for (const auto& heap : budget.heaps) {
            profileReport_.memory.vmaBudget.heaps.push_back({
                .heapIndex = heap.heapIndex,
                .usageBytes = heap.usageBytes,
                .budgetBytes = heap.budgetBytes,
                .allocationBytes = heap.allocationBytes,
                .blockBytes = heap.blockBytes,
                .allocationCount = heap.allocationCount,
                .blockCount = heap.blockCount,
                .usageRatio = heap.usageRatio,
                .pressure = heap.pressure,
            });
        }
        profileReport_.warnings.insert(
            profileReport_.warnings.end(),
            budget.warnings.begin(),
            budget.warnings.end());
    }

    const auto& stats = renderer->pipelineStats();
    profileReport_.pipelineStatistics.rayInvocations = stats.rayInvocations;
    profileReport_.pipelineStatistics.triangleHits = stats.triangleHits;
    profileReport_.pipelineStatistics.aabbHits = stats.aabbHits;

    const auto rtCounters = renderer->rayTracingDiagnosticCounters();
    profileReport_.rayTracingDiagnosticCounters.cameraAnyHitInvocations = rtCounters.cameraAnyHitInvocations;
    profileReport_.rayTracingDiagnosticCounters.cameraAnyHitIgnored = rtCounters.cameraAnyHitIgnored;
    profileReport_.rayTracingDiagnosticCounters.cameraAnyHitAccepted = rtCounters.cameraAnyHitAccepted;
    profileReport_.rayTracingDiagnosticCounters.shadowAnyHitInvocations = rtCounters.shadowAnyHitInvocations;
    profileReport_.rayTracingDiagnosticCounters.shadowAnyHitIgnored = rtCounters.shadowAnyHitIgnored;
    profileReport_.rayTracingDiagnosticCounters.shadowAnyHitAccepted = rtCounters.shadowAnyHitAccepted;
    profileReport_.rayTracingDiagnosticCounters.surfaceTraceRays = rtCounters.surfaceTraceRays;
    profileReport_.rayTracingDiagnosticCounters.shadowTraceRays = rtCounters.shadowTraceRays;
    profileReport_.rayTracingDiagnosticCounters.closestHitInvocations = rtCounters.closestHitInvocations;
    profileReport_.rayTracingDiagnosticCounters.closestHitAlphaMaterials = rtCounters.closestHitAlphaMaterials;
    profileReport_.rayTracingDiagnosticCounters.causticShadowAttempts = rtCounters.causticShadowAttempts;
    profileReport_.rayTracingDiagnosticCounters.causticTransmissiveHits = rtCounters.causticTransmissiveHits;
    profileReport_.rayTracingDiagnosticCounters.causticTransmissiveVisible = rtCounters.causticTransmissiveVisible;
    profileReport_.rayTracingDiagnosticCounters.causticShadowBlocked = rtCounters.causticShadowBlocked;

    const auto& rtStats = renderer->rayTracingStats();
    profileReport_.memory.accelerationStructureBytes = static_cast<uint64_t>(rtStats.accelerationStructureBytes);
    profileReport_.rayTracingGeometry.opaquePrimitiveCount = rtStats.geometry.opaquePrimitiveCount;
    profileReport_.rayTracingGeometry.alphaTestedPrimitiveCount = rtStats.geometry.alphaTestedPrimitiveCount;
    profileReport_.rayTracingGeometry.blendedPrimitiveCount = rtStats.geometry.blendedPrimitiveCount;
    profileReport_.rayTracingGeometry.opaqueTriangleCount = rtStats.geometry.opaqueTriangleCount;
    profileReport_.rayTracingGeometry.alphaTestedTriangleCount = rtStats.geometry.alphaTestedTriangleCount;
    profileReport_.rayTracingGeometry.blendedTriangleCount = rtStats.geometry.blendedTriangleCount;
    profileReport_.rayTracingGeometry.meshCountWithOnlyOpaqueGeometry = rtStats.geometry.meshCountWithOnlyOpaqueGeometry;
    profileReport_.rayTracingGeometry.meshCountWithAlphaTestedGeometry = rtStats.geometry.meshCountWithAlphaTestedGeometry;
    profileReport_.rayTracingGeometry.meshCountWithBlendedGeometry = rtStats.geometry.meshCountWithBlendedGeometry;
    profileReport_.rayTracingGeometry.blasGeometryCount = rtStats.blasGeometry.geometryCount;
    profileReport_.rayTracingGeometry.blasOpaqueGeometryCount = rtStats.blasGeometry.opaqueGeometryCount;
    profileReport_.rayTracingGeometry.blasAlphaTestedGeometryCount = rtStats.blasGeometry.alphaTestedGeometryCount;
    profileReport_.rayTracingGeometry.blasBlendedGeometryCount = rtStats.blasGeometry.blendedGeometryCount;
    profileReport_.rayTracingGeometry.blasOpacityMicromapGeometryCount = rtStats.blasGeometry.opacityMicromapGeometryCount;
    profileReport_.rayTracingGeometry.blasBuildBatchCount = rtStats.blasGeometry.buildBatchCount;

    const AnimatedGeometryStats& animatedGeometry = app.latestAnimatedGeometryStats();
    profileReport_.animatedGeometry.meshInstanceCount = animatedGeometry.meshInstanceCount;
    profileReport_.animatedGeometry.staticMeshInstanceCount = animatedGeometry.staticMeshInstanceCount;
    profileReport_.animatedGeometry.transformOnlyCandidateInstanceCount = animatedGeometry.transformOnlyCandidateInstanceCount;
    profileReport_.animatedGeometry.deformingInstanceCount = animatedGeometry.deformingInstanceCount;
    profileReport_.animatedGeometry.morphDeformingInstanceCount = animatedGeometry.morphDeformingInstanceCount;
    profileReport_.animatedGeometry.skinnedDeformingInstanceCount = animatedGeometry.skinnedDeformingInstanceCount;
    profileReport_.animatedGeometry.combinedMorphSkinnedInstanceCount = animatedGeometry.combinedMorphSkinnedInstanceCount;
    profileReport_.animatedGeometry.runtimeMeshInstanceCount = animatedGeometry.runtimeMeshInstanceCount;
    profileReport_.animatedGeometry.materialOverrideRuntimeMeshInstanceCount = animatedGeometry.materialOverrideRuntimeMeshInstanceCount;
    profileReport_.animatedGeometry.missingMeshInstanceCount = animatedGeometry.missingMeshInstanceCount;
    profileReport_.animatedGeometry.totalPrimitiveCount = animatedGeometry.totalPrimitiveCount;
    profileReport_.animatedGeometry.totalTriangleCount = animatedGeometry.totalTriangleCount;
    profileReport_.animatedGeometry.gpuSkinningCandidateInstanceCount = animatedGeometry.gpuSkinningCandidateInstanceCount;
    profileReport_.animatedGeometry.gpuSkinningMorphPreSkinInstanceCount = animatedGeometry.gpuSkinningMorphPreSkinInstanceCount;
    profileReport_.animatedGeometry.gpuSkinningJointMatrixCount = std::max(
        animatedGeometry.gpuSkinningJointMatrixCount,
        app.latestGpuSkinningJointMatrices().size() > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(app.latestGpuSkinningJointMatrices().size()));
    profileReport_.animatedGeometry.gpuSkinningJointUploadBytes = std::max(
        animatedGeometry.gpuSkinningJointUploadBytes,
        static_cast<uint64_t>(app.latestGpuSkinningJointMatrices().size()) * static_cast<uint64_t>(sizeof(glm::mat4)));
    profileReport_.animatedGeometry.gpuSkinningPreviousJointUploadBytes = std::max(
        animatedGeometry.gpuSkinningPreviousJointUploadBytes,
        static_cast<uint64_t>(app.latestGpuSkinningPreviousJointMatrices().size()) * static_cast<uint64_t>(sizeof(glm::mat4)));
    profileReport_.animatedGeometry.gpuSkinningSourceVertexUploadBytes = std::max(
        animatedGeometry.gpuSkinningSourceVertexUploadBytes,
        static_cast<uint64_t>(app.latestGpuSkinningSourceVertices().size()) * static_cast<uint64_t>(sizeof(PathTracerRenderer::GpuSkinningSourceVertex)));
    profileReport_.animatedGeometry.gpuSkinningMorphDeltaUploadBytes = std::max(
        animatedGeometry.gpuSkinningMorphDeltaUploadBytes,
        static_cast<uint64_t>(app.latestGpuSkinningMorphDeltas().size()) * static_cast<uint64_t>(sizeof(GpuLocalVertex)));
    profileReport_.animatedGeometry.gpuSkinningCurrentVertexCount = animatedGeometry.gpuSkinningCurrentVertexCount;
    profileReport_.animatedGeometry.gpuSkinningPreviousVertexCount = animatedGeometry.gpuSkinningPreviousVertexCount;
    profileReport_.animatedGeometry.gpuSkinningCurrentVertexBufferBytes = animatedGeometry.gpuSkinningCurrentVertexBufferBytes;
    profileReport_.animatedGeometry.gpuSkinningPreviousVertexBufferBytes = animatedGeometry.gpuSkinningPreviousVertexBufferBytes;
    profileReport_.animatedGeometry.gpuSkinningCpuFallbackInstanceCount = animatedGeometry.gpuSkinningCpuFallbackInstanceCount;
    profileReport_.animatedGeometry.gpuSkinningDispatchRecordCount = std::max(
        animatedGeometry.gpuSkinningDispatchRecordCount,
        app.latestGpuSkinningPlan().size() > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(app.latestGpuSkinningPlan().size()));
    renderer->updateGpuSkinningOutputReadbackValidation();
    const PathTracerRenderer::GpuSkinningResourcePlan& rendererSkinningPlan = renderer->gpuSkinningResourcePlan();
    profileReport_.animatedGeometry.gpuSkinningRendererPlanActive = rendererSkinningPlan.candidateInstanceCount > 0 || rendererSkinningPlan.dispatchRecordCount > 0;
    profileReport_.animatedGeometry.gpuSkinningRendererJointUploadBytes = rendererSkinningPlan.jointUploadBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererPreviousJointUploadBytes = rendererSkinningPlan.previousJointUploadBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererSourceVertexUploadBytes = rendererSkinningPlan.sourceVertexUploadBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererMorphDeltaUploadBytes = rendererSkinningPlan.morphDeltaUploadBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererCurrentVertexBufferBytes = rendererSkinningPlan.currentVertexBufferBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererPreviousVertexBufferBytes = rendererSkinningPlan.previousVertexBufferBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererBuffersAllocated = rendererSkinningPlan.rendererBuffersAllocated;
    profileReport_.animatedGeometry.gpuSkinningRendererSourceVertexBufferUploaded = rendererSkinningPlan.sourceVertexBufferUploaded;
    profileReport_.animatedGeometry.gpuSkinningRendererMorphDeltaBufferUploaded = rendererSkinningPlan.morphDeltaBufferUploaded;
    profileReport_.animatedGeometry.gpuSkinningRendererJointBufferUploaded = rendererSkinningPlan.jointBufferUploaded;
    profileReport_.animatedGeometry.gpuSkinningRendererPreviousJointBufferUploaded = rendererSkinningPlan.previousJointBufferUploaded;
    profileReport_.animatedGeometry.gpuSkinningRendererJointPayloadRefreshUploaded = rendererSkinningPlan.jointPayloadRefreshUploaded;
    profileReport_.animatedGeometry.gpuSkinningRendererJointPayloadRefreshStaged = rendererSkinningPlan.jointPayloadRefreshStaged;
    profileReport_.animatedGeometry.gpuSkinningRendererJointPayloadRefreshCopyRecorded = rendererSkinningPlan.jointPayloadRefreshCopyRecorded;
    profileReport_.animatedGeometry.gpuSkinningRendererJointPayloadRefreshCount = rendererSkinningPlan.jointPayloadRefreshCount;
    profileReport_.animatedGeometry.gpuSkinningRendererJointPayloadRefreshCopyRecordCount = rendererSkinningPlan.jointPayloadRefreshCopyRecordCount;
    profileReport_.animatedGeometry.gpuSkinningRendererJointPayloadRefreshBytes = rendererSkinningPlan.jointPayloadRefreshBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererJointPayloadRefreshStagedBytes = rendererSkinningPlan.jointPayloadRefreshStagedBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererJointPayloadRefreshCopyBytes = rendererSkinningPlan.jointPayloadRefreshCopyBytes;
    profileReport_.animatedGeometry.gpuSkinningRendererComputePipelineCreated = rendererSkinningPlan.computePipelineCreated;
    profileReport_.animatedGeometry.gpuSkinningRendererDescriptorsBound = rendererSkinningPlan.descriptorsBound;
    profileReport_.animatedGeometry.gpuSkinningRendererComputeDispatchEnabled = rendererSkinningPlan.computeDispatchEnabled;
    profileReport_.animatedGeometry.gpuSkinningRendererComputeDispatchRecordCount = rendererSkinningPlan.computeDispatchRecordCount;
    profileReport_.animatedGeometry.gpuSkinningRendererComputeMorphDispatchRecordCount = rendererSkinningPlan.computeMorphDispatchRecordCount;
    profileReport_.animatedGeometry.gpuSkinningRendererOutputReadbackBufferAllocated = rendererSkinningPlan.outputReadbackBufferAllocated;
    profileReport_.animatedGeometry.gpuSkinningRendererOutputReadbackCopyRecorded = rendererSkinningPlan.outputReadbackCopyRecorded;
    profileReport_.animatedGeometry.gpuSkinningRendererOutputReadbackValidationPassed = rendererSkinningPlan.outputReadbackValidationPassed;
    profileReport_.animatedGeometry.gpuSkinningRendererOutputReadbackVertexCount = rendererSkinningPlan.outputReadbackVertexCount;
    profileReport_.animatedGeometry.gpuSkinningRendererOutputReadbackMaxPositionError = rendererSkinningPlan.outputReadbackMaxPositionError;
    profileReport_.animatedGeometry.gpuSkinningRendererInitialComputeDispatchSubmitted = rendererSkinningPlan.initialComputeDispatchSubmitted;
    profileReport_.animatedGeometry.gpuSkinningRayTracingGeometryInputEnabled = rendererSkinningPlan.rayTracingGeometryInputEnabled;
    profileReport_.animatedGeometry.gpuSkinningRayTracingGeometryInputMeshCount = rendererSkinningPlan.rayTracingGeometryInputMeshCount;
    profileReport_.animatedGeometry.gpuSkinningRayTracingGeometryInputGeometryCount = rendererSkinningPlan.rayTracingGeometryInputGeometryCount;
    profileReport_.animatedGeometry.gpuSkinningRayTracingDescriptorInputEnabled = rendererSkinningPlan.rayTracingDescriptorInputEnabled;
    profileReport_.animatedGeometry.gpuSkinningRayTracingDescriptorInputMixedSceneEnabled = rendererSkinningPlan.rayTracingDescriptorInputMixedSceneEnabled;
    profileReport_.animatedGeometry.gpuSkinningRayTracingDescriptorInputMeshCount = rendererSkinningPlan.rayTracingDescriptorInputMeshCount;
    profileReport_.animatedGeometry.gpuSkinningRayTracingDescriptorInputBindingCount = rendererSkinningPlan.rayTracingDescriptorInputBindingCount;
    profileReport_.animatedGeometry.gpuSkinningSkinnedMotionVectorsEnabled = rendererSkinningPlan.skinnedMotionVectorsEnabled;
    profileReport_.animatedGeometry.gpuSkinningSkinnedMotionVectorsPreviousVertexInputEnabled = rendererSkinningPlan.skinnedMotionVectorsPreviousVertexInputEnabled;
    profileReport_.animatedGeometry.gpuSkinningSkinnedMotionVectorsDescriptorBound = rendererSkinningPlan.skinnedMotionVectorsDescriptorBound;
    profileReport_.animatedGeometry.transformOnlyPolicy = animatedGeometry.transformOnlyPolicy;
    profileReport_.animatedGeometry.deformingPolicy = animatedGeometry.deformingPolicy;
    profileReport_.animatedGeometry.morphPolicy = animatedGeometry.morphPolicy;
    profileReport_.animatedGeometry.skinningPolicy = animatedGeometry.skinningPolicy;
    profileReport_.animatedGeometry.gpuSkinningDataPolicy = animatedGeometry.gpuSkinningDataPolicy;
    profileReport_.animatedGeometry.gpuSkinningJointUploadPolicy = animatedGeometry.gpuSkinningJointUploadPolicy;
    profileReport_.animatedGeometry.gpuSkinningBufferPolicy = animatedGeometry.gpuSkinningBufferPolicy;
    if (rendererSkinningPlan.sourceVertexBufferUploaded && rendererSkinningPlan.jointBufferUploaded) {
        profileReport_.animatedGeometry.gpuSkinningDataPolicy = "renderer_source_joint_payload_ready_cpu_runtime_mesh_fallback";
        profileReport_.animatedGeometry.gpuSkinningJointUploadPolicy = "initial_renderer_joint_upload_done_budgeted_per_frame_required";
    }
    if (rendererSkinningPlan.jointPayloadRefreshCopyRecordCount > 0) {
        profileReport_.animatedGeometry.gpuSkinningJointUploadPolicy = "renderer_joint_payload_refresh_copy_recorded_budgeted_path_required";
    } else if (rendererSkinningPlan.jointPayloadRefreshCount > 0) {
        profileReport_.animatedGeometry.gpuSkinningJointUploadPolicy = "renderer_joint_payload_refresh_staged_copy_pending";
    }
    if (rendererSkinningPlan.rendererBuffersAllocated) {
        profileReport_.animatedGeometry.gpuSkinningBufferPolicy = "renderer_source_joint_current_previous_buffers_allocated";
    }
    profileReport_.animatedGeometry.gpuSkinningComputeShader = animatedGeometry.gpuSkinningComputeShader;
    profileReport_.animatedGeometry.rayTracingBlasCount = rtStats.blasCount;
    profileReport_.animatedGeometry.rayTracingInstanceCount = rtStats.instanceCount;
    profileReport_.animatedGeometry.tlasRefitCount = rtStats.motionInstances.tlasRefitCount;
    profileReport_.animatedGeometry.lastTlasRefitMs = rtStats.lastTlasRefitMs;
    profileReport_.animatedGeometry.dynamicBlasUpdateCount = rtStats.dynamicBlasUpdateCount;
    profileReport_.animatedGeometry.dynamicBlasUpdateRecordMs = rtStats.lastDynamicBlasUpdateRecordMs;
    profileReport_.animatedGeometry.dynamicBlasUpdateMs = timings.dynamicBlasUpdateMs;
    const uint32_t dynamicBlasCandidateCount = profileReport_.animatedGeometry.gpuSkinningCandidateInstanceCount;
    const uint32_t dynamicBlasCompatibleCount = profileReport_.animatedGeometry.gpuSkinningRayTracingGeometryInputMeshCount;
    profileReport_.animatedGeometry.dynamicBlasUpdateSupported =
        rtStats.active && dynamicBlasCompatibleCount > 0u && rtStats.dynamicBlasUpdateCount > 0u;
    profileReport_.animatedGeometry.dynamicBlasUnsupportedInstanceCount =
        dynamicBlasCandidateCount > dynamicBlasCompatibleCount
            ? dynamicBlasCandidateCount - dynamicBlasCompatibleCount
            : 0u;
    profileReport_.animatedGeometry.dynamicBlasCpuFallbackInstanceCount =
        profileReport_.animatedGeometry.gpuSkinningCpuFallbackInstanceCount;
    profileReport_.animatedGeometry.dynamicBlasFallbackWarnings.clear();
    if (dynamicBlasCandidateCount > 0u && !rtStats.active) {
        profileReport_.animatedGeometry.dynamicBlasFallbackWarnings.push_back(
            "Hardware ray tracing is unavailable, so GPU-skinned dynamic BLAS updates cannot run; CPU/runtime mesh fallback is required.");
    }
    if (profileReport_.animatedGeometry.dynamicBlasUnsupportedInstanceCount > 0u) {
        profileReport_.animatedGeometry.dynamicBlasFallbackWarnings.push_back(
            std::to_string(profileReport_.animatedGeometry.dynamicBlasUnsupportedInstanceCount) +
            " GPU-skinned candidate instance(s) lack compatible ray-tracing GPU-skinned vertex input; CPU/runtime mesh fallback remains required.");
    }
    if (dynamicBlasCompatibleCount > 0u && rtStats.dynamicBlasUpdateCount == 0u) {
        profileReport_.animatedGeometry.dynamicBlasFallbackWarnings.push_back(
            "Compatible GPU-skinned ray-tracing input exists, but no dynamic BLAS update was recorded in this profile window.");
    }
    if (profileReport_.animatedGeometry.dynamicBlasCpuFallbackInstanceCount > 0u) {
        profileReport_.animatedGeometry.dynamicBlasFallbackWarnings.push_back(
            std::to_string(profileReport_.animatedGeometry.dynamicBlasCpuFallbackInstanceCount) +
            " GPU-skinned instance(s) still retain CPU runtime mesh fallback for preview or compatibility.");
    }
    if (dynamicBlasCandidateCount == 0u) {
        profileReport_.animatedGeometry.dynamicBlasFallbackPolicy = "no_gpu_skinned_dynamic_blas_candidates";
    } else if (!rtStats.active) {
        profileReport_.animatedGeometry.dynamicBlasFallbackPolicy = "hardware_ray_tracing_unavailable_cpu_runtime_mesh_fallback";
    } else if (profileReport_.animatedGeometry.dynamicBlasUpdateSupported &&
        profileReport_.animatedGeometry.dynamicBlasUnsupportedInstanceCount == 0u &&
        profileReport_.animatedGeometry.dynamicBlasCpuFallbackInstanceCount == 0u) {
        profileReport_.animatedGeometry.dynamicBlasFallbackPolicy = "all_gpu_skinned_candidates_dynamic_blas_updated";
    } else if (profileReport_.animatedGeometry.dynamicBlasUpdateSupported &&
        profileReport_.animatedGeometry.dynamicBlasUnsupportedInstanceCount == 0u) {
        profileReport_.animatedGeometry.dynamicBlasFallbackPolicy = "compatible_dynamic_blas_updates_recorded_cpu_runtime_mesh_fallback_retained";
    } else if (profileReport_.animatedGeometry.dynamicBlasUpdateSupported) {
        profileReport_.animatedGeometry.dynamicBlasFallbackPolicy = "partial_dynamic_blas_updates_recorded_unsupported_candidates_fallback";
    } else if (dynamicBlasCompatibleCount > 0u) {
        profileReport_.animatedGeometry.dynamicBlasFallbackPolicy = "dynamic_blas_update_pending_or_not_recorded_cpu_runtime_mesh_fallback";
    } else {
        profileReport_.animatedGeometry.dynamicBlasFallbackPolicy = "no_dynamic_blas_compatible_rt_input_cpu_runtime_mesh_fallback";
    }
    profileReport_.animatedGeometry.dynamicBlasBudgetMs = static_cast<float>(frameWorkScheduler.budgets.accelerationStructureBuildMs);
    profileReport_.animatedGeometry.dynamicBlasOverBudgetFrameCount = countDynamicBlasOverBudgetFrames(
        app.perFrameGpuTimings(),
        warmup,
        profileReport_.animatedGeometry.dynamicBlasBudgetMs);
    profileReport_.animatedGeometry.dynamicBlasBudgetExceeded =
        profileReport_.animatedGeometry.dynamicBlasBudgetMs > 0.0f &&
        profileReport_.animatedGeometry.dynamicBlasUpdateMs > profileReport_.animatedGeometry.dynamicBlasBudgetMs;
    if (rtStats.dynamicBlasUpdateCount > 0u) {
        profileReport_.animatedGeometry.dynamicBlasBudgetPolicy =
            profileReport_.animatedGeometry.dynamicBlasBudgetExceeded || profileReport_.animatedGeometry.dynamicBlasOverBudgetFrameCount > 0u
                ? "gpu_skinned_dynamic_blas_required_updates_recorded_over_budget_reported"
                : "gpu_skinned_dynamic_blas_required_updates_recorded_within_budget";
    } else {
        profileReport_.animatedGeometry.dynamicBlasBudgetPolicy = "no_dynamic_blas_updates_recorded";
    }
    if (rtStats.dynamicBlasUpdateCount > 0u) {
        profileReport_.animatedGeometry.tlasRefitPolicy = rtStats.motionInstances.tlasRefitCount > 0u
            ? "transform_only_tlas_refit_recorded"
            : "no_transform_only_tlas_refit_recorded";
        profileReport_.animatedGeometry.dynamicBlasTimingPolicy = timings.dynamicBlasUpdateMs > 0.0f
            ? "gpu_skinned_dynamic_blas_update_gpu_timing_recorded"
            : "gpu_skinned_dynamic_blas_update_recorded_gpu_timing_pending";
    } else if (rtStats.motionInstances.tlasRefitCount > 0u) {
        profileReport_.animatedGeometry.tlasRefitPolicy = "transform_only_tlas_refit_recorded";
        profileReport_.animatedGeometry.dynamicBlasTimingPolicy = "tlas_refit_timing_recorded_dynamic_blas_update_not_implemented";
    } else if (animatedGeometry.transformOnlyCandidateInstanceCount > 0u) {
        profileReport_.animatedGeometry.tlasRefitPolicy = "transform_only_tlas_refit_available_not_recorded";
        profileReport_.animatedGeometry.dynamicBlasTimingPolicy = "dynamic_blas_update_not_implemented";
    } else {
        profileReport_.animatedGeometry.tlasRefitPolicy = "no_transform_only_tlas_refit_recorded";
        profileReport_.animatedGeometry.dynamicBlasTimingPolicy = "dynamic_blas_update_not_implemented";
    }
    if (profileReport_.animatedGeometry.meshInstanceCount == 0 && rtStats.instanceCount > 0) {
        profileReport_.animatedGeometry.meshInstanceCount = rtStats.instanceCount;
        profileReport_.animatedGeometry.staticMeshInstanceCount = rtStats.instanceCount;
        profileReport_.animatedGeometry.totalPrimitiveCount =
            rtStats.geometry.opaquePrimitiveCount +
            rtStats.geometry.alphaTestedPrimitiveCount +
            rtStats.geometry.blendedPrimitiveCount;
        profileReport_.animatedGeometry.totalTriangleCount =
            rtStats.geometry.opaqueTriangleCount +
            rtStats.geometry.alphaTestedTriangleCount +
            rtStats.geometry.blendedTriangleCount;
    }

    const auto& lightRecords = renderer->scene().lightRecordsCpu();
    profileReport_.sceneLights.recordCount = static_cast<uint32_t>(lightRecords.size());
    std::vector<std::array<float, 3>> distinctAuthoredPositions;
    constexpr float positionEpsilon = 0.001f;
    constexpr size_t maxAuthoredLightSamples = 32;
    constexpr size_t maxEmissiveLightSamples = 32;
    auto samePosition = [](const std::array<float, 3>& a, const std::array<float, 3>& b, float epsilon) {
        return std::abs(a[0] - b[0]) <= epsilon &&
            std::abs(a[1] - b[1]) <= epsilon &&
            std::abs(a[2] - b[2]) <= epsilon;
    };
    for (uint32_t i = 0; i < static_cast<uint32_t>(lightRecords.size()); ++i) {
        const GpuLightRecord& light = lightRecords[i];
        const uint32_t type = light.metadata.x;
        const bool active = light.data0.x > 0.0f;
        const bool authored = type >= 2u && type <= 5u;
        if (!active && !authored) {
            continue;
        }

        switch (type) {
        case 0u:
        case 1u:
            ++profileReport_.sceneLights.emissiveCount;
            if (profileReport_.sceneLights.emissiveSamples.size() < maxEmissiveLightSamples) {
                profileReport_.sceneLights.emissiveSamples.push_back(ProfileReport::SceneLightReport::Sample{
                    .recordIndex = i,
                    .type = type,
                    .sourceIndex = light.metadata.y,
                    .positionOrDirection = {light.data1.x, light.data1.y, light.data1.z},
                    .radiusOrSize = light.data0.z,
                    .weight = light.data0.x,
                });
            }
            break;
        case 2u:
            ++profileReport_.sceneLights.authoredCount;
            ++profileReport_.sceneLights.directionalCount;
            break;
        case 3u:
            ++profileReport_.sceneLights.authoredCount;
            ++profileReport_.sceneLights.pointCount;
            break;
        case 4u:
            ++profileReport_.sceneLights.authoredCount;
            ++profileReport_.sceneLights.areaCount;
            break;
        case 5u:
            ++profileReport_.sceneLights.authoredCount;
            ++profileReport_.sceneLights.spotCount;
            break;
        default:
            break;
        }

        if (type == 3u || type == 4u || type == 5u) {
            const std::array<float, 3> position{light.data1.x, light.data1.y, light.data1.z};
            ++profileReport_.sceneLights.authoredPositionalCount;
            const bool alreadySeen = std::any_of(
                distinctAuthoredPositions.begin(),
                distinctAuthoredPositions.end(),
                [&](const std::array<float, 3>& existing) { return samePosition(existing, position, positionEpsilon); });
            if (!alreadySeen) {
                distinctAuthoredPositions.push_back(position);
            }
        }

        if (authored && profileReport_.sceneLights.authoredSamples.size() < maxAuthoredLightSamples) {
            profileReport_.sceneLights.authoredSamples.push_back(ProfileReport::SceneLightReport::Sample{
                .recordIndex = i,
                .type = type,
                .sourceIndex = light.metadata.y,
                .positionOrDirection = {light.data1.x, light.data1.y, light.data1.z},
                .radiusOrSize = light.data0.z,
                .weight = light.data0.x,
            });
        }
    }
    profileReport_.sceneLights.authoredDistinctPositionCount = static_cast<uint32_t>(distinctAuthoredPositions.size());
    profileReport_.sceneLights.authoredPositionsCollapsed =
        profileReport_.sceneLights.authoredPositionalCount > 1u &&
        profileReport_.sceneLights.authoredDistinctPositionCount <= 1u;
    if (profileReport_.sceneLights.authoredPositionsCollapsed) {
        profileReport_.warnings.push_back("multiple authored positional lights collapsed to one uploaded GPU position");
    }

    profileReport_.rayTracingMotionBlur.motionInstancesActive = rtStats.motionInstances.active;
    profileReport_.rayTracingMotionBlur.motionInstanceCount = rtStats.motionInstances.instanceCount;
    profileReport_.rayTracingMotionBlur.movingInstanceCount = rtStats.motionInstances.movingInstanceCount;
    profileReport_.rayTracingMotionBlur.staticInstanceCount = rtStats.motionInstances.staticInstanceCount;
    profileReport_.rayTracingMotionBlur.tlasRefitCount = rtStats.motionInstances.tlasRefitCount;
    profileReport_.rayTracingMotionBlur.maxTransformDelta = rtStats.motionInstances.maxTransformDelta;
    profileReport_.rayTracingMotionBlur.hasMovingAndStaticInstances =
        rtStats.motionInstances.movingInstanceCount > 0 && rtStats.motionInstances.staticInstanceCount > 0;
    const auto wavefrontStats = renderer->wavefrontQueueStats();
    profileReport_.wavefrontQueues.buffersAllocated = wavefrontStats.buffersAllocated;
    profileReport_.wavefrontQueues.clearValidationPassed = wavefrontStats.clearValidationPassed;
    profileReport_.wavefrontQueues.maxPathDepth = wavefrontStats.maxPathDepth;
    profileReport_.wavefrontQueues.rayQueueCapacity = wavefrontStats.rayQueueCapacity;
    profileReport_.wavefrontQueues.compactedRayQueueCapacity = wavefrontStats.compactedRayQueueCapacity;
    profileReport_.wavefrontQueues.sortedRayQueueCapacity = wavefrontStats.sortedRayQueueCapacity;
    profileReport_.wavefrontQueues.hitQueueCapacity = wavefrontStats.hitQueueCapacity;
    profileReport_.wavefrontQueues.shadowQueueCapacity = wavefrontStats.shadowQueueCapacity;
    profileReport_.wavefrontQueues.pixelStateCapacity = wavefrontStats.pixelStateCapacity;
    profileReport_.wavefrontQueues.rayQueueCount = wavefrontStats.rayQueueCount;
    profileReport_.wavefrontQueues.hitQueueCount = wavefrontStats.hitQueueCount;
    profileReport_.wavefrontQueues.shadowQueueCount = wavefrontStats.shadowQueueCount;
    profileReport_.wavefrontQueues.pixelStateCount = wavefrontStats.pixelStateCount;
    profileReport_.wavefrontQueues.clearValidationCounter = wavefrontStats.clearValidationCounter;
    profileReport_.wavefrontQueues.primaryGenerationEnabled = wavefrontStats.primaryGenerationEnabled;
    profileReport_.wavefrontQueues.primaryGenerationValidationPassed = wavefrontStats.primaryGenerationValidationPassed;
    profileReport_.wavefrontQueues.expectedPrimaryRayCount = wavefrontStats.expectedPrimaryRayCount;
    profileReport_.wavefrontQueues.sampledPrimaryRayCount = wavefrontStats.sampledPrimaryRayCount;
    profileReport_.wavefrontQueues.firstRayDirectionError = wavefrontStats.firstRayDirectionError;
    profileReport_.wavefrontQueues.centerRayDirectionError = wavefrontStats.centerRayDirectionError;
    profileReport_.wavefrontQueues.cornerRayDirectionError = wavefrontStats.cornerRayDirectionError;
    profileReport_.wavefrontQueues.maxRayDirectionError = wavefrontStats.maxRayDirectionError;
    profileReport_.wavefrontQueues.traceEnabled = wavefrontStats.traceEnabled;
    profileReport_.wavefrontQueues.traceValidationPassed = wavefrontStats.traceValidationPassed;
    profileReport_.wavefrontQueues.traceRaysIndirectSupported = wavefrontStats.traceRaysIndirectSupported;
    profileReport_.wavefrontQueues.secondaryTraceIndirectEnabled = wavefrontStats.secondaryTraceIndirectEnabled;
    profileReport_.wavefrontQueues.traceCheckedPixels = wavefrontStats.traceCheckedPixels;
    profileReport_.wavefrontQueues.traceHitMismatchCount = wavefrontStats.traceHitMismatchCount;
    profileReport_.wavefrontQueues.traceInstanceMismatchCount = wavefrontStats.traceInstanceMismatchCount;
    profileReport_.wavefrontQueues.traceDepthMismatchCount = wavefrontStats.traceDepthMismatchCount;
    profileReport_.wavefrontQueues.traceNormalMismatchCount = wavefrontStats.traceNormalMismatchCount;
    profileReport_.wavefrontQueues.shadeEnabled = wavefrontStats.shadeEnabled;
    profileReport_.wavefrontQueues.shadeValidationPassed = wavefrontStats.shadeValidationPassed;
    profileReport_.wavefrontQueues.shadeCheckedPixels = wavefrontStats.shadeCheckedPixels;
    profileReport_.wavefrontQueues.shadeHitCount = wavefrontStats.shadeHitCount;
    profileReport_.wavefrontQueues.shadeMissCount = wavefrontStats.shadeMissCount;
    profileReport_.wavefrontQueues.shadeTerminatedCount = wavefrontStats.shadeTerminatedCount;
    profileReport_.wavefrontQueues.shadeShadowRayCount = wavefrontStats.shadeShadowRayCount;
    profileReport_.wavefrontQueues.shadeSecondaryRayCount = wavefrontStats.shadeSecondaryRayCount;
    profileReport_.wavefrontQueues.shadeMaterialCount = wavefrontStats.shadeMaterialCount;
    profileReport_.wavefrontQueues.shadeRestirReservoirWriteCount = wavefrontStats.shadeRestirReservoirWriteCount;
    profileReport_.wavefrontQueues.shadeRestirValidCandidateCount = wavefrontStats.shadeRestirValidCandidateCount;
    profileReport_.wavefrontQueues.shadeRestirTemporalMergeCount = wavefrontStats.shadeRestirTemporalMergeCount;
    profileReport_.wavefrontQueues.shadeRestirInvalidCandidateCount = wavefrontStats.shadeRestirInvalidCandidateCount;
    profileReport_.wavefrontQueues.shadeRestirGiReservoirWriteCount = wavefrontStats.shadeRestirGiReservoirWriteCount;
    profileReport_.wavefrontQueues.shadeRestirGiValidCandidateCount = wavefrontStats.shadeRestirGiValidCandidateCount;
    profileReport_.wavefrontQueues.shadeRestirGiTemporalMergeCount = wavefrontStats.shadeRestirGiTemporalMergeCount;
    profileReport_.wavefrontQueues.shadeRestirGiInvalidCandidateCount = wavefrontStats.shadeRestirGiInvalidCandidateCount;
    profileReport_.wavefrontQueues.secondaryShadeEnabled = wavefrontStats.secondaryShadeEnabled;
    profileReport_.wavefrontQueues.secondaryShadeValidationPassed = wavefrontStats.secondaryShadeValidationPassed;
    profileReport_.wavefrontQueues.secondaryShadeCheckedRays = wavefrontStats.secondaryShadeCheckedRays;
    profileReport_.wavefrontQueues.secondaryShadeHitCount = wavefrontStats.secondaryShadeHitCount;
    profileReport_.wavefrontQueues.secondaryShadeMissCount = wavefrontStats.secondaryShadeMissCount;
    profileReport_.wavefrontQueues.secondaryShadeTerminatedCount = wavefrontStats.secondaryShadeTerminatedCount;
    profileReport_.wavefrontQueues.secondaryShadeShadowRayCount = wavefrontStats.secondaryShadeShadowRayCount;
    profileReport_.wavefrontQueues.secondaryShadeSecondaryRayCount = wavefrontStats.secondaryShadeSecondaryRayCount;
    profileReport_.wavefrontQueues.secondaryShadeMaterialCount = wavefrontStats.secondaryShadeMaterialCount;
    profileReport_.wavefrontQueues.sortedShadeEnabled = wavefrontStats.sortedShadeEnabled;
    profileReport_.wavefrontQueues.sortedShadeValidationPassed = wavefrontStats.sortedShadeValidationPassed;
    profileReport_.wavefrontQueues.sortedShadeCheckedRays = wavefrontStats.sortedShadeCheckedRays;
    profileReport_.wavefrontQueues.sortedShadeHitCount = wavefrontStats.sortedShadeHitCount;
    profileReport_.wavefrontQueues.sortedShadeMissCount = wavefrontStats.sortedShadeMissCount;
    profileReport_.wavefrontQueues.sortedShadeTerminatedCount = wavefrontStats.sortedShadeTerminatedCount;
    profileReport_.wavefrontQueues.sortedShadeShadowRayCount = wavefrontStats.sortedShadeShadowRayCount;
    profileReport_.wavefrontQueues.sortedShadeSecondaryRayCount = wavefrontStats.sortedShadeSecondaryRayCount;
    profileReport_.wavefrontQueues.sortedShadeMaterialCount = wavefrontStats.sortedShadeMaterialCount;
    profileReport_.wavefrontQueues.compactEnabled = wavefrontStats.compactEnabled;
    profileReport_.wavefrontQueues.compactValidationPassed = wavefrontStats.compactValidationPassed;
    profileReport_.wavefrontQueues.compactInputRayCount = wavefrontStats.compactInputRayCount;
    profileReport_.wavefrontQueues.compactScannedRayCount = wavefrontStats.compactScannedRayCount;
    profileReport_.wavefrontQueues.compactLiveRayCount = wavefrontStats.compactLiveRayCount;
    profileReport_.wavefrontQueues.compactOutputRayCount = wavefrontStats.compactOutputRayCount;
    profileReport_.wavefrontQueues.compactDroppedInvalidCount = wavefrontStats.compactDroppedInvalidCount;
    profileReport_.wavefrontQueues.compactOverflowCount = wavefrontStats.compactOverflowCount;
    profileReport_.wavefrontQueues.compactInvalidPixelCount = wavefrontStats.compactInvalidPixelCount;
    profileReport_.wavefrontQueues.compactMappingMismatchCount = wavefrontStats.compactMappingMismatchCount;
    profileReport_.wavefrontQueues.sortEnabled = wavefrontStats.sortEnabled;
    profileReport_.wavefrontQueues.finalOutputEnabled = wavefrontStats.finalOutputEnabled;
    profileReport_.wavefrontQueues.sortValidationPassed = wavefrontStats.sortValidationPassed;
    profileReport_.wavefrontQueues.sortInputRayCount = wavefrontStats.sortInputRayCount;
    profileReport_.wavefrontQueues.sortOutputRayCount = wavefrontStats.sortOutputRayCount;
    profileReport_.wavefrontQueues.sortActiveBucketCount = wavefrontStats.sortActiveBucketCount;
    profileReport_.wavefrontQueues.sortVerifiedRayCount = wavefrontStats.sortVerifiedRayCount;
    profileReport_.wavefrontQueues.sortBucketCount = wavefrontStats.sortBucketCount;
    profileReport_.wavefrontQueues.sortOverflowCount = wavefrontStats.sortOverflowCount;
    profileReport_.wavefrontQueues.sortInvalidPixelCount = wavefrontStats.sortInvalidPixelCount;
    profileReport_.wavefrontQueues.sortOrderViolationCount = wavefrontStats.sortOrderViolationCount;
    profileReport_.wavefrontQueues.shadowTraceEnabled = wavefrontStats.shadowTraceEnabled;
    profileReport_.wavefrontQueues.shadowTraceValidationPassed = wavefrontStats.shadowTraceValidationPassed;
    profileReport_.wavefrontQueues.shadowTraceCheckedRays = wavefrontStats.shadowTraceCheckedRays;
    profileReport_.wavefrontQueues.shadowTraceVisibleCount = wavefrontStats.shadowTraceVisibleCount;
    profileReport_.wavefrontQueues.shadowTraceOccludedCount = wavefrontStats.shadowTraceOccludedCount;
    profileReport_.wavefrontQueues.shadowTraceAppliedCount = wavefrontStats.shadowTraceAppliedCount;
    profileReport_.wavefrontQueues.directLightingParityPassed = wavefrontStats.directLightingParityPassed;
    profileReport_.wavefrontQueues.directLightingCheckedPixels = wavefrontStats.directLightingCheckedPixels;
    profileReport_.wavefrontQueues.directLightingMismatchCount = wavefrontStats.directLightingMismatchCount;
    profileReport_.wavefrontQueues.directLightingMaxAbsError = wavefrontStats.directLightingMaxAbsError;
    profileReport_.wavefrontQueues.directLightingMaxRelativeError = wavefrontStats.directLightingMaxRelativeError;
    const auto safeRatio = [](uint64_t numerator, uint64_t denominator) {
        return denominator > 0u
            ? static_cast<float>(static_cast<double>(numerator) / static_cast<double>(denominator))
            : 0.0f;
    };
    profileReport_.wavefrontQueues.shadowQueueOccupancy = safeRatio(
        wavefrontStats.shadeShadowRayCount,
        wavefrontStats.shadowQueueCapacity);
    profileReport_.wavefrontQueues.shadowTraceRaysPerPixel = safeRatio(
        wavefrontStats.shadowTraceCheckedRays,
        wavefrontStats.expectedPrimaryRayCount);
    profileReport_.wavefrontQueues.shadowTraceVisibleRatio = safeRatio(
        wavefrontStats.shadowTraceVisibleCount,
        wavefrontStats.shadowTraceCheckedRays);
    profileReport_.wavefrontQueues.shadowTraceOccludedRatio = safeRatio(
        wavefrontStats.shadowTraceOccludedCount,
        wavefrontStats.shadowTraceCheckedRays);
    profileReport_.wavefrontQueues.compactQueueOccupancy = safeRatio(
        wavefrontStats.compactOutputRayCount,
        wavefrontStats.compactedRayQueueCapacity);
    profileReport_.wavefrontQueues.compactSurvivalRatio = safeRatio(
        wavefrontStats.compactOutputRayCount,
        wavefrontStats.expectedPrimaryRayCount);
    profileReport_.wavefrontQueues.primaryQueueOccupancy = safeRatio(
        wavefrontStats.rayQueueCount,
        wavefrontStats.rayQueueCapacity);
    profileReport_.wavefrontQueues.traceHitQueueOccupancy = safeRatio(
        wavefrontStats.hitQueueCount,
        wavefrontStats.hitQueueCapacity);
    profileReport_.wavefrontQueues.shadeSecondaryQueueOccupancy = safeRatio(
        wavefrontStats.shadeSecondaryRayCount,
        wavefrontStats.rayQueueCapacity);
    profileReport_.wavefrontQueues.sortOutputQueueOccupancy = safeRatio(
        wavefrontStats.sortOutputRayCount,
        wavefrontStats.sortedRayQueueCapacity);
    profileReport_.wavefrontQueues.secondaryShadeShadowQueueOccupancy = safeRatio(
        wavefrontStats.secondaryShadeShadowRayCount,
        wavefrontStats.shadowQueueCapacity);
    profileReport_.wavefrontQueues.secondaryShadeSecondaryQueueOccupancy = safeRatio(
        wavefrontStats.secondaryShadeSecondaryRayCount,
        wavefrontStats.rayQueueCapacity);
    profileReport_.wavefrontQueues.sortedShadeShadowQueueOccupancy = safeRatio(
        wavefrontStats.sortedShadeShadowRayCount,
        wavefrontStats.shadowQueueCapacity);
    profileReport_.wavefrontQueues.sortedShadeSecondaryQueueOccupancy = safeRatio(
        wavefrontStats.sortedShadeSecondaryRayCount,
        wavefrontStats.rayQueueCapacity);
    profileReport_.wavefrontQueues.queueOverflowCount =
        wavefrontStats.compactOverflowCount +
        wavefrontStats.sortOverflowCount;
    profileReport_.wavefrontQueues.queueStarvationDetected =
        (wavefrontStats.compactEnabled && wavefrontStats.shadeSecondaryRayCount > 0u && wavefrontStats.compactOutputRayCount == 0u) ||
        (wavefrontStats.sortEnabled && wavefrontStats.compactOutputRayCount > 0u && wavefrontStats.sortOutputRayCount == 0u) ||
        (wavefrontStats.secondaryShadeEnabled && wavefrontStats.compactOutputRayCount > 0u && wavefrontStats.secondaryShadeCheckedRays == 0u) ||
        (wavefrontStats.sortedShadeEnabled && wavefrontStats.sortOutputRayCount > 0u && wavefrontStats.sortedShadeCheckedRays == 0u) ||
        (wavefrontStats.shadowTraceEnabled && wavefrontStats.shadeShadowRayCount > 0u && wavefrontStats.shadowTraceCheckedRays == 0u);
    profileReport_.wavefrontQueues.queueBalanceValidationPassed = wavefrontStats.buffersAllocated &&
        wavefrontStats.clearValidationPassed &&
        !profileReport_.wavefrontQueues.queueStarvationDetected &&
        profileReport_.wavefrontQueues.queueOverflowCount == 0u &&
        (!wavefrontStats.compactEnabled || wavefrontStats.compactValidationPassed) &&
        (!wavefrontStats.sortEnabled || wavefrontStats.sortValidationPassed) &&
        (!wavefrontStats.secondaryShadeEnabled || wavefrontStats.secondaryShadeValidationPassed) &&
        (!wavefrontStats.sortedShadeEnabled || wavefrontStats.sortedShadeValidationPassed) &&
        (!wavefrontStats.shadowTraceEnabled || wavefrontStats.shadowTraceValidationPassed);
    profileReport_.wavefrontQueues.compactCostMs = timings.wavefrontCompactMs;
    profileReport_.wavefrontQueues.compactMicrosecondsPerRay = wavefrontStats.compactScannedRayCount > 0u
        ? timings.wavefrontCompactMs * 1000.0f / static_cast<float>(wavefrontStats.compactScannedRayCount)
        : 0.0f;
    profileReport_.wavefrontQueues.sortCostMs = timings.wavefrontSortMs;
    profileReport_.wavefrontQueues.sortMicrosecondsPerRay = wavefrontStats.sortInputRayCount > 0u
        ? timings.wavefrontSortMs * 1000.0f / static_cast<float>(wavefrontStats.sortInputRayCount)
        : 0.0f;
    profileReport_.wavefrontQueues.secondaryShadeCostMs = timings.wavefrontSecondaryShadeMs;
    profileReport_.wavefrontQueues.secondaryShadeMicrosecondsPerRay = wavefrontStats.secondaryShadeCheckedRays > 0u
        ? timings.wavefrontSecondaryShadeMs * 1000.0f / static_cast<float>(wavefrontStats.secondaryShadeCheckedRays)
        : 0.0f;
    profileReport_.wavefrontQueues.secondaryPathCostMs = timings.wavefrontSecondaryTraceMs + timings.wavefrontSecondaryShadeMs;
    profileReport_.wavefrontQueues.secondaryPathMicrosecondsPerRay = wavefrontStats.secondaryShadeCheckedRays > 0u
        ? profileReport_.wavefrontQueues.secondaryPathCostMs * 1000.0f / static_cast<float>(wavefrontStats.secondaryShadeCheckedRays)
        : 0.0f;
    profileReport_.wavefrontQueues.sortedShadeCostMs = timings.wavefrontSortedShadeMs;
    profileReport_.wavefrontQueues.sortedShadeMicrosecondsPerRay = wavefrontStats.sortedShadeCheckedRays > 0u
        ? timings.wavefrontSortedShadeMs * 1000.0f / static_cast<float>(wavefrontStats.sortedShadeCheckedRays)
        : 0.0f;
    profileReport_.wavefrontQueues.sortedPathCostMs = timings.wavefrontSortMs + timings.wavefrontSortedTraceMs + timings.wavefrontSortedShadeMs;
    profileReport_.wavefrontQueues.sortedPathMicrosecondsPerRay = wavefrontStats.sortedShadeCheckedRays > 0u
        ? profileReport_.wavefrontQueues.sortedPathCostMs * 1000.0f / static_cast<float>(wavefrontStats.sortedShadeCheckedRays)
        : 0.0f;
    profileReport_.wavefrontQueues.sortNetBenefitEvidenceAvailable =
        wavefrontStats.secondaryShadeValidationPassed &&
        wavefrontStats.sortedShadeValidationPassed &&
        wavefrontStats.secondaryShadeCheckedRays == wavefrontStats.sortedShadeCheckedRays &&
        wavefrontStats.secondaryShadeCheckedRays > 0u;
    profileReport_.wavefrontQueues.sortNetBenefitMs = profileReport_.wavefrontQueues.secondaryPathCostMs -
        profileReport_.wavefrontQueues.sortedPathCostMs;
    profileReport_.wavefrontQueues.sortNetBenefitPassed =
        profileReport_.wavefrontQueues.sortNetBenefitEvidenceAvailable &&
        profileReport_.wavefrontQueues.sortNetBenefitMs > 0.0f;
    profileReport_.wavefrontQueues.shadowTraceCostMs = timings.wavefrontShadowTraceMs;
    profileReport_.wavefrontQueues.shadowTraceMicrosecondsPerRay = wavefrontStats.shadowTraceCheckedRays > 0u
        ? timings.wavefrontShadowTraceMs * 1000.0f / static_cast<float>(wavefrontStats.shadowTraceCheckedRays)
        : 0.0f;
    profileReport_.wavefrontQueues.rayQueueBytes = wavefrontStats.rayQueueBytes;
    profileReport_.wavefrontQueues.compactedRayQueueBytes = wavefrontStats.compactedRayQueueBytes;
    profileReport_.wavefrontQueues.sortedRayQueueBytes = wavefrontStats.sortedRayQueueBytes;
    profileReport_.wavefrontQueues.hitQueueBytes = wavefrontStats.hitQueueBytes;
    profileReport_.wavefrontQueues.shadowQueueBytes = wavefrontStats.shadowQueueBytes;
    profileReport_.wavefrontQueues.pixelStateBytes = wavefrontStats.pixelStateBytes;
    profileReport_.wavefrontQueues.totalBytes = wavefrontStats.totalBytes;
    profileReport_.wavefrontQueues.transientArenaUsedBytes = wavefrontStats.transientArenaUsedBytes;
    profileReport_.wavefrontQueues.transientArenaHighWaterBytes = wavefrontStats.transientArenaHighWaterBytes;
    profileReport_.wavefrontQueues.transientArenaCapacityBytes = wavefrontStats.transientArenaCapacityBytes;

    const bool wavefrontProbeActive = renderer->settings().wavefrontQueuesEnabled || wavefrontStats.buffersAllocated;
    profileReport_.wavefrontValidation.enabled = config_.wavefrontValidationMode || wavefrontProbeActive;
    profileReport_.wavefrontValidation.mode = config_.wavefrontValidationMode
        ? "side_by_side_classic_direct"
        : (wavefrontProbeActive ? "wavefront_probe" : "disabled");
    const uint32_t wavefrontTraceMismatchCount =
        wavefrontStats.traceHitMismatchCount +
        wavefrontStats.traceInstanceMismatchCount +
        wavefrontStats.traceDepthMismatchCount +
        wavefrontStats.traceNormalMismatchCount;
    const uint32_t wavefrontParityTolerance = std::max(1u, wavefrontStats.expectedPrimaryRayCount / 1000000u);
    profileReport_.wavefrontValidation.primaryGenerationPassed =
        wavefrontStats.primaryGenerationEnabled &&
        wavefrontStats.primaryGenerationValidationPassed;
    profileReport_.wavefrontValidation.tracePassed =
        wavefrontStats.traceEnabled &&
        wavefrontStats.traceCheckedPixels > 0u &&
        (wavefrontStats.traceValidationPassed || wavefrontTraceMismatchCount <= wavefrontParityTolerance);
    profileReport_.wavefrontValidation.shadePassed =
        wavefrontStats.shadeEnabled &&
        wavefrontStats.shadeCheckedPixels == wavefrontStats.expectedPrimaryRayCount &&
        wavefrontStats.shadeHitCount + wavefrontStats.shadeMissCount == wavefrontStats.shadeCheckedPixels;
    profileReport_.wavefrontValidation.compactPassed =
        wavefrontStats.compactEnabled &&
        wavefrontStats.compactOutputRayCount == wavefrontStats.compactLiveRayCount &&
        wavefrontStats.compactDroppedInvalidCount == 0u &&
        wavefrontStats.compactOverflowCount == 0u &&
        wavefrontStats.compactInvalidPixelCount == 0u &&
        wavefrontStats.compactMappingMismatchCount == 0u;
    profileReport_.wavefrontValidation.secondaryShadePassed =
        wavefrontStats.secondaryShadeEnabled &&
        wavefrontStats.secondaryShadeCheckedRays == wavefrontStats.compactOutputRayCount &&
        wavefrontStats.secondaryShadeHitCount + wavefrontStats.secondaryShadeMissCount == wavefrontStats.secondaryShadeCheckedRays;
    profileReport_.wavefrontValidation.shadowTracePassed =
        wavefrontStats.shadowTraceEnabled &&
        wavefrontStats.shadowTraceCheckedRays == wavefrontStats.shadeShadowRayCount &&
        wavefrontStats.shadowTraceAppliedCount <= wavefrontStats.shadowTraceCheckedRays;
    profileReport_.wavefrontValidation.directLightingParityPassed =
        wavefrontStats.directLightingCheckedPixels > 0u &&
        wavefrontStats.directLightingMismatchCount <= wavefrontParityTolerance;
    profileReport_.wavefrontValidation.queueBalancePassed =
        wavefrontStats.buffersAllocated &&
        wavefrontStats.clearValidationPassed &&
        !profileReport_.wavefrontQueues.queueStarvationDetected &&
        profileReport_.wavefrontQueues.queueOverflowCount == 0u;
    profileReport_.wavefrontValidation.checkedPixels = std::max(
        wavefrontStats.traceCheckedPixels,
        wavefrontStats.directLightingCheckedPixels);
    profileReport_.wavefrontValidation.checkedSecondaryRays = wavefrontStats.secondaryShadeCheckedRays;
    profileReport_.wavefrontValidation.checkedShadowRays = wavefrontStats.shadowTraceCheckedRays;
    profileReport_.wavefrontValidation.directLightingMismatchCount = wavefrontStats.directLightingMismatchCount;
    profileReport_.wavefrontValidation.directLightingMaxAbsError = wavefrontStats.directLightingMaxAbsError;
    profileReport_.wavefrontValidation.directLightingMaxRelativeError = wavefrontStats.directLightingMaxRelativeError;
    profileReport_.wavefrontValidation.secondaryShadeGpuMs = timings.wavefrontSecondaryShadeMs;
    profileReport_.wavefrontValidation.shadowTraceGpuMs = timings.wavefrontShadowTraceMs;
    profileReport_.wavefrontValidation.wavefrontProbeGpuMs =
        timings.wavefrontTraceMs +
        timings.wavefrontShadeMs +
        timings.wavefrontCompactMs +
        timings.wavefrontSecondaryTraceMs +
        timings.wavefrontSecondaryShadeMs +
        timings.wavefrontSortedTraceMs +
        timings.wavefrontShadowTraceMs +
        timings.wavefrontSortMs +
        timings.wavefrontSortedShadeMs;
    profileReport_.wavefrontValidation.allRequiredPassed = profileReport_.wavefrontValidation.enabled &&
        profileReport_.wavefrontValidation.primaryGenerationPassed &&
        profileReport_.wavefrontValidation.tracePassed &&
        profileReport_.wavefrontValidation.shadePassed &&
        profileReport_.wavefrontValidation.compactPassed &&
        profileReport_.wavefrontValidation.secondaryShadePassed &&
        profileReport_.wavefrontValidation.shadowTracePassed &&
        profileReport_.wavefrontValidation.directLightingParityPassed &&
        profileReport_.wavefrontValidation.queueBalancePassed;
    profileReport_.opacityMicromap.preprocess.subdivisionLevel = rtStats.opacityMicromapPreprocess.subdivisionLevel;
    profileReport_.opacityMicromap.preprocess.eligiblePrimitiveCount = rtStats.opacityMicromapPreprocess.eligiblePrimitiveCount;
    profileReport_.opacityMicromap.preprocess.generatedPrimitiveCount = rtStats.opacityMicromapPreprocess.generatedPrimitiveCount;
    profileReport_.opacityMicromap.preprocess.alphaTexturePrimitiveCount = rtStats.opacityMicromapPreprocess.alphaTexturePrimitiveCount;
    profileReport_.opacityMicromap.preprocess.constantAlphaPrimitiveCount = rtStats.opacityMicromapPreprocess.constantAlphaPrimitiveCount;
    profileReport_.opacityMicromap.preprocess.cacheEntryCount = rtStats.opacityMicromapPreprocess.cacheEntryCount;
    profileReport_.opacityMicromap.preprocess.cacheHitCount = rtStats.opacityMicromapPreprocess.cacheHitCount;
    profileReport_.opacityMicromap.preprocess.totalTriangleCount = rtStats.opacityMicromapPreprocess.totalTriangleCount;
    profileReport_.opacityMicromap.preprocess.microTriangleCount = rtStats.opacityMicromapPreprocess.microTriangleCount;
    profileReport_.opacityMicromap.preprocess.opaqueCount = rtStats.opacityMicromapPreprocess.opaqueCount;
    profileReport_.opacityMicromap.preprocess.transparentCount = rtStats.opacityMicromapPreprocess.transparentCount;
    profileReport_.opacityMicromap.preprocess.unknownCount = rtStats.opacityMicromapPreprocess.unknownCount;
    profileReport_.opacityMicromap.preprocess.mixedCount = rtStats.opacityMicromapPreprocess.mixedCount;
    profileReport_.opacityMicromap.preprocess.dataBytes = rtStats.opacityMicromapPreprocess.dataBytes;
    profileReport_.opacityMicromap.preprocess.preprocessingMs = rtStats.opacityMicromapPreprocess.preprocessingMs;
    profileReport_.opacityMicromap.preprocess.validationErrorCount = rtStats.opacityMicromapPreprocess.validationErrorCount;
    profileReport_.opacityMicromap.preprocess.warnings = rtStats.opacityMicromapPreprocess.warnings;
    profileReport_.opacityMicromap.build.requested = rtStats.opacityMicromapBuild.requested;
    profileReport_.opacityMicromap.build.supported = rtStats.opacityMicromapBuild.supported;
    profileReport_.opacityMicromap.build.active = rtStats.opacityMicromapBuild.active;
    profileReport_.opacityMicromap.build.micromapCount = rtStats.opacityMicromapBuild.micromapCount;
    profileReport_.opacityMicromap.build.meshCount = rtStats.opacityMicromapBuild.meshCount;
    profileReport_.opacityMicromap.build.triangleArrayCount = rtStats.opacityMicromapBuild.triangleArrayCount;
    profileReport_.opacityMicromap.build.indexedTriangleCount = rtStats.opacityMicromapBuild.indexedTriangleCount;
    profileReport_.opacityMicromap.build.packedMicroTriangleCount = rtStats.opacityMicromapBuild.packedMicroTriangleCount;
    profileReport_.opacityMicromap.build.micromapBytes = static_cast<uint64_t>(rtStats.opacityMicromapBuild.micromapBytes);
    profileReport_.opacityMicromap.build.buildInputBytes = static_cast<uint64_t>(rtStats.opacityMicromapBuild.buildInputBytes);
    profileReport_.opacityMicromap.build.buildScratchBytes = static_cast<uint64_t>(rtStats.opacityMicromapBuild.buildScratchBytes);
    profileReport_.opacityMicromap.build.buildMs = rtStats.opacityMicromapBuild.buildMs;
    profileReport_.opacityMicromap.build.fallbackReason = rtStats.opacityMicromapBuild.fallbackReason;
    profileReport_.memory.texturesBytes = static_cast<uint64_t>(renderer->estimatedTextureMemory());
    profileReport_.memory.buffersBytes = static_cast<uint64_t>(renderer->estimatedBufferMemory());
    profileReport_.memory.temporalHistoryBytes = static_cast<uint64_t>(renderer->temporalHistoryMemory());
    profileReport_.memory.restirReservoirBytes = static_cast<uint64_t>(renderer->restirReservoirMemory());
    const auto reservoirBreakdown = renderer->restirReservoirMemoryBreakdown();
    profileReport_.memory.restirDiCurrentBytes = static_cast<uint64_t>(reservoirBreakdown.diCurrentBytes);
    profileReport_.memory.restirDiInitialBytes = static_cast<uint64_t>(reservoirBreakdown.diInitialBytes);
    profileReport_.memory.restirDiTemporalBytes = static_cast<uint64_t>(reservoirBreakdown.diTemporalBytes);
    profileReport_.memory.restirDiPreviousBytes = static_cast<uint64_t>(reservoirBreakdown.diPreviousBytes);
    profileReport_.memory.restirDiSpatialBytes = static_cast<uint64_t>(reservoirBreakdown.diSpatialBytes);
    profileReport_.memory.restirDiFinalBytes = static_cast<uint64_t>(reservoirBreakdown.diFinalBytes);
    profileReport_.memory.restirDiReceiverBytes = static_cast<uint64_t>(reservoirBreakdown.diReceiverBytes);
    profileReport_.memory.restirDiPreviousReceiverBytes = static_cast<uint64_t>(reservoirBreakdown.diPreviousReceiverBytes);
    profileReport_.memory.restirDiCountersBytes = static_cast<uint64_t>(reservoirBreakdown.diCountersBytes);
    profileReport_.memory.restirDiPhysicalBytes = static_cast<uint64_t>(reservoirBreakdown.diPhysicalBytes);
    profileReport_.memory.restirDiAliasSavingsBytes = static_cast<uint64_t>(reservoirBreakdown.diAliasSavingsBytes);
    profileReport_.memory.restirGiCurrentBytes = static_cast<uint64_t>(reservoirBreakdown.giCurrentBytes);
    profileReport_.memory.restirGiPreviousBytes = static_cast<uint64_t>(reservoirBreakdown.giPreviousBytes);
    profileReport_.memory.restirGiSpatialBytes = static_cast<uint64_t>(reservoirBreakdown.giSpatialBytes);
    profileReport_.memory.restirGiProductionTemporalBytes = static_cast<uint64_t>(reservoirBreakdown.giProductionTemporalBytes);
    profileReport_.memory.restirGiProductionSpatialBytes = static_cast<uint64_t>(reservoirBreakdown.giProductionSpatialBytes);
    profileReport_.memory.restirGiProductionPreviousBytes = static_cast<uint64_t>(reservoirBreakdown.giProductionPreviousBytes);
    profileReport_.memory.restirGiProductionUpsampledBytes = static_cast<uint64_t>(reservoirBreakdown.giProductionUpsampledBytes);
    profileReport_.memory.restirGiActiveTileMaskBytes = static_cast<uint64_t>(reservoirBreakdown.giActiveTileMaskBytes);
    profileReport_.memory.restirGiCountersBytes = static_cast<uint64_t>(reservoirBreakdown.giCountersBytes);
    profileReport_.memory.restirGiReceiverBytes = static_cast<uint64_t>(reservoirBreakdown.giReceiverBytes);
    profileReport_.memory.restirGiPreviousReceiverBytes = static_cast<uint64_t>(reservoirBreakdown.giPreviousReceiverBytes);
    if (auto* uploader = app.bufferUploader()) {
        const auto& uploadStats = uploader->stats();
        profileReport_.memory.stagingUploadTotalBytes = uploadStats.totalUploadedBytes;
        profileReport_.memory.stagingUploadPeakBytes = uploadStats.stagingPeakBytes;
        profileReport_.memory.stagingUploadLastBytes = uploadStats.lastStagingBytes;
        profileReport_.memory.stagingUploadCount = uploadStats.uploadCount;
        profileReport_.memory.stagingBufferUploadCount = uploadStats.bufferUploadCount;
        profileReport_.memory.stagingImageUploadCount = uploadStats.imageUploadCount;
        profileReport_.memory.stagingBatchUploadCount = uploadStats.batchUploadCount;
    }
    if (auto* uiOverlay = app.uiOverlay()) {
        const auto uiStats = uiOverlay->descriptorPoolStats();
        profileReport_.memory.ui.present = uiStats.present;
        profileReport_.memory.ui.descriptorMaxSets = uiStats.maxSets;
        profileReport_.memory.ui.combinedImageSamplerDescriptors = uiStats.combinedImageSamplerDescriptors;
        profileReport_.memory.ui.sampledImageDescriptors = uiStats.sampledImageDescriptors;
        profileReport_.memory.ui.samplerDescriptors = uiStats.samplerDescriptors;
        profileReport_.memory.ui.viewportDescriptorAllocated = uiStats.viewportDescriptorAllocated;
    }
    const auto descriptorStats = renderer->descriptorPoolStats();
    profileReport_.memory.descriptors.setsPerPool = descriptorStats.setsPerPool;
    profileReport_.memory.descriptors.maxPools = descriptorStats.maxPools;
    profileReport_.memory.descriptors.usedPools = descriptorStats.usedPools;
    profileReport_.memory.descriptors.freePools = descriptorStats.freePools;
    profileReport_.memory.descriptors.poolCount = descriptorStats.poolCount;
    profileReport_.memory.descriptors.capacitySets = descriptorStats.capacitySets;
    profileReport_.memory.descriptors.allocatedSets = descriptorStats.allocatedSets;
    profileReport_.memory.descriptors.peakAllocatedSets = descriptorStats.peakAllocatedSets;
    profileReport_.memory.descriptors.failedAllocations = descriptorStats.failedAllocations;
    profileReport_.memory.descriptors.fragmentedPoolFailures = descriptorStats.fragmentedPoolFailures;
    profileReport_.memory.descriptors.poolGrowthCount = descriptorStats.poolGrowthCount;
    profileReport_.restirGiLayout = renderer->restirGiReservoirLayoutName();

    const auto adaptiveState = renderer->adaptiveQualityState();
    profileReport_.adaptiveQuality.smoothedGpuMs = adaptiveState.smoothedGpuMs;
    profileReport_.adaptiveQuality.tier = adaptiveState.tier;
    profileReport_.adaptiveQuality.overBudgetFrames = adaptiveState.overBudgetFrames;
    profileReport_.adaptiveQuality.effectiveMaxBounces = adaptiveState.effectiveMaxBounces;
    profileReport_.adaptiveQuality.effectiveEnvironmentSamples = adaptiveState.effectiveEnvironmentSamples;
    profileReport_.adaptiveQuality.effectiveAtrousIterations = adaptiveState.effectiveAtrousIterations;
    profileReport_.adaptiveQuality.skipRestirSpatial = adaptiveState.skipRestirSpatial;
    profileReport_.adaptiveQuality.skipDenoiser = adaptiveState.skipDenoiser;

    const auto memoryPressureState = renderer->memoryPressureQualityState();
    profileReport_.memoryPressureQuality.active = memoryPressureState.active;
    profileReport_.memoryPressureQuality.overrideActive = memoryPressureState.overrideActive;
    profileReport_.memoryPressureQuality.tier = memoryPressureState.tier;
    profileReport_.memoryPressureQuality.usageRatio = memoryPressureState.usageRatio;
    profileReport_.memoryPressureQuality.pressure = memoryPressureState.pressure;
    profileReport_.memoryPressureQuality.effectiveRenderScale = memoryPressureState.effectiveRenderScale;
    profileReport_.memoryPressureQuality.limitSamplesPerPixel = memoryPressureState.limitSamplesPerPixel;
    profileReport_.memoryPressureQuality.restirGiHalfResolution = memoryPressureState.restirGiHalfResolution;
    profileReport_.memoryPressureQuality.denoiserMaxHistoryLength = memoryPressureState.denoiserMaxHistoryLength;

    profileReport_.sceneUpdateRoutes.clear();
    for (const SceneUpdateRouteEvent& route : renderer->validationLog().sceneUpdateRoutes()) {
        ProfileReport::SceneUpdateRouteReport report;
        report.kind = route.kind;
        report.action = route.action;
        report.count = route.count;
        report.totalCpuMs = route.totalCpuMs;
        report.lastCpuMs = route.lastCpuMs;
        report.averageCpuMs = route.count > 0 ? route.totalCpuMs / static_cast<double>(route.count) : 0.0;
        report.minCpuMs = route.minCpuMs;
        report.maxCpuMs = route.maxCpuMs;
        profileReport_.sceneUpdateRoutes.push_back(std::move(report));
    }
    profileReport_.schedulerQueues.clear();
    for (const SchedulerQueueEvent& event : renderer->validationLog().schedulerQueueEvents()) {
        ProfileReport::SchedulerQueueReport report;
        report.queue = event.queue;
        report.job = event.job;
        report.status = event.status;
        report.generation = event.generation;
        report.count = event.count;
        report.totalCpuMs = event.totalCpuMs;
        report.lastCpuMs = event.lastCpuMs;
        report.averageCpuMs = event.count > 0 ? event.totalCpuMs / static_cast<double>(event.count) : 0.0;
        report.minCpuMs = event.minCpuMs;
        report.maxCpuMs = event.maxCpuMs;
        report.frameBudgetViolationCount = event.frameBudgetViolationCount;
        profileReport_.schedulerQueues.push_back(std::move(report));
    }
    profileReport_.gpuUploadTickets = app.editorGpuUploadTicketSnapshots(true);
    profileReport_.gpuUploadNextTimelineValue = app.editorGpuUploadNextTimelineValue();
    profileReport_.mainThreadApplyTickets = app.editorMainThreadApplyTicketSnapshots(true);
    profileReport_.topologyRebuildTickets = app.editorTopologyRebuildTicketSnapshots(true);
    profileReport_.topologyRebuildLatestGeneration = app.editorTopologyRebuildLatestGeneration();
    profileReport_.topologyRebuildNextTimelineValue = app.editorTopologyRebuildNextTimelineValue();
    appendCompletedUploaderSchedulerReport(profileReport_);
    appendCompletedMainThreadApplySchedulerReport(profileReport_);
    appendCompletedTopologySchedulerReport(profileReport_);
    profileReport_.validationErrorCount = 0;

    profileReport_.settings = renderer->settings();
    profileReport_.effectiveRestirHistoryCopyMode = renderer->effectiveRestirHistoryCopyMode();
    profileReport_.effectiveRestirGiActiveTileMaskEnabled = renderer->effectiveRestirGiActiveTileMaskEnabled();
    if (const char* reason = renderer->restirHistoryCopyFallbackReason()) {
        profileReport_.restirHistoryCopyFallbackReason = reason;
    }
    const auto nvidiaStatus = renderer->nvidiaIntegrationStatus();
    profileReport_.nvidiaIntegrations.nrdSdkConfigured = nvidiaStatus.nrdSdkConfigured;
    profileReport_.nvidiaIntegrations.nrdRequestable = nvidiaStatus.nrdRequestable;
    profileReport_.nvidiaIntegrations.nrdAvailable = nvidiaStatus.nrdAvailable;
    profileReport_.nvidiaIntegrations.nrdUnavailableReason = nvidiaStatus.nrdUnavailableReason;
    profileReport_.nvidiaIntegrations.requestedDenoiserBackend = denoiserBackendName(profileReport_.settings.denoiserBackend);
    profileReport_.nvidiaIntegrations.effectiveDenoiserBackend = denoiserBackendName(renderer->effectiveDenoiserBackend());
    if (profileReport_.settings.denoiserBackend != DenoiserBackend::Nrd) {
        profileReport_.nvidiaIntegrations.nrdBackendPolicy = "disabled";
        profileReport_.nvidiaIntegrations.nrdBackendPolicyReason = "Engine denoiser selected";
    } else if (renderer->effectiveDenoiserBackend() == DenoiserBackend::Nrd) {
        profileReport_.nvidiaIntegrations.nrdBackendPolicy = "direct_nrd";
        profileReport_.nvidiaIntegrations.nrdBackendPolicyReason =
            "Direct NRD runtime selected; Streamline NRD is not used in this frame";
    } else {
        profileReport_.nvidiaIntegrations.nrdBackendPolicy = "engine_fallback";
        profileReport_.nvidiaIntegrations.nrdBackendPolicyReason = nvidiaStatus.nrdUnavailableReason;
    }
    profileReport_.nvidiaIntegrations.nrdBackendsMutuallyExclusive =
        renderer->effectiveDenoiserBackend() != DenoiserBackend::Nrd || !nvidiaStatus.streamlineNrd.requestable;
    profileReport_.nvidiaIntegrations.dlssSdkConfigured = nvidiaStatus.dlssSdkConfigured;
    profileReport_.nvidiaIntegrations.dlssAvailable = nvidiaStatus.dlssAvailable;
    profileReport_.nvidiaIntegrations.dlssUnavailableReason = nvidiaStatus.dlssUnavailableReason;
    profileReport_.nvidiaIntegrations.dlssRayReconstructionAvailable = nvidiaStatus.dlssRayReconstructionAvailable;
    profileReport_.nvidiaIntegrations.dlssRayReconstructionUnavailableReason = nvidiaStatus.dlssRayReconstructionUnavailableReason;
    profileReport_.nvidiaIntegrations.requestedDlssRayReconstruction = profileReport_.settings.dlssRayReconstructionEnabled;
    profileReport_.nvidiaIntegrations.effectiveDlssRayReconstruction = renderer->dlssRayReconstructionActive();
    profileReport_.nvidiaIntegrations.dlssFrameGenerationAvailable = nvidiaStatus.dlssFrameGenerationAvailable;
    profileReport_.nvidiaIntegrations.dlssFrameGenerationUnavailableReason = nvidiaStatus.dlssFrameGenerationUnavailableReason;
    profileReport_.nvidiaIntegrations.requestedDlssFrameGeneration = profileReport_.settings.dlssFrameGenerationEnabled;
    profileReport_.nvidiaIntegrations.effectiveDlssFrameGeneration =
        profileReport_.settings.dlssFrameGenerationEnabled && nvidiaStatus.dlssFrameGenerationAvailable;
    profileReport_.nvidiaIntegrations.dlssSharpeningStrength = profileReport_.settings.dlssSharpeningStrength;
    profileReport_.nvidiaIntegrations.requestedTemporalUpscaler = temporalUpscalerName(profileReport_.settings.temporalUpscaler);
    profileReport_.nvidiaIntegrations.effectiveTemporalUpscaler = temporalUpscalerName(renderer->effectiveTemporalUpscaler());
    profileReport_.nvidiaIntegrations.streamlineSdkConfigured = nvidiaStatus.streamlineSdkConfigured;
    profileReport_.nvidiaIntegrations.streamlineRuntimeConfigured = nvidiaStatus.streamlineRuntimeConfigured;
    profileReport_.nvidiaIntegrations.streamlineInitialized = nvidiaStatus.streamlineInitialized;
    profileReport_.nvidiaIntegrations.streamlineVulkanInfoSet = nvidiaStatus.streamlineVulkanInfoSet;
    profileReport_.nvidiaIntegrations.streamlineRuntimeDirectory = nvidiaStatus.streamlineRuntimeDirectory;
    profileReport_.nvidiaIntegrations.streamlineUnavailableReason = nvidiaStatus.streamlineUnavailableReason;
    profileReport_.nvidiaIntegrations.requestedStreamlineReflex = profileReport_.settings.streamlineReflexEnabled;
    profileReport_.nvidiaIntegrations.effectiveStreamlineReflex =
        profileReport_.settings.streamlineReflexEnabled && nvidiaStatus.streamlineReflex.requestable;
    auto copyStreamlineFeature = [](const StreamlineFeatureStatus& source) {
        ProfileReport::NvidiaIntegrationReport::StreamlineFeatureReport report;
        report.requestable = source.requestable;
        report.supported = source.supported;
        report.unavailableReason = source.unavailableReason;
        report.requirements = source.requirements;
        return report;
    };
    auto copyStreamlineTags = [](const PathTracerRenderer::StreamlineTagSummary& source) {
        ProfileReport::NvidiaIntegrationReport::StreamlineTagReport report;
        report.expected = source.expected;
        report.tagged = source.tagged;
        report.failed = source.failed;
        report.missingFrameToken = source.missingFrameToken;
        report.missingImage = source.missingImage;
        report.invalidLayout = source.invalidLayout;
        report.invalidFormat = source.invalidFormat;
        report.invalidExtent = source.invalidExtent;
        report.emptyRole = source.emptyRole;
        report.runtimeRejected = source.runtimeRejected;
        report.frameIndex = source.frameIndex;
        report.attempted = source.attempted;
        return report;
    };
    auto copyStreamlineEvaluation = [](const PathTracerRenderer::StreamlineEvaluationSummary& source) {
        ProfileReport::NvidiaIntegrationReport::StreamlineEvaluationReport report;
        report.attempted = source.attempted;
        report.succeeded = source.succeeded;
        report.failed = source.failed;
        report.skippedUnsupported = source.skippedUnsupported;
        report.skippedMissingTags = source.skippedMissingTags;
        report.frameIndex = source.frameIndex;
        return report;
    };
    auto copyStreamlineReflexMarkers = [](const PathTracerRenderer::StreamlineReflexMarkerSummary& source) {
        ProfileReport::NvidiaIntegrationReport::StreamlineReflexMarkerReport report;
        report.attempted = source.attempted;
        report.succeeded = source.succeeded;
        report.failed = source.failed;
        report.skippedUnavailable = source.skippedUnavailable;
        report.frameIndex = source.frameIndex;
        return report;
    };
    profileReport_.nvidiaIntegrations.streamlineDlss = copyStreamlineFeature(nvidiaStatus.streamlineDlss);
    profileReport_.nvidiaIntegrations.streamlineDlssRayReconstruction = copyStreamlineFeature(nvidiaStatus.streamlineDlssRayReconstruction);
    profileReport_.nvidiaIntegrations.streamlineDlssFrameGeneration = copyStreamlineFeature(nvidiaStatus.streamlineDlssFrameGeneration);
    profileReport_.nvidiaIntegrations.streamlineReflex = copyStreamlineFeature(nvidiaStatus.streamlineReflex);
    profileReport_.nvidiaIntegrations.streamlineNis = copyStreamlineFeature(nvidiaStatus.streamlineNis);
    profileReport_.nvidiaIntegrations.streamlineNrd = copyStreamlineFeature(nvidiaStatus.streamlineNrd);
    profileReport_.nvidiaIntegrations.streamlineDlssTags = copyStreamlineTags(nvidiaStatus.streamlineDlssTags);
    profileReport_.nvidiaIntegrations.streamlineDlssRayReconstructionTags = copyStreamlineTags(nvidiaStatus.streamlineDlssRayReconstructionTags);
    profileReport_.nvidiaIntegrations.streamlineDlssEvaluation = copyStreamlineEvaluation(nvidiaStatus.streamlineDlssEvaluation);
    profileReport_.nvidiaIntegrations.streamlineDlssRayReconstructionEvaluation = copyStreamlineEvaluation(nvidiaStatus.streamlineDlssRayReconstructionEvaluation);
    profileReport_.nvidiaIntegrations.streamlineReflexMarkers = copyStreamlineReflexMarkers(nvidiaStatus.streamlineReflexMarkers);
    if (profileReport_.settings.denoiserBackend == DenoiserBackend::Nrd && !nvidiaStatus.nrdAvailable) {
        profileReport_.warnings.push_back("NRD denoiser requested but unavailable: " + nvidiaStatus.nrdUnavailableReason);
    }
    if (profileReport_.settings.temporalUpscaler == TemporalUpscaler::Dlss && !nvidiaStatus.dlssAvailable) {
        profileReport_.warnings.push_back("DLSS requested but unavailable: " + nvidiaStatus.dlssUnavailableReason);
    }
    if (profileReport_.settings.temporalUpscaler == TemporalUpscaler::Nis &&
        renderer->effectiveTemporalUpscaler() != TemporalUpscaler::Nis) {
        profileReport_.warnings.push_back("NIS requested but unavailable: " + nvidiaStatus.streamlineNis.unavailableReason);
    }
    if (profileReport_.settings.dlssRayReconstructionEnabled && !nvidiaStatus.dlssRayReconstructionAvailable) {
        profileReport_.warnings.push_back("DLSS Ray Reconstruction requested but unavailable: " + nvidiaStatus.dlssRayReconstructionUnavailableReason);
    }
    if (profileReport_.settings.dlssFrameGenerationEnabled && !nvidiaStatus.dlssFrameGenerationAvailable) {
        profileReport_.warnings.push_back("DLSS Frame Generation requested but unavailable: " + nvidiaStatus.dlssFrameGenerationUnavailableReason);
    }

    // Read back DI counters if available
    if (auto* diCounters = renderer->restirDiCounterData()) {
        profileReport_.restirDiCounters.assign(diCounters, diCounters + 64);
    }
    if (auto* giCounters = renderer->restirGiCounterData()) {
        profileReport_.restirGiCounters.assign(giCounters, giCounters + 64);
    }
    profileReport_.restirDiHistoryValid = renderer->restirDiHistoryValid();

    return profileReport_;
}

void HeadlessDiagnostics::writeProfileJson(const std::filesystem::path& path) const {
    nlohmann::json j;
    j["engine_version"] = profileReport_.engineVersion;
    j["git_commit"] = profileReport_.gitCommit;
    j["gpu_name"] = profileReport_.gpuName;
    j["driver_version"] = profileReport_.driverVersion;
    j["vulkan_version"] = profileReport_.vulkanVersion;
    j["restir_gi_layout"] = profileReport_.restirGiLayout;
    j["resolution"] = profileReport_.resolution;
    j["frame_count"] = profileReport_.frameCount;
    j["warmup_frames"] = profileReport_.warmupFrames;
    j["profiled_frames"] = profileReport_.profiledFrames;
    j["cpu_frame_ms"] = profileReport_.cpuFrameMs;
    j["gpu_frame_ms"] = profileReport_.gpuFrameMs;
    j["per_pass_gpu_ms"] = profileReport_.perPassGpuMs;
    j["per_pass_gpu_ms_p95"] = profileReport_.perPassGpuMsP95;
    j["per_pass_gpu_ms_p99"] = profileReport_.perPassGpuMsP99;
    j["queue_lane_ms"] = profileReport_.queueLaneMs;
    j["async_compute"] = profileReport_.asyncCompute;
    j["opacity_micromap"] = profileReport_.opacityMicromap;
    j["shader_execution_reordering"] = profileReport_.shaderExecutionReordering;
    j["ray_tracing_motion_blur"] = profileReport_.rayTracingMotionBlur;
    j["pipeline_statistics"] = profileReport_.pipelineStatistics;
    j["ray_tracing_geometry"] = profileReport_.rayTracingGeometry;
    j["animated_geometry"] = profileReport_.animatedGeometry;
    j["scene_lights"] = profileReport_.sceneLights;
    j["wavefront_queues"] = profileReport_.wavefrontQueues;
    j["wavefront_validation"] = profileReport_.wavefrontValidation;
    const uint64_t hitCount = profileReport_.pipelineStatistics.triangleHits + profileReport_.pipelineStatistics.aabbHits;
    j["gpu_debug_counters"] = {
        {"ray_count", profileReport_.pipelineStatistics.rayInvocations},
        {"shadow_ray_count", nullptr},
        {"hit_count", hitCount},
        {"miss_count", profileReport_.pipelineStatistics.rayInvocations > hitCount ? profileReport_.pipelineStatistics.rayInvocations - hitCount : 0},
        {"ray_tracing_any_hit", profileReport_.rayTracingDiagnosticCounters},
        {"path_length_histogram", nlohmann::json::array()},
        {"restir_accepted_count", nullptr},
        {"restir_rejected_count", nullptr},
        {"taa_history_accepted_count", nullptr},
        {"taa_history_rejected_count", nullptr},
        {"denoiser_history_accepted_count", nullptr},
        {"denoiser_history_rejected_count", nullptr},
        {"notes", nlohmann::json::array({
            "ray_count/hit_count/miss_count come from Vulkan pipeline statistics when available",
            "ray_tracing_any_hit is shader-instrumented during profile runs and reports the last completed frame",
            "remaining counters require shader atomic instrumentation and are intentionally null until instrumented"
        })},
    };
    j["memory"] = profileReport_.memory;
    j["restir_di_counters"] = profileReport_.restirDiCounters;
    j["restir_gi_counters"] = profileReport_.restirGiCounters;
    auto diCounter = [this](size_t index) -> uint64_t {
        return index < profileReport_.restirDiCounters.size()
            ? static_cast<uint64_t>(profileReport_.restirDiCounters[index])
            : 0ull;
    };
    auto ratioOrNull = [](uint64_t numerator, uint64_t denominator) -> nlohmann::json {
        return denominator == 0ull
            ? nlohmann::json(nullptr)
            : nlohmann::json(static_cast<double>(numerator) / static_cast<double>(denominator));
    };
    auto diCounter64 = [&diCounter](size_t lowIndex, size_t highIndex) -> uint64_t {
        return diCounter(lowIndex) | (diCounter(highIndex) << 32u);
    };
    auto averageOrNull = [](uint64_t sum, uint64_t count, double scale = 1.0) -> nlohmann::json {
        return count == 0ull
            ? nlohmann::json(nullptr)
            : nlohmann::json(static_cast<double>(sum) / (static_cast<double>(count) * scale));
    };
    const uint64_t temporalAccepted = diCounter(29);
    const uint64_t temporalSelected = temporalAccepted + diCounter(30);
    const uint64_t spatialTested = diCounter(33);
    const uint64_t spatialAccepted = diCounter(34);
    const uint64_t finalPixels = diCounter(48);
    const uint64_t finalFallback = diCounter(51);
    const uint64_t unknownVisibility = diCounter(58);
    const uint64_t clampCount = diCounter(31) + diCounter(40) + diCounter(55);
    const uint64_t nonFiniteCount = diCounter(4) + diCounter(13) + diCounter(57);
    const uint64_t sumM = diCounter64(14, 15);
    const uint64_t sumAge = diCounter64(43, 44);
    const uint64_t sumWeight = diCounter64(45, 46);
    const uint64_t sumLuminance = diCounter64(47, 60);
    const uint64_t finalEstimateCount = diCounter(56);
    const bool initialPassActive = diCounter(0) > 0ull;
    const bool temporalPassActive = diCounter(16) > 0ull;
    const bool spatialPassActive = spatialTested > 0ull;
    const bool finalPassActive = finalPixels > 0ull;
    const bool newDiRequested =
        profileReport_.settings.restirDiMode == RestirDiMode::Production ||
        profileReport_.settings.restirDiMode == RestirDiMode::ReferenceValidation ||
        profileReport_.settings.restirDiMode == RestirDiMode::HybridCompare;
    const uint64_t diTotalBytes =
        profileReport_.memory.restirDiInitialBytes +
        profileReport_.memory.restirDiTemporalBytes +
        profileReport_.memory.restirDiSpatialBytes +
        profileReport_.memory.restirDiFinalBytes +
        profileReport_.memory.restirDiPreviousBytes +
        profileReport_.memory.restirDiReceiverBytes +
        profileReport_.memory.restirDiPreviousReceiverBytes +
        profileReport_.memory.restirDiCountersBytes;
    nlohmann::json diWarnings = nlohmann::json::array();
    if (initialPassActive && !profileReport_.restirDiHistoryValid && newDiRequested) {
        diWarnings.push_back({
            {"code", "RESTIR_DI_HISTORY_INVALID"},
            {"severity", "warning"},
            {"pass", "restir_di_temporal"},
            {"count", 1},
        });
    }
    const uint64_t diPixels = static_cast<uint64_t>(profileReport_.resolution.renderWidth) *
        static_cast<uint64_t>(profileReport_.resolution.renderHeight);
    const uint64_t diReservoirStride = diPixels > 0ull && profileReport_.memory.restirDiInitialBytes > 0ull
        ? profileReport_.memory.restirDiInitialBytes / diPixels
        : 0ull;
    const uint64_t diReceiverStride = diPixels > 0ull && profileReport_.memory.restirDiReceiverBytes > 0ull
        ? profileReport_.memory.restirDiReceiverBytes / diPixels
        : 0ull;
    const uint32_t diReservoirBufferCount = profileReport_.memory.restirDiInitialBytes > 0ull ? 5u : 0u;
    const uint32_t diReceiverBufferCount = profileReport_.memory.restirDiReceiverBytes > 0ull ? 2u : 0u;
    j["restir_di"] = {
        {"schema_version", 1},
        {"layout_version", 3},
        {"mode", restirDiModeName(profileReport_.settings.restirDiMode)},
        {"layout", restirDiReservoirLayoutName(profileReport_.settings.restirDiReservoirLayout)},
        {"counter_scope", "last_completed_frame"},
        {"counter_value_bits", 32},
        {"aggregate_encoding", "uint32-low-high-with-carry"},
        {"history_valid", profileReport_.restirDiHistoryValid},
        {"history_reset_reason", profileReport_.restirDiHistoryValid
            ? "none"
            : (initialPassActive ? "not_finalized" : "inactive")},
        {"fallback_reason", newDiRequested && !initialPassActive
            ? "pipeline_inactive_or_unsupported"
            : "none"},
        {"light_identity", {
            {"scheme", "persistent-id-hash32-generation-cached-index"},
            {"reorder_fallback", "history-invalidation"},
            {"distribution_version_policy", "source-mass-preserved-current-pdf-recomputed"},
        }},
        {"direct_light_ownership", {
            {"emissive_and_analytic_lights", initialPassActive ? "restir_di" : "classic_nee"},
            {"sun", "classic_nee"},
            {"environment", "classic_nee"},
            {"participating_media", "classic_nee"},
        }},
        {"stabilization", {
            {"enabled", profileReport_.settings.restirDiProductionStabilizationEnabled},
            {"clamp_luminance", profileReport_.settings.restirDiClampLuminance},
            {"reference_mode", profileReport_.settings.restirDiMode == RestirDiMode::ReferenceValidation},
        }},
        {"render_extent", {
            {"width", profileReport_.resolution.renderWidth},
            {"height", profileReport_.resolution.renderHeight},
        }},
        {"active_passes", {
            {"initial", initialPassActive},
            {"temporal", temporalPassActive},
            {"spatial", spatialPassActive},
            {"final", finalPassActive},
            {"history_copy", profileReport_.restirDiHistoryValid},
        }},
        {"initial_valid_count", diCounter(1)},
        {"initial_invalid_surface_count", diCounter(2)},
        {"initial_invalid_target_count", diCounter(3)},
        {"initial_non_finite_count", diCounter(4)},
        {"initial_invalid_pdf_count", diCounter(10)},
        {"initial_invalid_identity_count", diCounter(11)},
        {"initial_weight_overflow_count", diCounter(12)},
        {"temporal_non_finite_count", diCounter(13)},
        {"initial_light_class_counts", {
            {"emissive", diCounter(5)},
            {"directional", diCounter(6)},
            {"point", diCounter(7)},
            {"area", diCounter(8)},
            {"spot", diCounter(9)},
        }},
        {"temporal_tested_count", diCounter(17)},
        {"temporal_accepted_count", temporalAccepted},
        {"temporal_rejected_count", diCounter(17) > temporalSelected ? diCounter(17) - temporalSelected : 0ull},
        {"spatial_tested_count", spatialTested},
        {"spatial_accepted_count", spatialAccepted},
        {"spatial_rejected_count", spatialTested > spatialAccepted ? spatialTested - spatialAccepted : 0ull},
        {"final_visible_count", diCounter(53)},
        {"final_occluded_count", diCounter(54)},
        {"final_valid_count", diCounter(49)},
        {"final_invalid_count", diCounter(50)},
        {"final_estimate_count", finalEstimateCount},
        {"final_fallback_count", finalFallback},
        {"clamp_count", clampCount},
        {"non_finite_count", nonFiniteCount},
        {"light_version_rejection_count", diCounter(23) + diCounter(42) + diCounter(59)},
        {"spatial_light_identity_rejection_count", diCounter(42)},
        {"final_light_identity_rejection_count", diCounter(59)},
        {"surface_rejection_count", diCounter(21) + diCounter(35)},
        {"visibility_rejection_count", diCounter(28) + diCounter(37)},
        {"temporal_acceptance_ratio", ratioOrNull(temporalAccepted, temporalSelected)},
        {"spatial_acceptance_ratio", ratioOrNull(spatialAccepted, spatialTested)},
        {"final_fallback_ratio", ratioOrNull(finalFallback, finalPixels)},
        {"unknown_visibility_ratio", ratioOrNull(unknownVisibility, finalPixels)},
        {"clamp_ratio", ratioOrNull(clampCount, finalPixels)},
        {"non_finite_ratio", ratioOrNull(nonFiniteCount, diCounter(0) + finalPixels)},
        {"aggregate_totals", {
            {"m", sumM},
            {"age", sumAge},
            {"weight_sum_fixed_8", sumWeight},
            {"final_luminance_fixed_8", sumLuminance},
            {"sample_count", finalEstimateCount},
        }},
        {"average_m", averageOrNull(sumM, finalEstimateCount)},
        {"average_age", averageOrNull(sumAge, finalEstimateCount)},
        {"average_weight_sum", averageOrNull(sumWeight, finalEstimateCount, 256.0)},
        {"average_final_luminance", averageOrNull(sumLuminance, finalEstimateCount, 256.0)},
        {"memory", {
            {"reservoir_stride", diReservoirStride},
            {"receiver_stride", diReceiverStride},
            {"reservoir_buffer_count", diReservoirBufferCount},
            {"physically_allocated_reservoir_buffer_count",
                diReservoirBufferCount > 0u && profileReport_.memory.restirDiAliasSavingsBytes > 0ull
                    ? diReservoirBufferCount - 1u
                    : diReservoirBufferCount},
            {"receiver_buffer_count", diReceiverBufferCount},
            {"initial_bytes", profileReport_.memory.restirDiInitialBytes},
            {"temporal_bytes", profileReport_.memory.restirDiTemporalBytes},
            {"spatial_bytes", profileReport_.memory.restirDiSpatialBytes},
            {"final_bytes", profileReport_.memory.restirDiFinalBytes},
            {"previous_bytes", profileReport_.memory.restirDiPreviousBytes},
            {"receiver_bytes", profileReport_.memory.restirDiReceiverBytes},
            {"previous_receiver_bytes", profileReport_.memory.restirDiPreviousReceiverBytes},
            {"counters_bytes", profileReport_.memory.restirDiCountersBytes},
            {"logical_bytes", diTotalBytes},
            {"physically_allocated_bytes", profileReport_.memory.restirDiPhysicalBytes},
            {"alias_savings_bytes", profileReport_.memory.restirDiAliasSavingsBytes},
            {"total_bytes", diTotalBytes},
        }},
        {"warnings", std::move(diWarnings)},
    };
    auto giCounter = [this](size_t index) -> uint64_t {
        return index < profileReport_.restirGiCounters.size()
            ? static_cast<uint64_t>(profileReport_.restirGiCounters[index])
            : 0ull;
    };
    const uint64_t giTemporalPixels = giCounter(0);
    const uint64_t giTemporalCurrentValid = giCounter(1);
    const uint64_t giTemporalPreviousValid = giCounter(2);
    const uint64_t giTemporalAccepted = giCounter(3);
    const uint64_t giTemporalRejected =
        giCounter(4) + giCounter(5) + giCounter(6) + giCounter(7) + giCounter(8) + giCounter(9);
    const uint64_t giSpatialPixels = giCounter(10);
    const uint64_t giSpatialTested = giCounter(11);
    const uint64_t giSpatialAccepted = giCounter(12);
    const uint64_t giFinalPixels = giCounter(17);
    const uint64_t giFinalFallback = giCounter(20);
    const uint64_t giClampCount = giCounter(21) + giCounter(27);
    const uint64_t giUpsamplePixels = giCounter(22);
    const uint64_t giUpsampleAccepted = giCounter(23);
    const uint64_t giUpsampleFallback = giCounter(24);
    constexpr double giGridFixedScale = 512.0;
    const uint64_t giPixels = static_cast<uint64_t>(profileReport_.resolution.renderWidth) *
        static_cast<uint64_t>(profileReport_.resolution.renderHeight);
    const uint64_t giXEvenCount =
        ((static_cast<uint64_t>(profileReport_.resolution.renderWidth) + 1ull) / 2ull) *
        static_cast<uint64_t>(profileReport_.resolution.renderHeight);
    const uint64_t giXOddCount =
        (static_cast<uint64_t>(profileReport_.resolution.renderWidth) / 2ull) *
        static_cast<uint64_t>(profileReport_.resolution.renderHeight);
    const uint64_t giYEvenCount =
        static_cast<uint64_t>(profileReport_.resolution.renderWidth) *
        ((static_cast<uint64_t>(profileReport_.resolution.renderHeight) + 1ull) / 2ull);
    const uint64_t giYOddCount =
        static_cast<uint64_t>(profileReport_.resolution.renderWidth) *
        (static_cast<uint64_t>(profileReport_.resolution.renderHeight) / 2ull);
    auto gridAverage = [giGridFixedScale](uint64_t sumFixed, uint64_t count) -> double {
        return count == 0ull ? 0.0 : static_cast<double>(sumFixed) / (static_cast<double>(count) * giGridFixedScale);
    };
    auto gridScore = [](double a, double b) -> nlohmann::json {
        const double mean = std::max((std::abs(a) + std::abs(b)) * 0.5, 1.0e-6);
        return nlohmann::json(std::abs(a - b) / mean);
    };
    const double giGridXEven = gridAverage(giCounter(28), giXEvenCount);
    const double giGridXOdd = gridAverage(giCounter(29), giXOddCount);
    const double giGridYEven = gridAverage(giCounter(30), giYEvenCount);
    const double giGridYOdd = gridAverage(giCounter(31), giYOddCount);
    const bool giGridAvailable = giFinalPixels > 0ull && (giCounter(28) + giCounter(29) + giCounter(30) + giCounter(31)) > 0ull;
    const nlohmann::json giGridHorizontalScore = giGridAvailable ? gridScore(giGridXEven, giGridXOdd) : nlohmann::json(nullptr);
    const nlohmann::json giGridVerticalScore = giGridAvailable ? gridScore(giGridYEven, giGridYOdd) : nlohmann::json(nullptr);
    nlohmann::json giGridOverallScore = nullptr;
    if (giGridAvailable && giGridHorizontalScore.is_number() && giGridVerticalScore.is_number()) {
        giGridOverallScore = std::max(giGridHorizontalScore.get<double>(), giGridVerticalScore.get<double>());
    }
    const uint64_t giPathTemporalCurrentOnly = giCounter(34);
    const uint64_t giPathFinalCurrentOnly = giCounter(41);
    const uint64_t giPathFinalClassified =
        giCounter(38) + giCounter(39) + giCounter(40) + giCounter(41);
    const uint64_t giActiveTileCount = giCounter(46);
    const uint64_t giActivePixelCount = giCounter(47);
    const uint64_t giTileColumns = (static_cast<uint64_t>(profileReport_.resolution.renderWidth) + 15ull) / 16ull;
    const uint64_t giTileRows = (static_cast<uint64_t>(profileReport_.resolution.renderHeight) + 15ull) / 16ull;
    const uint64_t giTileCount = giTileColumns * giTileRows;
    const uint64_t giReusePixels =
        (profileReport_.settings.restirGiHalfResolution && giPixels > 0ull)
            ? ((static_cast<uint64_t>(profileReport_.resolution.renderWidth) + 1ull) / 2ull) *
                ((static_cast<uint64_t>(profileReport_.resolution.renderHeight) + 1ull) / 2ull)
            : giPixels;
    const uint64_t giProductionStride = giReusePixels > 0ull && profileReport_.memory.restirGiProductionTemporalBytes > 0ull
        ? profileReport_.memory.restirGiProductionTemporalBytes / giReusePixels
        : 0ull;
    const uint64_t giReceiverStride = giPixels > 0ull && profileReport_.memory.restirGiReceiverBytes > 0ull
        ? profileReport_.memory.restirGiReceiverBytes / giPixels
        : 0ull;
    const uint64_t giTotalBytes =
        profileReport_.memory.restirGiCurrentBytes +
        profileReport_.memory.restirGiPreviousBytes +
        profileReport_.memory.restirGiSpatialBytes +
        profileReport_.memory.restirGiProductionTemporalBytes +
        profileReport_.memory.restirGiProductionSpatialBytes +
        profileReport_.memory.restirGiProductionPreviousBytes +
        profileReport_.memory.restirGiProductionUpsampledBytes +
        profileReport_.memory.restirGiActiveTileMaskBytes +
        profileReport_.memory.restirGiReceiverBytes +
        profileReport_.memory.restirGiPreviousReceiverBytes +
        profileReport_.memory.restirGiCountersBytes;
    const uint64_t giAliasSavings =
        (profileReport_.settings.restirGiMode == RestirGiMode::Production ||
         profileReport_.settings.restirGiMode == RestirGiMode::ReferenceValidation)
        ? profileReport_.memory.restirGiPreviousBytes + profileReport_.memory.restirGiSpatialBytes
        : 0ull;
    const uint64_t giPhysicalBytes = giTotalBytes >= giAliasSavings ? giTotalBytes - giAliasSavings : giTotalBytes;
    nlohmann::json giWarnings = nlohmann::json::array();
    const bool giProductionPassesActive = !profileReport_.settings.wavefrontFinalOutputEnabled &&
        (profileReport_.settings.restirGiMode == RestirGiMode::Production ||
         profileReport_.settings.restirGiMode == RestirGiMode::ReferenceValidation);
    if (giProductionPassesActive &&
        profileReport_.restirGiCounters.empty()) {
        giWarnings.push_back({
            {"code", "RESTIR_GI_COUNTERS_UNAVAILABLE"},
            {"severity", "warning"},
            {"pass", "restir_gi_counters_readback"},
            {"count", 1},
        });
    }
    j["restir_gi"] = {
        {"schema_version", 2},
        {"mode", restirGiModeName(profileReport_.settings.restirGiMode)},
        {"layout", profileReport_.restirGiLayout},
        {"effective_production_reuse", giProductionPassesActive},
        {"wavefront_current_frame_fallback", profileReport_.settings.wavefrontFinalOutputEnabled},
        {"counter_scope", "last_completed_frame"},
        {"counter_value_bits", 32},
        {"history_valid", giTemporalPreviousValid > 0ull || giTemporalAccepted > 0ull},
        {"half_resolution", profileReport_.settings.restirGiHalfResolution},
        {"render_extent", {
            {"width", profileReport_.resolution.renderWidth},
            {"height", profileReport_.resolution.renderHeight},
        }},
        {"reuse_extent", {
            {"width", profileReport_.settings.restirGiHalfResolution
                ? (profileReport_.resolution.renderWidth + 1u) / 2u
                : profileReport_.resolution.renderWidth},
            {"height", profileReport_.settings.restirGiHalfResolution
                ? (profileReport_.resolution.renderHeight + 1u) / 2u
                : profileReport_.resolution.renderHeight},
        }},
        {"active_passes", {
            {"temporal", giProductionPassesActive},
            {"spatial", profileReport_.settings.restirGiSpatialRounds > 0u &&
                giProductionPassesActive},
            {"upsample", profileReport_.settings.restirGiHalfResolution &&
                giProductionPassesActive},
            {"final", giProductionPassesActive},
        }},
        {"temporal_pixels", giTemporalPixels},
        {"temporal_current_valid_count", giTemporalCurrentValid},
        {"temporal_previous_valid_count", giTemporalPreviousValid},
        {"temporal_accepted_count", giTemporalAccepted},
        {"temporal_rejected_count", giTemporalRejected},
        {"temporal_reject_camera_cut_count", giCounter(4)},
        {"temporal_reject_offscreen_count", giCounter(5)},
        {"temporal_reject_surface_count", giCounter(6)},
        {"temporal_reject_age_count", giCounter(7)},
        {"temporal_reject_target_count", giCounter(8)},
        {"temporal_reject_velocity_count", giCounter(9)},
        {"temporal_acceptance_ratio", ratioOrNull(giTemporalAccepted, giTemporalPreviousValid)},
        {"active_tile_mask_mode", restirGiActiveTileMaskModeName(profileReport_.settings.restirGiActiveTileMaskMode)},
        {"active_tile_count", giActiveTileCount},
        {"active_tile_total_count", giTileCount},
        {"active_tile_coverage", ratioOrNull(giActiveTileCount, giTileCount)},
        {"active_pixel_count", giActivePixelCount},
        {"active_pixel_coverage", ratioOrNull(giActivePixelCount, giReusePixels)},
        {"spatial_pixels", giSpatialPixels},
        {"spatial_tested_count", giSpatialTested},
        {"spatial_accepted_count", giSpatialAccepted},
        {"spatial_rejected_count", giSpatialTested > giSpatialAccepted ? giSpatialTested - giSpatialAccepted : 0ull},
        {"spatial_reject_invalid_count", giCounter(13)},
        {"spatial_reject_surface_count", giCounter(14)},
        {"spatial_reject_target_count", giCounter(15)},
        {"spatial_selected_neighbor_count", giCounter(16)},
        {"spatial_acceptance_ratio", ratioOrNull(giSpatialAccepted, giSpatialTested)},
        {"upsample_pixels", giUpsamplePixels},
        {"upsample_accepted_count", giUpsampleAccepted},
        {"upsample_fallback_count", giUpsampleFallback},
        {"upsample_fallback_ratio", ratioOrNull(giUpsampleFallback, giUpsamplePixels)},
        {"final_pixels", giFinalPixels},
        {"final_spatial_used_count", giCounter(18)},
        {"final_current_used_count", giCounter(19)},
        {"final_fallback_count", giFinalFallback},
        {"final_stabilized_count", giCounter(25)},
        {"final_fallback_ratio", ratioOrNull(giFinalFallback, giFinalPixels)},
        {"clamp_count", giClampCount},
        {"clamp_ratio", ratioOrNull(giClampCount, giFinalPixels)},
        {"visibility_ray_count", giCounter(42)},
        {"visibility_unknown_count", giCounter(43)},
        {"version_reject_count", giCounter(44)},
        {"non_finite_reject_count", giCounter(45)},
        {"half_res_grid_score", giGridOverallScore},
        {"grid_score", giGridOverallScore},
        {"grid", {
            {"schema_version", 1},
            {"metric", "even_odd_final_gi_perceptual_luma_periodicity"},
            {"counter_encoding", "sqrt_luma_clamped_0_1_fixed_9"},
            {"available", giGridAvailable},
            {"horizontal_score", giGridHorizontalScore},
            {"vertical_score", giGridVerticalScore},
            {"score", giGridOverallScore},
            {"x_even_average_luma", giGridAvailable ? nlohmann::json(giGridXEven) : nlohmann::json(nullptr)},
            {"x_odd_average_luma", giGridAvailable ? nlohmann::json(giGridXOdd) : nlohmann::json(nullptr)},
            {"y_even_average_luma", giGridAvailable ? nlohmann::json(giGridYEven) : nlohmann::json(nullptr)},
            {"y_odd_average_luma", giGridAvailable ? nlohmann::json(giGridYOdd) : nlohmann::json(nullptr)},
        }},
        {"path_classes", {
            {"schema_version", 2},
            {"policy", "diffuse_glossy_environment_emissive_reusable_unsafe_closures_current_only"},
            {"debug_view", "restir-gi-path-class"},
            {"temporal_diffuse_reusable_count", giCounter(32)},
            {"temporal_glossy_reusable_count", giCounter(33)},
            {"temporal_unsupported_current_only_count", giCounter(34)},
            {"temporal_current_only_count", giPathTemporalCurrentOnly},
            {"temporal_current_only_reuse_reject_count", giCounter(35)},
            {"spatial_path_class_reject_count", giCounter(36)},
            {"spatial_center_current_only_count", giCounter(37)},
            {"final_diffuse_reusable_count", giCounter(38)},
            {"final_glossy_reusable_count", giCounter(39)},
            {"final_environment_or_emissive_reusable_count", giCounter(40)},
            {"final_current_only_count", giPathFinalCurrentOnly},
            {"final_unknown_or_invalid_count", giCounter(41)},
            {"final_classified_count", giPathFinalClassified},
            {"final_current_only_ratio", ratioOrNull(giPathFinalCurrentOnly, giPathFinalClassified)},
        }},
        {"memory", {
            {"production_reservoir_stride", giProductionStride},
            {"receiver_stride", giReceiverStride},
            {"legacy_current_bytes", profileReport_.memory.restirGiCurrentBytes},
            {"legacy_previous_bytes", profileReport_.memory.restirGiPreviousBytes},
            {"legacy_spatial_bytes", profileReport_.memory.restirGiSpatialBytes},
            {"production_temporal_bytes", profileReport_.memory.restirGiProductionTemporalBytes},
            {"production_spatial_bytes", profileReport_.memory.restirGiProductionSpatialBytes},
            {"production_previous_bytes", profileReport_.memory.restirGiProductionPreviousBytes},
            {"production_upsampled_bytes", profileReport_.memory.restirGiProductionUpsampledBytes},
            {"active_tile_mask_bytes", profileReport_.memory.restirGiActiveTileMaskBytes},
            {"receiver_bytes", profileReport_.memory.restirGiReceiverBytes},
            {"previous_receiver_bytes", profileReport_.memory.restirGiPreviousReceiverBytes},
            {"counters_bytes", profileReport_.memory.restirGiCountersBytes},
            {"logical_bytes", giTotalBytes},
            {"physical_bytes", giPhysicalBytes},
            {"alias_savings_bytes", giAliasSavings},
            {"total_bytes", giPhysicalBytes},
        }},
        {"warnings", std::move(giWarnings)},
    };
    j["adaptive_quality"] = profileReport_.adaptiveQuality;
    j["memory_pressure_quality"] = profileReport_.memoryPressureQuality;
    j["nvidia_integrations"] = profileReport_.nvidiaIntegrations;
    j["scene_update_routes"] = profileReport_.sceneUpdateRoutes;
    j["scheduler_queues"] = profileReport_.schedulerQueues;
    j["job_tickets"] = {
        {"schema", "JobTicketsDiagnosticsV1"},
        {"execution_policy", "diagnostic ticket snapshots; live Vulkan upload/topology ownership remains reported by scheduler/resource-readiness fields"},
        {"gpu_upload", {
            {"next_timeline_value", profileReport_.gpuUploadNextTimelineValue},
            {"tickets", profileReport_.gpuUploadTickets},
        }},
        {"main_thread_apply", {
            {"tickets", profileReport_.mainThreadApplyTickets},
        }},
        {"topology_rebuild", {
            {"latest_generation", profileReport_.topologyRebuildLatestGeneration},
            {"next_timeline_value", profileReport_.topologyRebuildNextTimelineValue},
            {"tickets", profileReport_.topologyRebuildTickets},
        }},
    };
    j["validation_error_count"] = profileReport_.validationErrorCount;
    j["warnings"] = profileReport_.warnings;
    j["settings"] = profileReport_.settings;
    j["settings"]["restir_gi_active_tile_mask_enabled"] =
        profileReport_.effectiveRestirGiActiveTileMaskEnabled;
    j["settings"]["restir_history_copy_mode_effective"] =
        restirHistoryCopyModeName(profileReport_.effectiveRestirHistoryCopyMode);
    j["settings"]["restir_history_copy_mode_fallback_reason"] =
        profileReport_.restirHistoryCopyFallbackReason.empty()
        ? nlohmann::json(nullptr)
        : nlohmann::json(profileReport_.restirHistoryCopyFallbackReason);
    const auto dir = path.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) { std::filesystem::create_directories(dir); }
    std::ofstream file(path);
    if (!file.is_open()) { throw std::runtime_error("Failed to open profile JSON: " + path.string()); }
    file << j.dump(2);
}

void HeadlessDiagnostics::writeRenderGraphJson(const std::filesystem::path& path) {
    const auto dir = path.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) { std::filesystem::create_directories(dir); }
    GpuFrameTimings t{};
    RenderGraph g;
    dumpRenderGraphJson(g, t, path);
}

void HeadlessDiagnostics::exportDebugViews(Application& app, const std::filesystem::path& dir) {
    if (!config_.saveDebugViewsDir.has_value()) return;

    auto* renderer = app.pathTracer();
    auto* allocator = app.resourceAllocator();
    auto* context = app.vulkanContext();
    if (!renderer || !allocator || !context) return;

    std::filesystem::create_directories(dir);

    DiagnosticImageExport exporter(*context, *allocator);
    VkExtent2D displayExtent = renderer->displayExtent();
    if (!exporter.initialize(VK_FORMAT_R8G8B8A8_UNORM, displayExtent)) {
        std::cerr << "Warning: Failed to initialize image export\n";
        return;
    }

    const auto views = DiagnosticImageExport::allExportViews();
    const RendererSettings exportSettings = renderer->settings();
    std::vector<std::string> exported;
    std::vector<std::string> skipped;
    const uint32_t kWarmupFrames = config_.warmupFrames > 0 ? config_.warmupFrames : 4;

    for (auto view : views) {
        std::string viewName = rendererDebugViewName(view);
        auto outputPath = dir / (viewName + ".png");
        if (!shouldExportDebugViewForSettings(exportSettings, view)) {
            std::cout << "Skipping debug view: " << viewName << " (disabled feature)\n";
            skipped.push_back(viewName);
            continue;
        }
        std::cout << "Exporting debug view: " << viewName << "...\n";

        app.applyDebugView(view);
        app.renderFrames(kWarmupFrames + 1);

        if (exporter.exportView(*renderer, view, outputPath, 0)) {
            exported.push_back(viewName);
        } else {
            std::cerr << "Warning: Failed to export debug view: " << viewName << "\n";
        }
    }

    std::vector<std::string> missing;
    if (std::find(exported.begin(), exported.end(), "metallic") == exported.end()) {
        missing.push_back("metallic");
    }

    exporter.writeExportManifest(dir, exported, displayExtent.width, displayExtent.height);
    if (!missing.empty() || !skipped.empty()) {
        auto manifestPath = dir / "export_manifest.json";
        if (std::filesystem::exists(manifestPath)) {
            std::ifstream in(manifestPath);
            nlohmann::json manifest;
            in >> manifest;
            if (!missing.empty()) {
                manifest["missing_debug_views"] = missing;
            }
            if (!skipped.empty()) {
                manifest["skipped_debug_views"] = skipped;
            }
            std::ofstream out(manifestPath);
            out << manifest.dump(2);
        }
    }
    std::cout << "Exported " << exported.size() << " debug views to " << dir.string() << "\n";
    if (writeOpacityMicromapDebugImages(renderer->scene().opacityMicromapData(), dir / "opacity_micromap")) {
        std::cout << "Exported opacity micromap CPU debug atlas to "
                  << (dir / "opacity_micromap").string() << "\n";
    }
}

void HeadlessDiagnostics::exportFrameSequence(Application& app, const std::filesystem::path& dir) {
    if (!config_.saveFrameSequenceDir.has_value()) return;

    auto* renderer = app.pathTracer();
    auto* allocator = app.resourceAllocator();
    auto* context = app.vulkanContext();
    if (!renderer || !allocator || !context) return;

    std::filesystem::create_directories(dir);

    DiagnosticImageExport exporter(*context, *allocator);
    VkExtent2D displayExtent = renderer->displayExtent();
    if (!exporter.initialize(VK_FORMAT_R8G8B8A8_UNORM, displayExtent)) {
        std::cerr << "Warning: Failed to initialize frame sequence export\n";
        return;
    }

    std::vector<RendererDebugView> views = config_.sequenceViews;
    if (views.empty()) {
        views.push_back(RendererDebugView::Beauty);
    }

    const uint32_t warmupFrames = config_.warmupFrames;
    const uint32_t step = std::max(1u, config_.sequenceStep);
    const uint32_t profiledFrames = config_.totalFrames > warmupFrames
        ? config_.totalFrames - warmupFrames
        : config_.totalFrames;
    const uint32_t framesToExport = config_.sequenceFrameCount.value_or(std::max(1u, profiledFrames));

    nlohmann::json manifest;
    manifest["views"] = nlohmann::json::array();
    manifest["warmup_frames"] = warmupFrames;
    manifest["sequence_start_frame"] = config_.sequenceStartFrame;
    manifest["sequence_frame_count"] = framesToExport;
    manifest["sequence_step"] = step;
    manifest["resolution"] = { {"width", displayExtent.width}, {"height", displayExtent.height} };
    manifest["frames"] = nlohmann::json::object();

    for (RendererDebugView view : views) {
        const std::string viewName = rendererDebugViewName(view);
        const std::filesystem::path viewDir = dir / viewName;
        std::filesystem::create_directories(viewDir);
        manifest["views"].push_back(viewName);
        manifest["frames"][viewName] = nlohmann::json::array();

        std::cout << "Exporting frame sequence view: " << viewName << "...\n";
        app.applyDebugView(view);
        app.resetDiagnosticFrameCounter(0);
        if (warmupFrames > 0u) {
            app.renderFrames(warmupFrames);
        }
        if (config_.sequenceStartFrame > 0u) {
            app.renderFrames(config_.sequenceStartFrame);
        }

        uint32_t sequenceFrame = config_.sequenceStartFrame;
        for (uint32_t exported = 0; exported < framesToExport; ++exported) {
            app.renderFrames(1);
            const std::string fileName = sequenceFrameFileName(sequenceFrame);
            const std::filesystem::path outputPath = viewDir / fileName;
            if (exporter.exportView(*renderer, view, outputPath, 0)) {
                manifest["frames"][viewName].push_back(fileName);
            } else {
                std::cerr << "Warning: Failed to export sequence frame " << fileName
                          << " for view " << viewName << "\n";
            }
            if (step > 1u && exported + 1u < framesToExport) {
                app.renderFrames(step - 1u);
            }
            sequenceFrame += step;
        }
    }

    std::ofstream file(dir / "sequence_manifest.json");
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open sequence manifest: " + (dir / "sequence_manifest.json").string());
    }
    file << manifest.dump(2);
    std::cout << "Exported frame sequence to " << dir.string() << "\n";
}

void HeadlessDiagnostics::makeDebugPackage(Application& app, const std::filesystem::path& dir, const std::filesystem::path& scenePath) {
    if (!std::filesystem::exists(dir)) { std::filesystem::create_directories(dir); }

    auto copyIf = [](const auto& src, const auto& dest) {
        if (src.has_value() && std::filesystem::exists(*src)) {
            std::filesystem::copy_file(*src, dest, std::filesystem::copy_options::overwrite_existing);
        }
    };
    copyIf(config_.profileJsonPath, dir / "profile.json");
    copyIf(config_.dumpRenderGraphPath, dir / "rendergraph.json");

    if (config_.saveDebugViewsDir.has_value() && std::filesystem::exists(*config_.saveDebugViewsDir)) {
        try {
            std::filesystem::copy(*config_.saveDebugViewsDir, dir / "debug_views",
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        } catch (...) {}
    }

    if (config_.saveFrameSequenceDir.has_value() && std::filesystem::exists(*config_.saveFrameSequenceDir)) {
        try {
            std::filesystem::copy(*config_.saveFrameSequenceDir, dir / "frame_sequence",
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        } catch (...) {}
    }

    if (!scenePath.empty() && std::filesystem::exists(scenePath)) {
        std::filesystem::copy_file(scenePath, dir / "scene_copy.rtlevel",
            std::filesystem::copy_options::overwrite_existing);
    }

    if (auto* renderer = app.pathTracer()) {
        writeValidationLog(renderer->validationLog(), dir / "validation.txt");
        writeSettingsJson(renderer->settings(), dir / "settings.json");
    }

    if (logCapture_) {
        std::string logText = releaseStdout();
        std::ofstream logFile(dir / "log.txt");
        if (logFile.is_open()) { logFile << logText; }
    }

    if (config_.captureRenderDocPath.has_value() && std::filesystem::exists(*config_.captureRenderDocPath)) {
        std::filesystem::copy_file(*config_.captureRenderDocPath, dir / "capture.rdc",
            std::filesystem::copy_options::overwrite_existing);
    }
}

ValidationSuiteSummary HeadlessDiagnostics::runValidationSuite() {
    ValidationSuiteSummary summary;

    struct SceneConfig {
        std::string name;
        std::filesystem::path path;
        uint32_t frames;
        uint32_t warmup;
    };

    std::vector<SceneConfig> scenes;
    const std::filesystem::path validationDir = "scenes/validation";
    const std::filesystem::path manifestPath = validationDir / "manifest.json";
    if (std::filesystem::exists(manifestPath)) {
        std::ifstream manifestFile(manifestPath);
        nlohmann::json manifest;
        manifestFile >> manifest;
        for (const auto& scene : manifest.value("scenes", nlohmann::json::array())) {
            const std::string fileName = scene.value("path", "");
            if (fileName.empty()) {
                continue;
            }
            const std::filesystem::path scenePath = validationDir / fileName;
            scenes.push_back(SceneConfig{
                .name = sanitizeSceneName(scenePath.stem().string()),
                .path = scenePath,
                .frames = 120,
                .warmup = 30,
            });
        }
    }
    if (scenes.empty()) {
        scenes = {
            {"material_grid", validationDir / "material_grid.rtlevel", 120, 30},
            {"transform_stress", validationDir / "transform_stress.rtlevel", 120, 30},
        };
    }

    std::filesystem::path outputBase = config_.validationOutputDir.value_or("validation_output");
    std::filesystem::create_directories(outputBase);

    for (const auto& scene : scenes) {
        ValidationSceneResult result;
        result.name = scene.name;
        result.framesRendered = scene.frames;

        auto sceneOutDir = outputBase / scene.name;
        try {
            std::filesystem::create_directories(sceneOutDir);
            if (!std::filesystem::exists(scene.path)) {
                throw std::runtime_error("Missing validation scene: " + scene.path.string());
            }

            HeadlessDiagnosticsConfig sceneConfig = config_;
            sceneConfig.headless = true;
            sceneConfig.profile = true;
            sceneConfig.runValidationSuite = false;
            sceneConfig.totalFrames = config_.wavefrontValidationMode ? config_.totalFrames : scene.frames;
            sceneConfig.warmupFrames = config_.wavefrontValidationMode && config_.warmupFrames > 0u
                ? config_.warmupFrames
                : scene.warmup;
            sceneConfig.fixedSeed = sceneConfig.fixedSeed.value_or(1u);
            sceneConfig.profileJsonPath = sceneOutDir / "profile.json";
            sceneConfig.dumpRenderGraphPath = sceneOutDir / "rendergraph.json";
            if (!sceneConfig.wavefrontValidationMode) {
                sceneConfig.saveDebugViewsDir = sceneOutDir / "debug_views";
            } else {
                sceneConfig.saveDebugViewsDir.reset();
            }
            sceneConfig.makeDebugPackageDir.reset();

            HeadlessDiagnostics sceneDiagnostics(sceneConfig);
            sceneDiagnostics.captureStdout();
            Application app(
                RendererDebugView::Beauty,
                std::nullopt,
                std::nullopt,
                scene.path,
                std::nullopt,
                {},
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                false,
                false,
                false,
                true,
                sceneConfig.disableAsyncCompute,
                sceneConfig.singleQueueFallback,
                sceneConfig.disableResourceAliasing);
            if (auto* renderer = app.pathTracer()) {
                RendererSettings settings = renderer->settings();
                settings.fixedSeed = sceneConfig.fixedSeed;
                if (sceneConfig.wavefrontValidationMode) {
                    settings.debugView = RendererDebugView::Beauty;
                    settings.restirMode = RestirMode::ClassicNee;
                    settings.restirGiMode = RestirGiMode::Off;
                    settings.restirGiEnabled = false;
                    settings.wavefrontQueuesEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontShadowTraceEnabled = true;
                    settings.wavefrontCompactEnabled = true;
                    settings.renderPreset = RenderPreset::Custom;
                }
                renderer->applySettings(settings);
                renderer->setDumpRenderGraphPath(sceneConfig.dumpRenderGraphPath);
            }

            app.runHeadless(sceneConfig.warmupFrames, sceneConfig.totalFrames);
            const ProfileReport profile = sceneDiagnostics.run(app);
            sceneDiagnostics.writeProfileJson(*sceneConfig.profileJsonPath);
            if (sceneConfig.saveDebugViewsDir.has_value()) {
                sceneDiagnostics.exportDebugViews(app, *sceneConfig.saveDebugViewsDir);
            }
            if (auto* renderer = app.pathTracer()) {
                writeValidationLog(renderer->validationLog(), sceneOutDir / "validation.txt");
                writeSettingsJson(renderer->settings(), sceneOutDir / "settings.json");
            }
            std::filesystem::copy_file(scene.path, sceneOutDir / "scene_copy.rtlevel",
                std::filesystem::copy_options::overwrite_existing);

            std::ofstream logFile(sceneOutDir / "log.txt");
            if (logFile.is_open()) {
                logFile << sceneDiagnostics.releaseStdout();
            } else {
                (void)sceneDiagnostics.releaseStdout();
            }

            result.gpuMsTotal = profile.gpuFrameMs.avg;
            result.validationErrors = profile.validationErrorCount;
            result.framesRendered = sceneConfig.totalFrames;
            result.wavefrontValidationEnabled = profile.wavefrontValidation.enabled;
            result.wavefrontValidationPassed = profile.wavefrontValidation.allRequiredPassed;
            result.wavefrontCheckedPixels = profile.wavefrontValidation.checkedPixels;
            result.wavefrontCheckedSecondaryRays = profile.wavefrontValidation.checkedSecondaryRays;
            result.wavefrontCheckedShadowRays = profile.wavefrontValidation.checkedShadowRays;
            result.wavefrontDirectLightingMismatches = profile.wavefrontValidation.directLightingMismatchCount;
            result.wavefrontProbeGpuMs = profile.wavefrontValidation.wavefrontProbeGpuMs;
            result.status = result.validationErrors == 0 &&
                (!result.wavefrontValidationEnabled || result.wavefrontValidationPassed)
                ? "pass"
                : "fail";
            summary.scenes.push_back(result);
            if (result.status == "pass") {
                summary.totalPass++;
            } else {
                summary.totalFail++;
            }
            if (result.wavefrontValidationEnabled) {
                if (result.wavefrontValidationPassed) {
                    summary.wavefrontValidationPass++;
                } else {
                    summary.wavefrontValidationFail++;
                }
            }
        } catch (const std::exception& error) {
            result.status = "fail";
            std::ofstream logFile(sceneOutDir / "log.txt", std::ios::app);
            if (logFile.is_open()) {
                logFile << "Fatal error: " << error.what() << "\n";
            }
            summary.scenes.push_back(result);
            summary.totalFail++;
        } catch (...) {
            result.status = "fail";
            summary.scenes.push_back(result);
            summary.totalFail++;
        }
    }

    auto summaryPath = outputBase / "summary.json";
    nlohmann::json j;
    nlohmann::json scenesJson = nlohmann::json::array();
    for (const auto& s : summary.scenes) {
        nlohmann::json sj;
        sj["name"] = s.name;
        sj["status"] = s.status;
        sj["gpu_ms_total"] = s.gpuMsTotal;
        sj["validation_errors"] = s.validationErrors;
        sj["frames_rendered"] = s.framesRendered;
        if (s.wavefrontValidationEnabled) {
            sj["wavefront_validation"] = {
                {"enabled", s.wavefrontValidationEnabled},
                {"passed", s.wavefrontValidationPassed},
                {"checked_pixels", s.wavefrontCheckedPixels},
                {"checked_secondary_rays", s.wavefrontCheckedSecondaryRays},
                {"checked_shadow_rays", s.wavefrontCheckedShadowRays},
                {"direct_lighting_mismatch_count", s.wavefrontDirectLightingMismatches},
                {"wavefront_probe_gpu_ms", s.wavefrontProbeGpuMs},
            };
        }
        scenesJson.push_back(sj);
    }
    j["scenes"] = scenesJson;
    j["total_pass"] = summary.totalPass;
    j["total_fail"] = summary.totalFail;
    j["wavefront_validation_pass"] = summary.wavefrontValidationPass;
    j["wavefront_validation_fail"] = summary.wavefrontValidationFail;
    std::ofstream file(summaryPath);
    if (file.is_open()) { file << j.dump(2); }

    return summary;
}

void HeadlessDiagnostics::captureStdout() {
    logCapture_ = std::make_unique<std::ostringstream>();
    oldCout_ = std::cout.rdbuf(logCapture_->rdbuf());
    oldCerr_ = std::cerr.rdbuf(logCapture_->rdbuf());
}

std::string HeadlessDiagnostics::releaseStdout() {
    const std::string text = logCapture_ ? logCapture_->str() : std::string{};
    if (oldCout_ != nullptr) {
        std::cout.rdbuf(oldCout_);
        oldCout_ = nullptr;
    }
    if (oldCerr_ != nullptr) {
        std::cerr.rdbuf(oldCerr_);
        oldCerr_ = nullptr;
    }
    logCapture_.reset();
    return text;
}

void HeadlessDiagnostics::collectValidationLog(Application& app) {
    auto* renderer = app.pathTracer();
    if (renderer == nullptr) return;
    const auto& log = renderer->validationLog();
    (void)log;
}

} // namespace rtv
