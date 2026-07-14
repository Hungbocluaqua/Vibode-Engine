#pragma once

#include "rtv/DescriptorWriter.h"
#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

#include <Volk/volk.h>

#include <algorithm>
#include <cstdint>

namespace rtv::passes {

struct RestirGIPass {
    static constexpr const char* kContractId = "restir_gi";
    static constexpr const char* kPassName = "RestirGIPass";
    static constexpr const char* kRole = "lighting_reuse";
    static constexpr const char* kExtractionState =
        "contract-module plus mode/runtime resource-sizing/parameter/stage-readiness/counter/history-slot/history-selection policy; scheduling and GPU resources remain coordinated by PathTracerRenderer";

    enum class HistorySlot {
        Primary,
        Secondary,
    };

    enum class ProductionHistorySource {
        SpatialHistory,
        TemporalReservoir,
    };

    static constexpr uint32_t kActiveTileSize = 16u;

    static uint32_t reuseWidth(uint32_t renderWidth, bool halfResolution) {
        return halfResolution ? (renderWidth + 1u) / 2u : renderWidth;
    }

    static uint32_t reuseHeight(uint32_t renderHeight, bool halfResolution) {
        return halfResolution ? (renderHeight + 1u) / 2u : renderHeight;
    }

    static VkDeviceSize pixelByteSize(uint64_t pixelCount, VkDeviceSize strideBytes) {
        return static_cast<VkDeviceSize>(pixelCount) * strideBytes;
    }

    static VkDeviceSize productionReservoirByteSize(
        uint32_t renderWidth,
        uint32_t renderHeight,
        bool halfResolution,
        VkDeviceSize reservoirStrideBytes) {
        return static_cast<VkDeviceSize>(reuseWidth(renderWidth, halfResolution)) *
            static_cast<VkDeviceSize>(reuseHeight(renderHeight, halfResolution)) *
            reservoirStrideBytes;
    }

    static VkDeviceSize activeTileMaskByteSize(uint32_t renderWidth, uint32_t renderHeight) {
        const uint32_t columns = (renderWidth + kActiveTileSize - 1u) / kActiveTileSize;
        const uint32_t rows = (renderHeight + kActiveTileSize - 1u) / kActiveTileSize;
        return std::max<VkDeviceSize>(1u, static_cast<VkDeviceSize>(columns) * rows) * sizeof(uint32_t);
    }

    struct SpatialParameterInputs {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameIndex = 0;
        bool legacySpatialReuseRuns = false;
        bool giReservoirsRun = false;
        bool giHalfResolution = false;
        bool rawOutputIsCurrentSample = false;
    };

    template <typename Params, typename CameraPosition>
    static Params makeSpatialParams(
        const RendererSettings& settings,
        const SpatialParameterInputs& inputs,
        const CameraPosition& cameraPosition) {
        Params params{};
        params.width = inputs.width;
        params.height = inputs.height;
        params.frameCount = inputs.frameIndex;
        const uint32_t restirReuseEnabled = (inputs.legacySpatialReuseRuns || inputs.giReservoirsRun) ? 1u : 0u;
        params.enabled = restirReuseEnabled |
            ((inputs.giReservoirsRun && settings.restirGiFinalStabilizationEnabled) ? 2u : 0u);
        params.giSpatialRounds = settings.restirGiSpatialRounds;
        params.giHalfResolution = inputs.giHalfResolution ? 1u : 0u;
        params.giTemporalMaxAge = settings.restirGiTemporalMaxAge;
        params.giVisibilityRayBudget = settings.restirGiVisibilityRayBudget;
        params.giSpatialRadius = settings.restirGiSpatialRadius;
        params.giDepthThresholdScale = settings.restirGiDepthThresholdScale;
        params.giSpatialCompatibilityThreshold = settings.restirGiSpatialCompatibilityThreshold;
        params.rawOutputIsCurrentSample = inputs.rawOutputIsCurrentSample ? 1.0f : 0.0f;
        params.cameraPosition = cameraPosition;
        return params;
    }

    static constexpr HistorySlot currentHistorySlot(uint32_t temporalFrameIndex) {
        return (temporalFrameIndex & 1u) == 0u ? HistorySlot::Primary : HistorySlot::Secondary;
    }

