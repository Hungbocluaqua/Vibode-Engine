#pragma once

#include "rtv/DescriptorWriter.h"
#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

#include <Volk/volk.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace rtv::passes {

struct RegirPass {
    static constexpr const char* kContractId = "regir";
    static constexpr const char* kPassName = "RegirPass";
    static constexpr const char* kRole = "lighting_reuse";
    static constexpr const char* kExtractionState =
        "contract-module plus mode/grid/resource-sizing/parameter/stage-readiness/query/promotion-gate runtime policy; scheduling and GPU resources remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings& settings) {
        return settings.lightingReuseMode == LightingReuseMode::LegacyRestirDiGiPlusReGIR;
    }

    static bool isRequested(const RendererSettings& settings) {
        return isActive(settings);
    }

    static bool canRun(const RendererSettings& settings) {
        return !settings.wavefrontFinalOutputEnabled && isActive(settings);
    }

    static bool analyticSunAvailable(const RendererSettings& settings) {
        const float sunPower = settings.usePhysicalCamera
            ? settings.sunIlluminanceLux
            : settings.sunIntensity;
        const float luminance =
            settings.sunColor.x * 0.2126f +
            settings.sunColor.y * 0.7152f +
            settings.sunColor.z * 0.0722f;
        return settings.sunlightEnabled &&
            settings.sunDirection.y > 0.0f &&
            sunPower > 0.0f &&
            luminance > 0.0f;
    }

    static bool requestsEnvironment(const RendererSettings& settings, bool sceneEnvironmentEnabled) {
        return settings.regirEnvironment &&
            (sceneEnvironmentEnabled || analyticSunAvailable(settings));
    }

    static bool requestsActiveGrid(const RendererSettings& settings) {
        return settings.regirGridMode == RegirGridMode::Active;
    }

    static bool requestsHashGrid(const RendererSettings& settings) {
        return settings.regirGridMode == RegirGridMode::Hash;
    }

    static bool requestsSpatialReuse(const RendererSettings& settings) {
        return settings.regirGridMode != RegirGridMode::Hash &&
            settings.regirSpatialReuse;
    }

    static bool requestsTemporalReuse(const RendererSettings& settings) {
        return settings.regirGridMode != RegirGridMode::Hash &&
            settings.regirTemporalReuse;
    }

    static bool spatialReuseEffective(const RendererSettings& settings, bool regirRequested) {
        return regirRequested && requestsSpatialReuse(settings);
    }

    static bool temporalReuseEffective(const RendererSettings& settings, bool regirRequested) {
        return regirRequested && requestsTemporalReuse(settings);
    }

    static bool hashGridActive(const RendererSettings& settings, bool regirRequested) {
        return regirRequested && settings.regirGridMode == RegirGridMode::Hash;
    }

    static bool activeGridMode(const RendererSettings& settings, bool regirRequested) {
        return regirRequested && settings.regirGridMode == RegirGridMode::Active;
    }

    static bool hashGridSaturated(bool hashGrid, uint32_t hashSaturationCount) {
        return hashGrid && hashSaturationCount > 0u;
    }

    static bool hashReuseFallback(const RendererSettings& settings, bool hashGrid) {
        return hashGrid && (settings.regirSpatialReuse || settings.regirTemporalReuse);
    }

    static bool unsupportedAdvancedRequested(
        const RendererSettings& settings,
        bool hashGrid,
        uint32_t hashSaturationCount) {
        return hashGridSaturated(hashGrid, hashSaturationCount) ||
            hashReuseFallback(settings, hashGrid);
    }

    static RegirGridMode effectiveGridMode(const RendererSettings& settings, bool regirRequested) {
        if (!regirRequested) {
            return RegirGridMode::Dense;
        }
        return settings.regirGridMode;
    }

    static uint32_t effectiveFiniteQueryFramePeriod(const RendererSettings& settings, bool hashGrid) {
        if (settings.regirQueryMode == RegirQueryMode::Deterministic) {
            return 1u;
        }
        if (settings.regirFiniteQueryFramePeriod > 0u) {
            return settings.regirFiniteQueryFramePeriod;
        }
        return hashGrid ? 256u : 8u;
    }

    static double finiteQueryProbability(bool regirRequested, uint32_t framePeriod) {
        return !regirRequested || framePeriod == 0u
            ? 0.0
            : 1.0 / static_cast<double>(framePeriod);
    }

    static uint64_t gridCellCount(const RendererSettings& settings) {
        return static_cast<uint64_t>(std::max(settings.regirGridDimensions.x, 1u)) *
            static_cast<uint64_t>(std::max(settings.regirGridDimensions.y, 1u)) *
            static_cast<uint64_t>(std::max(settings.regirGridDimensions.z, 1u));
    }

    static uint32_t hashCellCapacity(const RendererSettings& settings) {
        const uint64_t totalCells = gridCellCount(settings);
        const uint64_t target = totalCells <= 64ull ? totalCells : std::max<uint64_t>(64ull, totalCells / 4ull);
        uint64_t capacity = 1ull;
        while (capacity < target && capacity < (1ull << 31u)) {
            capacity <<= 1u;
        }
        return static_cast<uint32_t>(std::max<uint64_t>(capacity, 1ull));
    }

    static uint32_t storageCellCapacity(const RendererSettings& settings) {
        if (settings.regirGridMode == RegirGridMode::Hash) {
            return hashCellCapacity(settings);
        }
        return static_cast<uint32_t>(std::min<uint64_t>(gridCellCount(settings), std::numeric_limits<uint32_t>::max()));
    }

    static uint32_t reservoirDispatchCount(uint64_t cellCount, uint32_t reservoirsPerCell) {
        const uint64_t total =
            cellCount * static_cast<uint64_t>(std::max(reservoirsPerCell, 1u));
        return static_cast<uint32_t>(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
    }

    static uint32_t buildReservoirDispatchCount(const RendererSettings& settings) {
        return reservoirDispatchCount(storageCellCapacity(settings), settings.regirReservoirsPerCell);
    }

    static uint32_t reuseReservoirDispatchCount(const RendererSettings& settings) {
        return reservoirDispatchCount(gridCellCount(settings), settings.regirReservoirsPerCell);
    }

    static VkDeviceSize reservoirStorageByteSize(
        const RendererSettings& settings,
        VkDeviceSize reservoirStrideBytes) {
        return static_cast<VkDeviceSize>(storageCellCapacity(settings)) *
            static_cast<VkDeviceSize>(std::max(settings.regirReservoirsPerCell, 1u)) *
            std::max<VkDeviceSize>(reservoirStrideBytes, 1u);
    }

    static VkDeviceSize environmentReservoirByteSize(
        uint32_t reservoirCount,
        VkDeviceSize reservoirStrideBytes) {
        return static_cast<VkDeviceSize>(reservoirCount) *
            std::max<VkDeviceSize>(reservoirStrideBytes, 1u);
    }

    static VkDeviceSize activeGridFeedbackByteSize(const RendererSettings& settings) {
        return static_cast<VkDeviceSize>(gridCellCount(settings) + 4ull) * sizeof(uint32_t);
    }

    static VkDeviceSize hashGridFeedbackByteSize(uint32_t hashCellCapacity) {
        return (static_cast<VkDeviceSize>(hashCellCapacity) + 4ull) * sizeof(uint32_t);
    }

    static constexpr uint32_t controlFlags(const RendererSettings& settings, bool regirRuns) {
        return (regirRuns ? 1u : 0u) |
            (static_cast<uint32_t>(settings.regirGridMode) << 1u) |
            (settings.regirEnvironment ? 8u : 0u) |
            (settings.regirVisibilityReuse ? 16u : 0u);
    }

    static uint32_t temporalHistoryFrames(const RendererSettings& settings) {
        return settings.regirTemporalReuse
            ? std::max(settings.regirTemporalHistory, 1u)
            : 0u;
    }

    struct ParameterInputs {
        uint32_t gridDimX = 1;
        uint32_t gridDimY = 1;
        uint32_t gridDimZ = 1;
        uint32_t reservoirsPerCell = 1;
        uint32_t sampleFrameIndex = 0;
        uint32_t environmentVersion = 0;
        uint32_t infiniteLightBankSize = 0;
        uint32_t environmentBankSize = 0;
        uint32_t sunBankSize = 0;
        bool regirRuns = false;
        bool temporalHistoryValid = false;
    };

    template <typename Params>
    static Params makeParams(const RendererSettings& settings, const ParameterInputs& inputs) {
        Params params{};
        using UVec4 = decltype(params.gridDimensionsReservoirs);
        using Vec4 = decltype(params.gridPadding);
        params.gridDimensionsReservoirs = UVec4(
            inputs.gridDimX,
            inputs.gridDimY,
            inputs.gridDimZ,
            std::max(inputs.reservoirsPerCell, 1u));
        params.controls = UVec4(
            controlFlags(settings, inputs.regirRuns),
            std::max(settings.regirCandidatesPerReservoir, 1u),
            inputs.sampleFrameIndex,
            temporalHistoryFrames(settings));
        params.gridPadding = Vec4(
            std::max(settings.regirGridPadding, 0.0f),
            settings.regirTemporalReuse ? 1.0f : 0.0f,
            inputs.temporalHistoryValid ? 1.0f : 0.0f,
            static_cast<float>(std::max(settings.regirTemporalMaxM, 1u)));
        params.queryControls = Vec4(
            std::clamp(settings.regirCanonicalMix, 0.0f, 1.0f),
            settings.regirQueryMode == RegirQueryMode::Stochastic ? 1.0f : 0.0f,
            settings.regirSpatialReuse ? static_cast<float>(std::clamp(settings.regirSpatialRounds, 1u, 8u)) : 1.0f,
            settings.regirSpatialReuse ? 1.0f : 0.0f);
        params.environmentControls = UVec4(
            inputs.environmentVersion,
            inputs.infiniteLightBankSize,
            inputs.environmentBankSize,
            inputs.sunBankSize);
        return params;
    }

    struct BuildStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool paramsReady = false;
        bool reservoirReady = false;
        bool reservoirCapacityReady = false;
        bool lightRecordsReady = false;
        bool lightBvhReady = false;
        bool meshParamsReady = false;
    };

    static bool canRunBuild(
        const RendererSettings& settings,
        const BuildStageResources& resources) {
        return canRun(settings) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.paramsReady &&
            resources.reservoirReady &&
            resources.reservoirCapacityReady &&
            resources.lightRecordsReady &&
            resources.lightBvhReady &&
            resources.meshParamsReady;
    }

    struct EnvironmentStageResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool reservoirReady = false;
        bool reservoirCapacityReady = false;
    };

    static bool canRunEnvironment(
        const RendererSettings& settings,
        bool regirBuildReady,
        bool sceneEnvironmentEnabled,
        const EnvironmentStageResources& resources) {
        return regirBuildReady &&
            requestsEnvironment(settings, sceneEnvironmentEnabled) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.reservoirReady &&
            resources.reservoirCapacityReady;
    }

    struct ActiveGridResources {
        bool activeCellBufferReady = false;
        bool activeCellCapacityReady = false;
        bool readbackBufferReady = false;
        bool readbackCapacityReady = false;
    };

    static bool canUseActiveGrid(
        const RendererSettings& settings,
        bool regirBuildReady,
        const ActiveGridResources& resources) {
        return regirBuildReady &&
            requestsActiveGrid(settings) &&
            resources.activeCellBufferReady &&
            resources.activeCellCapacityReady &&
            resources.readbackBufferReady &&
            resources.readbackCapacityReady;
    }

    struct HashGridResources {
        bool currentCellBufferReady = false;
        bool currentCellCapacityReady = false;
        bool nextCellBufferReady = false;
        bool nextCellCapacityReady = false;
        bool readbackBufferReady = false;
    };

    static bool canUseHashGrid(
        const RendererSettings& settings,
        bool regirBuildReady,
        const HashGridResources& resources) {
        return regirBuildReady &&
            requestsHashGrid(settings) &&
            resources.currentCellBufferReady &&
            resources.currentCellCapacityReady &&
            resources.nextCellBufferReady &&
            resources.nextCellCapacityReady &&
            resources.readbackBufferReady;
    }

    struct SpatialReuseResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool spatialReservoirReady = false;
        bool spatialReservoirCapacityReady = false;
    };

    static bool canRunSpatialReuse(
        const RendererSettings& settings,
        bool regirBuildReady,
        const SpatialReuseResources& resources) {
        return regirBuildReady &&
            requestsSpatialReuse(settings) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.spatialReservoirReady &&
            resources.spatialReservoirCapacityReady;
    }

    struct TemporalReuseResources {
        bool pipelineReady = false;
        bool descriptorLayoutReady = false;
        bool temporalReservoirReady = false;
        bool temporalReservoirCapacityReady = false;
        bool previousReservoirReady = false;
        bool previousReservoirCapacityReady = false;
    };

    static bool canRunTemporalReuse(
        const RendererSettings& settings,
        bool regirBuildReady,
        const TemporalReuseResources& resources) {
        return regirBuildReady &&
            requestsTemporalReuse(settings) &&
            resources.pipelineReady &&
            resources.descriptorLayoutReady &&
            resources.temporalReservoirReady &&
            resources.temporalReservoirCapacityReady &&
            resources.previousReservoirReady &&
            resources.previousReservoirCapacityReady;
    }

    static uint64_t denseReservoirBytes(
        const RendererSettings& settings,
        uint64_t reportedDenseReservoirBytes,
        uint64_t reservoirStrideBytes) {
        if (reportedDenseReservoirBytes > 0ull) {
            return reportedDenseReservoirBytes;
        }
        return gridCellCount(settings) *
            static_cast<uint64_t>(std::max(settings.regirReservoirsPerCell, 1u)) *
            std::max<uint64_t>(reservoirStrideBytes, 1ull);
    }

    static uint64_t effectiveActiveCellCount(
        bool sparseGrid,
        bool feedbackAvailable,
        uint64_t reportedActiveCellCount,
        uint64_t totalCellCount,
        bool regirRequested) {
        if (!regirRequested) {
            return 0ull;
        }
        if (!sparseGrid) {
            return totalCellCount;
        }
        return feedbackAvailable
            ? std::min<uint64_t>(reportedActiveCellCount, totalCellCount)
            : 0ull;
    }

    static uint64_t effectiveReservoirBytes(
        bool sparseGrid,
        bool feedbackAvailable,
        uint64_t reportedEffectiveReservoirBytes,
        uint64_t denseReservoirBytes,
        bool regirRequested) {
        if (!regirRequested) {
            return 0ull;
        }
        if (sparseGrid) {
            return feedbackAvailable ? reportedEffectiveReservoirBytes : 0ull;
        }
        return std::max<uint64_t>(reportedEffectiveReservoirBytes, denseReservoirBytes);
    }

    struct PromotionDiagnostics {
        uint32_t profiledFrames = 0;
        double gpuFrameAvgMs = 0.0;
        double regirBuildMs = 0.0;
        double regirSpatialReuseMs = 0.0;
        double regirTemporalReuseMs = 0.0;
        bool feedbackAvailable = false;
        uint32_t activeCellCount = 0;
        uint32_t hashCollisionCount = 0;
        uint32_t hashSaturationCount = 0;
        uint32_t hashCellCapacity = 0;
        uint64_t totalCellCount = 0;
        uint64_t denseReservoirBytes = 0;
        uint64_t effectiveReservoirBytes = 0;
        uint64_t backingBytes = 0;
        uint32_t environmentBankSize = 0;
        uint32_t sunBankSize = 0;
        uint32_t validEnvironmentReservoirs = 0;
        uint32_t validSunReservoirs = 0;
        uint64_t environmentBankBytes = 0;
        bool environmentEffective = false;
        bool sunEffective = false;
        bool temporalHistoryValid = false;
        bool finiteLightReferenceMatrixPassed = false;
        bool environmentMatrixPassed = false;
        bool visibilityReuseValidationPassed = false;
        bool equalTimeQualityPassed = false;
        bool manyLightReferencePassed = false;
    };

    static double totalGpuMs(const PromotionDiagnostics& diagnostics) {
        return diagnostics.regirBuildMs +
            diagnostics.regirSpatialReuseMs +
            diagnostics.regirTemporalReuseMs;
    }

    static bool quickPromotionPlumbingPassed(
        const RendererSettings& settings,
        const PromotionDiagnostics& diagnostics) {
        if (!isRequested(settings)) {
            return true;
        }
        const bool memoryEvidence =
            diagnostics.backingBytes > 0ull ||
            diagnostics.effectiveReservoirBytes > 0ull ||
            diagnostics.denseReservoirBytes > 0ull;
        return diagnostics.profiledFrames > 0u &&
            memoryEvidence &&
            !hashGridSaturated(hashGridActive(settings, true), diagnostics.hashSaturationCount);
    }

    static bool fullPromotionEligible(
        const RendererSettings& settings,
        const PromotionDiagnostics& diagnostics) {
        return isRequested(settings) &&
            quickPromotionPlumbingPassed(settings, diagnostics) &&
            diagnostics.finiteLightReferenceMatrixPassed &&
            diagnostics.environmentMatrixPassed &&
            diagnostics.visibilityReuseValidationPassed &&
            diagnostics.equalTimeQualityPassed &&
            diagnostics.manyLightReferencePassed;
    }

    struct BuildDescriptorBindings {
        VkDescriptorBufferInfo lightRecords{};
        VkDescriptorBufferInfo lightBvhNodes{};
        VkDescriptorBufferInfo meshParams{};
        VkDescriptorBufferInfo params{};
        VkDescriptorBufferInfo reservoirs{};
        VkDescriptorBufferInfo gridCells{};
    };

    static void writeBuildDescriptors(DescriptorWriter& writer, const BuildDescriptorBindings& bindings) {
        writer
            .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.lightRecords)
            .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.lightBvhNodes)
            .writeBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindings.meshParams)
            .writeBuffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindings.params)
            .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.reservoirs)
            .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.gridCells);
    }

    struct EnvironmentDescriptorBindings {
        VkDescriptorBufferInfo params{};
        VkDescriptorBufferInfo envParams{};
        VkDescriptorImageInfo environmentImage{};
        VkDescriptorImageInfo environmentSampler{};
        VkDescriptorBufferInfo envRows{};
        VkDescriptorBufferInfo envCols{};
        VkDescriptorBufferInfo skyCdfCols{};
        VkDescriptorBufferInfo environmentReservoirs{};
        VkDescriptorBufferInfo cameraUniform{};
    };

    static void writeEnvironmentDescriptors(DescriptorWriter& writer, const EnvironmentDescriptorBindings& bindings) {
        writer
            .writeBuffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindings.params)
            .writeBuffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindings.envParams)
            .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, bindings.environmentImage)
            .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLER, bindings.environmentSampler)
            .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.envRows)
            .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.envCols)
            .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.skyCdfCols)
            .writeBuffer(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.environmentReservoirs)
            .writeBuffer(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindings.cameraUniform);
    }

    struct SpatialReuseDescriptorBindings {
        VkDescriptorBufferInfo params{};
        VkDescriptorBufferInfo inputReservoirs{};
        VkDescriptorBufferInfo outputReservoirs{};
    };

    static void writeSpatialReuseDescriptors(DescriptorWriter& writer, const SpatialReuseDescriptorBindings& bindings) {
        writer
            .writeBuffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindings.params)
            .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.inputReservoirs)
            .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.outputReservoirs);
    }

    struct TemporalReuseDescriptorBindings {
        VkDescriptorBufferInfo params{};
        VkDescriptorBufferInfo currentReservoirs{};
        VkDescriptorBufferInfo previousReservoirs{};
        VkDescriptorBufferInfo outputReservoirs{};
    };

    static void writeTemporalReuseDescriptors(DescriptorWriter& writer, const TemporalReuseDescriptorBindings& bindings) {
        writer
            .writeBuffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindings.params)
            .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.currentReservoirs)
            .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.previousReservoirs)
            .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings.outputReservoirs);
    }

    static bool shouldTraceFiniteLightsThisFrame(
        const RendererSettings& settings,
        uint32_t temporalFrameIndex,
        bool hashGridActive) {
        if (settings.regirQueryMode == RegirQueryMode::Deterministic) {
            return true;
        }
        const uint32_t framePeriod = settings.regirFiniteQueryFramePeriod > 0u
            ? settings.regirFiniteQueryFramePeriod
            : (hashGridActive ? 256u : 8u);
        return temporalFrameIndex % framePeriod == framePeriod - 1u;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::Regir;
        contract.role = RendererPassContractRole::LightingReuse;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/RegirPass.h (contract + mode/grid/capacity/resource-sizing/parameter/stage-readiness/query/promotion-gate/descriptor-binding policy), src/rtv/PathTracerRenderer.cpp (resource descriptor sources/scheduling)";
        contract.featureFlagsRequired = "lightingReuseMode == legacy-regir";
        contract.inputs = rendererContractArray({"scene light records", "environment distribution", "camera/world bounds", "previous ReGIR reservoirs"});
        contract.outputs = rendererContractArray({"grid metadata", "cell reservoirs", "environment reservoirs", "active cell feedback", "ReGIR counters"});
        contract.historyResources = rendererContractArray({"previous ReGIR reservoirs", "active/hash cell feedback", "environment/light generation"});
        contract.descriptorLayouts = rendererContractArray({"ReGIR descriptor set", "scene light descriptor set"});
        contract.pushConstants = rendererContractArray({"RegirParams"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/regir_build.comp", "shaders/regir_spatial_reuse.comp", "shaders/regir_temporal_reuse.comp"});
        contract.rendergraphReads = rendererContractArray({"scene lights", "previous regir reservoirs", "environment map"});
        contract.rendergraphWrites = rendererContractArray({"regir_reservoirs", "regir_environment_reservoirs", "regir_active_cells", "regir_hash_tables"});
        contract.requiredBarriers = rendererContractArray({"build to temporal/spatial reuse", "reuse to consumer sampling"});
        contract.cameraHistoryResetBehavior = "Invalidate temporal history when light or environment generation changes, grid dimensions change, scene bounds change, or camera reset requests it.";
        contract.debugOutputs = rendererContractArray({"regir-grid-occupancy", "regir-selected-light", "regir-reservoir-weight", "regir-infinite-source", "regir-environment-pdf"});
        contract.profilingSections = rendererContractArray({"regir_build", "regir_temporal_reuse", "regir_spatial_reuse"});
        contract.validationChecks = rendererContractArray({"empty-light scene", "many-light stress", "environment-only", "hash/active grid saturation"});
        return contract;
    }
};

} // namespace rtv::passes
