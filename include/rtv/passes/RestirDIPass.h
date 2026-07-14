#pragma once

#include "rtv/DescriptorWriter.h"
#include "rtv/GpuScene.h"
#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

#include <Volk/volk.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtv::passes {

struct RestirDIPass {
    static constexpr const char* kContractId = "restir_di";
    static constexpr const char* kPassName = "RestirDIPass";
    static constexpr const char* kRole = "lighting_reuse";
    static constexpr const char* kExtractionState =
        "contract-module plus legacy/estimator/stage-readiness/resource-sizing/parameter/debug/counter/history-slot/history-selection/light-history policy; scheduling and GPU resources remain coordinated by PathTracerRenderer";

    enum class HistorySlot {
        Primary,
        Secondary,
    };

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

    template <typename Resource>
    static const Resource& selectFinalOutputResource(
        bool pingPongHistory,
        bool finalReservoirAliased,
        uint32_t temporalFrameIndex,
        const Resource& finalReservoir,
        const Resource& previousReservoir,
        const Resource& aliasedFinalReservoir) {
        if (pingPongHistory) {
            return selectCurrentHistoryResource(
                true,
                temporalFrameIndex,
                finalReservoir,
                previousReservoir);
        }
        return finalReservoirAliased ? aliasedFinalReservoir : finalReservoir;
    }

    struct LightHistoryKey {
        uint32_t identityLo = 0;
        uint32_t identityHi = 0;
        uint32_t generation = 0;
        uint32_t kind = 0;

        constexpr bool operator<(const LightHistoryKey& rhs) const {
            if (identityHi != rhs.identityHi) return identityHi < rhs.identityHi;
            if (identityLo != rhs.identityLo) return identityLo < rhs.identityLo;
            if (generation != rhs.generation) return generation < rhs.generation;
            return kind < rhs.kind;
        }

        constexpr bool operator==(const LightHistoryKey& rhs) const {
            return identityLo == rhs.identityLo &&
                identityHi == rhs.identityHi &&
                generation == rhs.generation &&
                kind == rhs.kind;
        }
    };

    struct StableLightKey {
        uint32_t identityLo = 0;
        uint32_t identityHi = 0;
        uint32_t kind = 0;

        constexpr bool operator<(const StableLightKey& rhs) const {
            if (identityHi != rhs.identityHi) return identityHi < rhs.identityHi;
            if (identityLo != rhs.identityLo) return identityLo < rhs.identityLo;
            return kind < rhs.kind;
        }

        constexpr bool operator==(const StableLightKey& rhs) const {
            return identityLo == rhs.identityLo &&
                identityHi == rhs.identityHi &&
                kind == rhs.kind;
        }
    };