    static constexpr HistorySlot previousHistorySlot(uint32_t temporalFrameIndex) {
        return currentHistorySlot(temporalFrameIndex) == HistorySlot::Primary
            ? HistorySlot::Secondary
            : HistorySlot::Primary;
    }

    template <typename Resource>
    static const Resource& selectCurrentHistoryResource(
        bool pingPongHistory,
        uint32_t temporalFrameIndex,
        const Resource& primary,
        const Resource& secondary) {
        if (!pingPongHistory) {
            return primary;
        }
        return currentHistorySlot(temporalFrameIndex) == HistorySlot::Primary
            ? primary
            : secondary;
    }

    template <typename Resource>
    static const Resource& selectPreviousHistoryResource(
        bool pingPongHistory,
        uint32_t temporalFrameIndex,
        const Resource& primary,
        const Resource& secondary) {
        if (!pingPongHistory) {
            return secondary;
        }
        return previousHistorySlot(temporalFrameIndex) == HistorySlot::Primary
            ? primary
            : secondary;
    }

    static constexpr ProductionHistorySource productionHistorySource(bool spatialStageRuns) {
        return spatialStageRuns
            ? ProductionHistorySource::SpatialHistory
            : ProductionHistorySource::TemporalReservoir;
    }

    static constexpr bool usesTemporalReservoirAsProductionHistorySource(bool spatialStageRuns) {
        return productionHistorySource(spatialStageRuns) == ProductionHistorySource::TemporalReservoir;
    }

    template <typename Resource>
    static const Resource& selectProductionHistorySource(
        bool spatialStageRuns,
        const Resource& spatialHistory,
        const Resource& temporalReservoir) {
        return productionHistorySource(spatialStageRuns) == ProductionHistorySource::SpatialHistory
            ? spatialHistory
            : temporalReservoir;
    }

    static bool isActive(const RendererSettings& settings) {
        return settings.restirGiEnabled && settings.restirGiMode != RestirGiMode::Off;
    }

    static bool isLegacyCacheMode(const RendererSettings& settings) {
        return settings.restirGiMode == RestirGiMode::LegacyCache;
    }

    static bool isProductionMode(const RendererSettings& settings) {
        return settings.restirGiMode == RestirGiMode::Production;
    }

    static bool isReferenceValidationMode(const RendererSettings& settings) {
        return settings.restirGiMode == RestirGiMode::ReferenceValidation;
    }

    static bool isNewReservoirMode(const RendererSettings& settings) {
        return isProductionMode(settings) || isReferenceValidationMode(settings);
    }

    static constexpr bool usesSpatialDebugView(RendererDebugView view) {
        return view == RendererDebugView::RestirGiSpatial ||
            view == RendererDebugView::RestirGiGrid ||
            view == RendererDebugView::RestirGiPathClass ||
            view == RendererDebugView::RestirGiTarget ||
            view == RendererDebugView::RestirGiSourcePdf ||
            view == RendererDebugView::RestirGiWeightSum ||
            view == RendererDebugView::RestirGiM ||
            view == RendererDebugView::RestirGiConfidence ||
            view == RendererDebugView::RestirGiVisibility;
    }

    static constexpr bool usesDebugView(RendererDebugView view) {
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
            view == RendererDebugView::RestirGiTarget ||
            view == RendererDebugView::RestirGiSourcePdf ||
            view == RendererDebugView::RestirGiWeightSum ||
            view == RendererDebugView::RestirGiM ||
            view == RendererDebugView::RestirGiConfidence ||
            view == RendererDebugView::RestirGiVisibility ||
            view == RendererDebugView::WavefrontRestirGi;
    }

    static constexpr bool usesLegacyFinalDebugView(RendererDebugView view) {
        return view == RendererDebugView::RestirGiFinal ||
            usesSpatialDebugView(view);
    }

    static bool usesUncompressedInitialReservoir(
        const RendererSettings& settings,
        bool environmentOverride) {
        return environmentOverride ||
            isProductionMode(settings) ||
            settings.restirGiReservoirLayout == RestirGiReservoirLayout::ValidationFull;
    }

    static bool effectiveHalfResolution(const RendererSettings& settings, uint32_t memoryPressureTier) {
        return settings.restirGiHalfResolution || memoryPressureTier > 0u;
    }

    static constexpr uint32_t kCounterUintCount = 64u;

    static constexpr VkDeviceSize counterSlotByteSize() {
        return sizeof(uint32_t) * kCounterUintCount;
    }

    static constexpr VkDeviceSize counterSlotByteOffset(uint32_t temporalFrameIndex, uint32_t framesInFlight) {
        return (temporalFrameIndex % framesInFlight) * counterSlotByteSize();
    }

    struct SceneDescriptorBindings {
        VkDescriptorBufferInfo meshParams{};
        VkDescriptorBufferInfo materials{};
        VkDescriptorBufferInfo rtTriangleMaterialIds{};
        VkDescriptorBufferInfo instanceRecords{};
        VkDescriptorBufferInfo meshRecords{};
        VkDescriptorBufferInfo localVertices{};
        VkDescriptorBufferInfo localIndices{};
        VkDescriptorBufferInfo geometryTriangleOffsets{};
        VkDescriptorBufferInfo meshGeometryRanges{};
        VkDescriptorBufferInfo tlasGeometryRanges{};
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    };

    static void writeSceneDescriptors(DescriptorWriter& writer, const SceneDescriptorBindings& scene) {
        writer
            .writeBuffer(13, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, scene.meshParams)
            .writeBuffer(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.materials)
            .writeBuffer(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.rtTriangleMaterialIds)
            .writeBuffer(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.instanceRecords)
            .writeBuffer(18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.meshRecords)
            .writeBuffer(19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.localVertices)
            .writeBuffer(20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.localIndices)
            .writeBuffer(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.geometryTriangleOffsets)
            .writeBuffer(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.meshGeometryRanges)
            .writeBuffer(23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.tlasGeometryRanges)
            .writeAccelerationStructure(33, scene.tlas);
    }

    static bool requestsReservoirs(const RendererSettings& settings) {
        return !settings.wavefrontFinalOutputEnabled &&
            (settings.restirGiMode != RestirGiMode::Off ||
                usesDebugView(settings.debugView));
    }

    static bool requestsLegacyFinal(const RendererSettings& settings) {
        return !settings.wavefrontFinalOutputEnabled &&
            (isLegacyCacheMode(settings) || usesLegacyFinalDebugView(settings.debugView));
    }

    static bool requestsProductionTemporal(const RendererSettings& settings) {
        return !settings.wavefrontFinalOutputEnabled && isNewReservoirMode(settings);
    }

    struct TemporalStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool initialReservoirReady = false;
        bool previousProductionHistoryReady = false;
        bool temporalReservoirReady = false;
        bool currentReceiverReady = false;
        bool previousReceiverReady = false;
        bool tlasReady = false;
    };

    static bool canRunProductionTemporal(
        const RendererSettings& settings,
        const TemporalStageResources& resources) {
        return requestsProductionTemporal(settings) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.initialReservoirReady &&
            resources.previousProductionHistoryReady &&
            resources.temporalReservoirReady &&
            resources.currentReceiverReady &&
            resources.previousReceiverReady &&
            resources.tlasReady;
    }

    static bool requestsProductionSpatial(const RendererSettings& settings, bool referenceValidation) {
        return requestsProductionTemporal(settings) &&
            requestsSpatialStage(settings, referenceValidation);
    }