    static std::vector<LightHistoryKey> lightHistoryKeys(const std::vector<GpuLightRecord>& records) {
        std::vector<LightHistoryKey> keys;
        keys.reserve(records.size());
        for (const GpuLightRecord& record : records) {
            keys.push_back(LightHistoryKey{
                record.identity.x,
                record.identity.y,
                record.identity.z,
                record.metadata.x,
            });
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    static std::vector<StableLightKey> stableLightKeys(const std::vector<GpuLightRecord>& records) {
        std::vector<StableLightKey> keys;
        keys.reserve(records.size());
        for (const GpuLightRecord& record : records) {
            keys.push_back(StableLightKey{
                record.identity.x,
                record.identity.y,
                record.metadata.x,
            });
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    static bool sameStableLightHistorySet(
        const std::vector<GpuLightRecord>& previousRecords,
        const std::vector<GpuLightRecord>& currentRecords) {
        if (previousRecords.empty() || previousRecords.size() != currentRecords.size()) {
            return false;
        }
        return lightHistoryKeys(previousRecords) == lightHistoryKeys(currentRecords);
    }

    static bool sameStableLightIdentitySet(
        const std::vector<GpuLightRecord>& previousRecords,
        const std::vector<GpuLightRecord>& currentRecords) {
        if (previousRecords.empty() || previousRecords.size() != currentRecords.size()) {
            return false;
        }
        return stableLightKeys(previousRecords) == stableLightKeys(currentRecords);
    }

    static bool preservesStableLightHistory(
        const std::vector<GpuLightRecord>& previousRecords,
        const std::vector<GpuLightRecord>& currentRecords) {
        return sameStableLightHistorySet(previousRecords, currentRecords) ||
            sameStableLightIdentitySet(previousRecords, currentRecords);
    }

    static constexpr bool isStandaloneMode(RestirDiMode mode) {
        return mode == RestirDiMode::Production ||
            mode == RestirDiMode::ReferenceValidation ||
            mode == RestirDiMode::HybridCompare;
    }

    static bool isActive(const RendererSettings& settings) {
        return isStandaloneMode(settings.restirDiMode);
    }

    static bool isAnyModeActive(const RendererSettings& settings) {
        return settings.restirDiMode != RestirDiMode::Off;
    }

    static constexpr bool usesDebugView(RendererDebugView view) {
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
            view == RendererDebugView::RestirDiLightMapStatus ||
            view == RendererDebugView::RestirDiInitialReservoir ||
            view == RendererDebugView::RestirDiTemporalReservoir ||
            view == RendererDebugView::RestirDiSpatialReservoir ||
            view == RendererDebugView::RestirDiFinalReservoir ||
            view == RendererDebugView::RestirDiWeightSum ||
            view == RendererDebugView::RestirDiM ||
            view == RendererDebugView::RestirDiLightClass ||
            view == RendererDebugView::RestirDiAge ||
            view == RendererDebugView::RestirDiConfidence ||
            view == RendererDebugView::RestirDiReferenceDiff;
    }

    static bool requestsCounterCollectionInAutoMode(const RendererSettings& settings) {
        return usesDebugView(settings.debugView) ||
            settings.restirDiMode == RestirDiMode::ReferenceValidation ||
            settings.restirDiMode == RestirDiMode::HybridCompare;
    }

    static bool supportsNewEstimatorMode(
        const RendererSettings& settings,
        uint32_t effectiveSamplesPerPixel,
        bool rayQueryVisibilityUnsupported) {
        const bool productionMode = isStandaloneMode(settings.restirDiMode);
        const bool diagnosticView = usesDebugView(settings.debugView);
        return (productionMode || diagnosticView) &&
            effectiveSamplesPerPixel == 1u &&
            !settings.homogeneousVolumeEnabled &&
            !rayQueryVisibilityUnsupported;
    }

    static bool requestsEstimatorWork(const RendererSettings& settings, bool hasUsefulLightCandidate) {
        const bool diagnosticOrValidation = usesDebugView(settings.debugView) ||
            settings.restirDiMode == RestirDiMode::ReferenceValidation ||
            settings.restirDiMode == RestirDiMode::HybridCompare;
        return diagnosticOrValidation || hasUsefulLightCandidate;
    }

    static bool canSkipCompactImportedEmissiveDirectSampling(
        const RendererSettings& settings,
        bool hasImportedEmissive,
        bool hasNonCompactEmissive,
        size_t lightRecordCount,
        float maxImportedPower) {
        return settings.compactImportedEmissiveTriangleSampling &&
            hasImportedEmissive &&
            !hasNonCompactEmissive &&
            lightRecordCount <= 16'384u &&
            maxImportedPower <= 1.0f;
    }

    static constexpr uint32_t kCounterUintCount = 64u;
    static constexpr VkDeviceSize kReceiverFullByteSize = 96u;
    static constexpr VkDeviceSize kReceiverPackedByteSize = 48u;
    static constexpr VkDeviceSize kReservoirFullByteSize = 112u;
    static constexpr VkDeviceSize kReservoirPackedByteSize = 48u;

    static constexpr VkDeviceSize counterSlotByteSize() {
        return sizeof(uint32_t) * kCounterUintCount;
    }

    static constexpr VkDeviceSize counterSlotByteOffset(uint32_t temporalFrameIndex, uint32_t framesInFlight) {
        return (temporalFrameIndex % framesInFlight) * counterSlotByteSize();
    }

    static constexpr VkDeviceSize counterBufferByteSize(uint32_t framesInFlight) {
        const uint32_t slotCount = framesInFlight == 0u ? 1u : framesInFlight;
        return counterSlotByteSize() * slotCount;
    }

    static constexpr VkDeviceSize receiverStride(RestirDiReservoirLayout layout) {
        return layout == RestirDiReservoirLayout::ValidationFull
            ? kReceiverFullByteSize
            : kReceiverPackedByteSize;
    }

    static constexpr VkDeviceSize reservoirStride(RestirDiReservoirLayout layout) {
        return layout == RestirDiReservoirLayout::ValidationFull
            ? kReservoirFullByteSize
            : kReservoirPackedByteSize;
    }

    static constexpr VkDeviceSize pixelByteSize(VkDeviceSize pixelCount, VkDeviceSize strideBytes) {
        return pixelCount * strideBytes;
    }

    static constexpr VkDeviceSize receiverByteSize(VkDeviceSize pixelCount, RestirDiReservoirLayout layout) {
        return pixelByteSize(pixelCount, receiverStride(layout));
    }

    static constexpr VkDeviceSize reservoirByteSize(VkDeviceSize pixelCount, RestirDiReservoirLayout layout) {
        return pixelByteSize(pixelCount, reservoirStride(layout));
    }

    static constexpr VkDeviceSize sourcePixelByteSize(VkDeviceSize pixelCount) {
        return pixelByteSize(pixelCount, sizeof(uint32_t));
    }

    static constexpr bool requiresDedicatedFinalReservoir(
        bool finalReservoirAliased,
        RestirHistoryCopyMode historyCopyMode) {
        return !finalReservoirAliased || historyCopyMode == RestirHistoryCopyMode::PingPong;
    }

    static constexpr VkDeviceSize finalReservoirByteSize(
        VkDeviceSize pixelCount,
        RestirDiReservoirLayout layout,
        bool finalReservoirAliased,
        RestirHistoryCopyMode historyCopyMode) {
        return requiresDedicatedFinalReservoir(finalReservoirAliased, historyCopyMode)
            ? reservoirByteSize(pixelCount, layout)
            : 0u;
    }

    struct ResourceByteSizes {
        VkDeviceSize receiverBytes = 0;
        VkDeviceSize reservoirBytes = 0;
        VkDeviceSize sourcePixelBytes = 0;
        VkDeviceSize finalReservoirBytes = 0;
        VkDeviceSize counterBytes = 0;
    };

    static constexpr ResourceByteSizes resourceByteSizes(
        VkDeviceSize pixelCount,
        RestirDiReservoirLayout layout,
        bool finalReservoirAliased,
        RestirHistoryCopyMode historyCopyMode,
        uint32_t framesInFlight) {
        return ResourceByteSizes{
            receiverByteSize(pixelCount, layout),
            reservoirByteSize(pixelCount, layout),
            sourcePixelByteSize(pixelCount),
            finalReservoirByteSize(pixelCount, layout, finalReservoirAliased, historyCopyMode),
            counterBufferByteSize(framesInFlight),
        };
    }

    static constexpr uint32_t kPreviousLightRecordCapacity = 16'384u;

    static constexpr VkDeviceSize previousLightRecordBufferByteSize() {
        return static_cast<VkDeviceSize>(kPreviousLightRecordCapacity) * sizeof(GpuLightRecord);
    }

    static uint32_t clampedLightRecordCount(size_t recordCount) {
        return static_cast<uint32_t>(
            std::min<uint64_t>(static_cast<uint64_t>(recordCount), kPreviousLightRecordCapacity));
    }

    static VkDeviceSize previousLightRecordCopyBytes(
        uint32_t lightRecordCount,
        VkDeviceSize sourceBufferBytes,
        VkDeviceSize destinationBufferBytes) {
        const VkDeviceSize requestedBytes =
            static_cast<VkDeviceSize>(lightRecordCount) * sizeof(GpuLightRecord);
        return std::min<VkDeviceSize>(requestedBytes, std::min(sourceBufferBytes, destinationBufferBytes));
    }

    static constexpr bool previousLightHistoryValid(VkDeviceSize copyBytes) {
        return copyBytes > 0u;
    }

    struct ConfidenceReplayResources {
        bool diFinalRuns = false;
        bool currentReceiverReady = false;
        bool previousReceiverReady = false;
        bool finalReservoirReady = false;
        bool previousLightRecordsReady = false;
        bool previousLightHistoryValid = false;
    };

    static constexpr bool canRunConfidenceReplay(const ConfidenceReplayResources& resources) {
        return resources.diFinalRuns &&
            resources.currentReceiverReady &&
            resources.previousReceiverReady &&
            resources.finalReservoirReady &&
            resources.previousLightRecordsReady &&
            resources.previousLightHistoryValid;
    }

    static constexpr float kNormalCompatibilityThreshold = 0.85f;
    static constexpr float kDepthCompatibilityThreshold = 0.05f;
    static constexpr float kTemporalLuminanceLimitFactor = 8.0f;
    static constexpr float kConfidenceDecay = 0.96f;
    static constexpr float kLuminanceClampNeighborAverageFactor = 6.0f;
    static constexpr float kLuminanceClampNeighborMaxFactor = 3.0f;

    struct ParameterInputs {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameIndex = 0;
        uint32_t lightVersion = 0;
        uint32_t environmentVersion = 0;
        bool estimatorRuns = false;
        bool temporalStageRuns = false;
        bool historyValid = false;
        bool materialVisibilityEnabled = false;
        bool counterEnabled = false;
        bool rawOutputIsCurrentSample = false;
    };

    static constexpr uint32_t encodedMode(const RendererSettings& settings) {
        return settings.restirMode == RestirMode::HybridCompare ||
                settings.restirDiMode == RestirDiMode::HybridCompare
            ? static_cast<uint32_t>(RestirDiMode::HybridCompare)
            : static_cast<uint32_t>(settings.restirDiMode);
    }

    template <typename Params>
    static Params makeParams(const RendererSettings& settings, const ParameterInputs& inputs) {
        Params params{};
        if (!inputs.estimatorRuns) {
            return params;
        }

        params.width = inputs.width;
        params.height = inputs.height;
        params.frameIndex = inputs.frameIndex;
        params.enabled = 1u;
        params.temporalMaxAge = inputs.temporalStageRuns ? settings.restirDiTemporalMaxAge : 0u;
        params.spatialRounds = settings.restirDiSpatialRounds;
        params.spatialMaxM = settings.restirDiMaxM;
        params.visibilityPolicy = settings.restirDiFinalVisibilityEnabled ? 1u : 0u;
        params.spatialRadius = settings.restirDiSpatialRadius;
        params.normalThreshold = kNormalCompatibilityThreshold;
        params.depthThreshold = kDepthCompatibilityThreshold;
        params.temporalLuminanceLimitFactor =
            settings.restirDiProductionStabilizationEnabled ? kTemporalLuminanceLimitFactor : 0.0f;
        params.confidenceDecay = kConfidenceDecay;
        params.lumClampNeighborAvgFactor = kLuminanceClampNeighborAverageFactor;
        params.lumClampNeighborMaxFactor = kLuminanceClampNeighborMaxFactor;
        params.fireflyClamp = settings.fireflyClamp;
        params.productionClampLuminance = settings.restirDiProductionStabilizationEnabled
            ? settings.restirDiClampLuminance
            : 0.0f;
        params.mode = encodedMode(settings);
        params.spatialResultValid = settings.restirDiSpatialEnabled ? 1u : 0u;
        params.visibilityRayBudget = settings.restirDiVisibilityRayBudget;
        params.historyValid = inputs.historyValid ? 1u : 0u;
        params.materialVisibilityFlags = inputs.materialVisibilityEnabled ? 1u : 0u;
        params.counterEnabled = inputs.counterEnabled ? 1u : 0u;
        params.rawOutputIsCurrentSample = inputs.rawOutputIsCurrentSample ? 1u : 0u;
        params.shadowDistanceBias = settings.shadowDistanceBias;
        params.lightVersion = inputs.lightVersion;
        params.environmentVersion = inputs.environmentVersion;
        return params;
    }

    struct SceneDescriptorBindings {
        VkDescriptorBufferInfo lightRecords{};
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
            .writeBuffer(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.lightRecords)
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
            .writeAccelerationStructure(10, scene.tlas);
    }

    static bool requestsTemporalStage(const RendererSettings& settings) {
        return settings.restirDiTemporalEnabled;
    }

    static bool requestsSpatialStage(const RendererSettings& settings) {
        return settings.restirDiSpatialEnabled;
    }

    struct TemporalStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool currentReceiverReady = false;
        bool initialReservoirReady = false;
        bool previousReservoirReady = false;
        bool previousReceiverReady = false;
        bool tlasReady = false;
    };

    static bool canRunTemporalStage(
        const RendererSettings& settings,
        bool estimatorRequested,
        const TemporalStageResources& resources) {
        return estimatorRequested &&
            requestsTemporalStage(settings) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.currentReceiverReady &&
            resources.initialReservoirReady &&
            resources.previousReservoirReady &&
            resources.previousReceiverReady &&
            resources.tlasReady;
    }

    struct SpatialStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool temporalReservoirReady = false;
        bool currentReceiverReady = false;
        bool tlasReady = false;
    };

    static bool canRunSpatialStage(
        const RendererSettings& settings,
        bool estimatorRequested,
        const SpatialStageResources& resources) {
        return estimatorRequested &&
            requestsSpatialStage(settings) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.temporalReservoirReady &&
            resources.currentReceiverReady &&
            resources.tlasReady;
    }

    struct FinalStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool spatialReservoirReady = false;
        bool currentReceiverReady = false;
        bool rawImageReady = false;
        bool pathDataReady = false;
        bool accumulationReady = false;
        bool tlasReady = false;
    };

    static bool canRunFinalStage(
        bool estimatorRequested,
        const FinalStageResources& resources) {
        return estimatorRequested &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.spatialReservoirReady &&
            resources.currentReceiverReady &&
            resources.rawImageReady &&
            resources.pathDataReady &&
            resources.accumulationReady &&
            resources.tlasReady;
    }

    static bool requestsLegacySpatialStage(const RendererSettings& settings, bool adaptiveSkipSpatial) {
        return !settings.wavefrontFinalOutputEnabled &&
            !adaptiveSkipSpatial &&
            settings.restirDiMode == RestirDiMode::Legacy &&
            settings.restirMode != RestirMode::ClassicNee;
    }

    static bool canAliasFinalReservoir(const RendererSettings& settings, bool resourceAliasingEnabled) {
        return resourceAliasingEnabled &&
            settings.restirHistoryCopyMode != RestirHistoryCopyMode::PingPong &&
            settings.restirDiTemporalEnabled &&
            settings.restirDiSpatialEnabled;
    }

    static bool requestsSharedHistoryPingPong(
        const RendererSettings& settings,
        bool diHistoryActive,
        bool giHistoryActive) {
        return settings.restirHistoryCopyMode == RestirHistoryCopyMode::PingPong &&
            !settings.wavefrontFinalOutputEnabled &&
            (diHistoryActive || giHistoryActive);
    }

    static RestirHistoryCopyMode effectiveHistoryCopyMode(bool pingPongReady) {
        return pingPongReady ? RestirHistoryCopyMode::PingPong : RestirHistoryCopyMode::Copy;
    }

    static const char* sharedHistoryPingPongFallbackReason(
        const RendererSettings& settings,
        bool pingPongReady,
        bool diHistoryActive,
        bool giHistoryActive) {
        if (settings.restirHistoryCopyMode != RestirHistoryCopyMode::PingPong || pingPongReady) {
            return nullptr;
        }
        if (settings.wavefrontFinalOutputEnabled) {
            return "wavefront final output is enabled";
        }
        if (!diHistoryActive && !giHistoryActive) {
            return "no production ReSTIR history path is active";
        }
        return nullptr;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::RestirDI;
        contract.role = RendererPassContractRole::LightingReuse;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/RestirDIPass.h (contract + legacy/estimator/light-candidate/stage-readiness/resource-sizing/parameter/debug/counter/history-slot/light-history/descriptor-binding policy), src/rtv/PathTracerRenderer.cpp (resource descriptor sources/scheduling)";
        contract.featureFlagsRequired = "restirDiMode != off";
        contract.inputs = rendererContractArray({"surface receiver buffer", "scene light records", "previous DI reservoirs", "ReGIR optional service"});
        contract.outputs = rendererContractArray({"initial reservoirs", "temporal reservoirs", "spatial reservoirs", "final reservoirs", "source pixel buffers", "DI counters"});
        contract.historyResources = rendererContractArray({"current/previous DI reservoirs", "current/previous DI receiver buffers", "deterministic previous-light remap table", "light identity generation"});
        contract.descriptorLayouts = rendererContractArray({"ReSTIR DI descriptor set", "scene/light descriptor sets"});
        contract.pushConstants = rendererContractArray({"RestirDiParams"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/restir_di_temporal.comp", "shaders/restir_di_spatial.comp", "shaders/restir_di_final.comp"});
        contract.rendergraphReads = rendererContractArray({"restir_di_receiver", "restir_di_previous", "scene_lights", "regir_reservoirs"});
        contract.rendergraphWrites = rendererContractArray({"restir_di_initial", "restir_di_temporal", "restir_di_spatial", "restir_di_final", "restir_di_counters"});
        contract.requiredBarriers = rendererContractArray({"initial to temporal", "temporal to spatial", "spatial to final", "final to history copy"});
        contract.cameraHistoryResetBehavior = "Reject or reset history on camera cut, light topology/generation change, scene reload, material reload, or incompatible surface reprojection.";
        contract.debugOutputs = rendererContractArray({"restir-di-selected-light", "restir-di-age", "restir-di-m", "restir-di-confidence", "restir-di-rejection-reason", "restir-di-light-map-status"});
        contract.profilingSections = rendererContractArray({"restir_di_temporal", "restir_di_spatial", "restir_di_final", "restir_di_history_copy", "restir_di_counters_readback"});
        contract.validationChecks = rendererContractArray({"invalid reservoir count", "temporal accepted/rejected", "spatial accepted/rejected", "source PDF/target PDF parity", "current/previous stable-light identity remap"});
        return contract;
    }
};

} // namespace rtv::passes