    struct SpatialStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool temporalReservoirReady = false;
        bool currentProductionHistoryReady = false;
        bool currentReceiverReady = false;
        bool tlasReady = false;
    };

    static bool canRunProductionSpatial(
        const RendererSettings& settings,
        bool referenceValidation,
        const SpatialStageResources& resources) {
        return requestsProductionSpatial(settings, referenceValidation) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.temporalReservoirReady &&
            resources.currentProductionHistoryReady &&
            resources.currentReceiverReady &&
            resources.tlasReady;
    }

    static bool requestsProductionFinal(const RendererSettings& settings) {
        return requestsProductionTemporal(settings);
    }

    struct FinalStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool upsampleStageRuns = false;
        bool upsampledReservoirReady = false;
        bool temporalReservoirReady = false;
        bool currentProductionHistoryReady = false;
        bool currentReceiverReady = false;
        bool rawImageReady = false;
        bool pathDataReady = false;
        bool accumulationReady = false;
    };

    static bool canRunProductionFinal(
        const RendererSettings& settings,
        const FinalStageResources& resources) {
        const bool finalReservoirReady = resources.upsampleStageRuns
            ? resources.upsampledReservoirReady
            : (resources.temporalReservoirReady && resources.currentProductionHistoryReady);
        return requestsProductionFinal(settings) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            finalReservoirReady &&
            resources.currentReceiverReady &&
            resources.rawImageReady &&
            resources.pathDataReady &&
            resources.accumulationReady;
    }

    static bool requestsUpsample(
        const RendererSettings& settings,
        bool /*referenceValidation*/,
        bool halfResolution) {
        return requestsProductionTemporal(settings) &&
            halfResolution;
    }

    struct UpsampleStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool temporalReservoirReady = false;
        bool currentProductionHistoryReady = false;
        bool upsampledReservoirReady = false;
        bool currentReceiverReady = false;
    };

    static bool canRunUpsample(
        const RendererSettings& settings,
        bool referenceValidation,
        bool halfResolution,
        const UpsampleStageResources& resources) {
        return requestsUpsample(settings, referenceValidation, halfResolution) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.temporalReservoirReady &&
            resources.currentProductionHistoryReady &&
            resources.upsampledReservoirReady &&
            resources.currentReceiverReady;
    }

    static bool requestsSpatialStage(const RendererSettings& settings, bool referenceValidation) {
        return settings.restirGiSpatialRounds > 0u ||
            usesSpatialDebugView(settings.debugView) ||
            referenceValidation;
    }

    static bool requestsActiveTileMask(
        const RendererSettings& settings,
        bool autoEnabled,
        bool referenceValidation) {
        const bool modeEnabled = settings.restirGiActiveTileMaskMode == RestirGiActiveTileMaskMode::On ||
            (settings.restirGiActiveTileMaskMode == RestirGiActiveTileMaskMode::Auto && autoEnabled);
        return modeEnabled && !referenceValidation;
    }

    static bool requestsCounterCollectionInAutoMode(const RendererSettings& settings) {
        return usesDebugView(settings.debugView) ||
            settings.restirGiMode == RestirGiMode::ReferenceValidation;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::RestirGI;
        contract.role = RendererPassContractRole::LightingReuse;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/RestirGIPass.h (contract + mode/runtime/effective-resolution/resource-sizing/parameter/stage-readiness/counter/history-slot/descriptor-binding policy), src/rtv/PathTracerRenderer.cpp (resource descriptor sources/scheduling)";
        contract.featureFlagsRequired = "restirGiEnabled && restirGiMode != off";
        contract.inputs = rendererContractArray({"GI receiver buffer", "path-traced sample data", "previous GI reservoirs", "motion/depth/normal guides"});
        contract.outputs = rendererContractArray({"temporal GI reservoirs", "spatial GI reservoirs", "upsampled GI reservoirs", "GI final contribution", "GI counters"});
        contract.historyResources = rendererContractArray({"previous GI production reservoirs", "previous GI receiver buffer", "GI active tile mask"});
        contract.descriptorLayouts = rendererContractArray({"ReSTIR GI descriptor set", "guide image descriptor set"});
        contract.pushConstants = rendererContractArray({"RestirGiParams"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/restir_gi_temporal.comp", "shaders/restir_gi_spatial_production.comp", "shaders/restir_gi_final_production.comp"});
        contract.rendergraphReads = rendererContractArray({"restir_gi_previous", "restir_gi_receiver", "path sample data", "temporal guides"});
        contract.rendergraphWrites = rendererContractArray({"restir_gi_temporal", "restir_gi_spatial", "restir_gi_final", "restir_gi_counters"});
        contract.requiredBarriers = rendererContractArray({"temporal to spatial", "half-res upsample to full-res", "final to denoiser"});
        contract.cameraHistoryResetBehavior = "Reject history on camera cut, disocclusion, normal/depth/material/object mismatch, resolution change, or frame-count reset.";
        contract.debugOutputs = rendererContractArray({"restir-gi-validity", "restir-gi-age", "restir-gi-confidence", "restir-gi-path-class", "restir-gi-hit-distance"});
        contract.profilingSections = rendererContractArray({"restir_gi_temporal", "restir_gi_spatial", "restir_gi_upsample", "restir_gi_final", "restir_gi_counters_readback"});
        contract.validationChecks = rendererContractArray({"invalid receiver count", "temporal rejection reasons", "spatial rejection reasons", "half-res grid score"});
        return contract;
    }
};

} // namespace rtv::passes
