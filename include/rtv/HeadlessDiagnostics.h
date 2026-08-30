#pragma once

#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"
#include "rtv/GpuCrashDiagnostics.h"
#include "rtv/NsightPerfDiagnostics.h"
#include "rtv/GpuUploadTicket.h"
#include "rtv/MainThreadApplyTicket.h"
#include "rtv/TopologyRebuildTicket.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace rtv {

class Application;
class DiagnosticImageExport;
class PathTracerRenderer;

struct HeadlessDiagnosticsConfig {
    bool headless = false;
    uint32_t headlessWidth = 1280;
    uint32_t headlessHeight = 720;
    uint32_t warmupFrames = 0;
    uint32_t totalFrames = 120;
    std::optional<uint32_t> fixedSeed;
    bool profile = false;
    bool rayTracingDiagnosticCounters = false;
    bool runValidationSuite = false;
    std::optional<std::filesystem::path> profileJsonPath;
    std::optional<std::filesystem::path> dumpRenderGraphPath;
    std::optional<std::filesystem::path> saveDebugViewsDir;
    std::optional<std::filesystem::path> saveFrameSequenceDir;
    std::vector<RendererDebugView> sequenceViews;
    uint32_t sequenceStartFrame = 0;
    std::optional<uint32_t> sequenceFrameCount;
    uint32_t sequenceStep = 1;
    std::optional<std::filesystem::path> captureRenderDocPath;
    uint32_t captureFrame = 60;
    std::optional<std::filesystem::path> makeDebugPackageDir;
    std::optional<std::filesystem::path> validationOutputDir;
    bool disableAsyncCompute = false;
    bool singleQueueFallback = false;
    bool disableResourceAliasing = false;
    bool wavefrontValidationMode = false;
};

struct ProfileReport {
    std::string engineVersion = "0.1.0";
    std::string gitCommit;
    std::string gpuName;
    std::string driverVersion;
    std::string vulkanVersion;
    std::string restirGiLayout = "compressed";
    RestirHistoryCopyMode effectiveRestirHistoryCopyMode = RestirHistoryCopyMode::Copy;
    std::string restirHistoryCopyFallbackReason;
    bool effectiveRestirGiActiveTileMaskEnabled = false;
    PathTraceKernelMode effectivePathTraceKernelMode = PathTraceKernelMode::Generic;
    bool native2BTerminalPayloadActive = false;
    bool native2BCompactPrimaryLightsActive = false;
    std::string pathTraceKernelFallbackReason;

    struct Resolution {
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t displayWidth = 0;
        uint32_t displayHeight = 0;
        float renderScale = 1.0f;
    } resolution{};

    uint32_t frameCount = 0;
    uint32_t warmupFrames = 0;
    uint32_t profiledFrames = 0;
    bool rayTracingDiagnosticCountersEnabled = false;
    std::string lastAccumulationResetReason = "Startup";
    nlohmann::json temporalSystemDiagnostics = nlohmann::json::object();

    struct MinMaxAvg {
        float min = 0.0f;
        float avg = 0.0f;
        float max = 0.0f;
        float p95 = 0.0f;
        float p99 = 0.0f;
    };
    MinMaxAvg cpuFrameMs{};
    MinMaxAvg gpuFrameMs{};

    struct PerPassGpuMs {
        float pathTrace = 0.0f;
        float restirHistoryClear = 0.0f;
        float restirGiClear = 0.0f;
        float restirGiTemporal = 0.0f;
        float restirSpatial = 0.0f;
        float restirSpatialCopy = 0.0f;
        float restirGiSpatial = 0.0f;
        float restirGiUpsample = 0.0f;
        float restirGiFinal = 0.0f;
        float restirGiCountersReadback = 0.0f;
        float regirBuild = 0.0f;
        float regirSpatialReuse = 0.0f;
        float regirTemporalReuse = 0.0f;
        float restirDiTemporal = 0.0f;
        float restirDiSpatial = 0.0f;
        float restirDiFinal = 0.0f;
        float restirDiHistoryCopy = 0.0f;
        float restirDiCountersReadback = 0.0f;
        float fogIntegrate = 0.0f;
        float atmosphere = 0.0f;
        float atmosphereTransmittance = 0.0f;
        float atmosphereMultiScatter = 0.0f;
        float atmosphereSkyView = 0.0f;
        float atmosphereSkyReproject = 0.0f;
        float atmosphereSkyCdf = 0.0f;
        float atmosphereAerialPerspective = 0.0f;
        float denoiser = 0.0f;
        float momentUpdate = 0.0f;
        float adaptiveSamplingDiagnostics = 0.0f;
        float adaptiveSamplingFill = 0.0f;
        float historyCopy = 0.0f;
        float skipDenoiserCopy = 0.0f;
        float taa = 0.0f;
        float taaHistoryCopy = 0.0f;
        float dlssGuides = 0.0f;
        float dlss = 0.0f;
        float dlssRayReconstructionGuides = 0.0f;
        float dlssRayReconstruction = 0.0f;
        float autoExposureHistogramClear = 0.0f;
        float autoExposureHistogram = 0.0f;
        float autoExposureReduce = 0.0f;
        float toneMap = 0.0f;
        float selectionOutline = 0.0f;
        float fullscreen = 0.0f;
        float editorPresentation = 0.0f;
        float dynamicBlasUpdate = 0.0f;
        float wavefrontTrace = 0.0f;
        float wavefrontSecondaryTrace = 0.0f;
        float wavefrontSortedTrace = 0.0f;
        float wavefrontShadowTrace = 0.0f;
        float wavefrontShade = 0.0f;
        float wavefrontSecondaryShade = 0.0f;
        float wavefrontSortedShade = 0.0f;
        float wavefrontCompact = 0.0f;
        float wavefrontSort = 0.0f;
    } perPassGpuMs{};
    PerPassGpuMs perPassGpuMsP95{};
    PerPassGpuMs perPassGpuMsP99{};

    struct QueueLaneMs {
        float graphics = 0.0f;
        float rayTracing = 0.0f;
        float compute = 0.0f;
        float queueWait = 0.0f;
    } queueLaneMs{};

    struct AsyncComputeReport {
        bool enabled = false;
        bool disabledByCli = false;
        bool singleQueueFallback = false;
        bool timelineSemaphore = false;
        bool independentQueue = false;
        bool dedicatedComputeFamily = false;
        bool crossFamily = false;
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> computeFamily;
        uint32_t computeQueueIndex = 0;
        std::string resourceSharingMode = "exclusive";
        uint32_t resourceSharingQueueFamilyCount = 0;
        std::vector<uint32_t> resourceSharingQueueFamilies;
    } asyncCompute{};

    struct OpacityMicromapReport {
        bool supported = false;
        bool extensionSupported = false;
        bool micromapFeature = false;
        bool captureReplay = false;
        bool hostCommands = false;
        uint32_t maxOpacity2StateSubdivisionLevel = 0;
        uint32_t maxOpacity4StateSubdivisionLevel = 0;
        std::string disabledReason;

        struct PreprocessReport {
            uint32_t subdivisionLevel = 0;
            uint32_t eligiblePrimitiveCount = 0;
            uint32_t generatedPrimitiveCount = 0;
            uint32_t alphaTestedPrimitiveCount = 0;
            uint32_t blendedPrimitiveCount = 0;
            uint32_t alphaTexturePrimitiveCount = 0;
            uint32_t constantAlphaPrimitiveCount = 0;
            uint32_t cacheEntryCount = 0;
            uint32_t cacheHitCount = 0;
            uint32_t totalTriangleCount = 0;
            uint32_t microTriangleCount = 0;
            uint32_t opaqueCount = 0;
            uint32_t transparentCount = 0;
            uint32_t unknownCount = 0;
            uint32_t mixedCount = 0;
            uint64_t dataBytes = 0;
            double preprocessingMs = 0.0;
            uint32_t validationErrorCount = 0;
            std::vector<std::string> warnings;
        } preprocess{};
        struct BuildReport {
            bool requested = false;
            bool supported = false;
            bool active = false;
            uint32_t micromapCount = 0;
            uint32_t meshCount = 0;
            uint32_t triangleArrayCount = 0;
            uint32_t indexedTriangleCount = 0;
            uint32_t packedMicroTriangleCount = 0;
            uint64_t micromapBytes = 0;
            uint64_t buildInputBytes = 0;
            uint64_t buildScratchBytes = 0;
            float buildMs = 0.0f;
            std::string fallbackReason;
        } build{};
    } opacityMicromap{};

    struct SerReport {
        bool supported = false;
        bool enabled = false;
        bool extensionSupported = false;
        bool invocationReorderFeature = false;
        bool dedicatedSerPipeline = false;
        bool pipelineCreateFlagRequired = false;
        bool maxInvocationReorderDepthReported = false;
        uint32_t maxRayTracingInvocationReorderDepth = 0;
        bool performanceEvidenceAvailable = false;
        bool performanceTargetPassed = false;
        float performanceTargetMinPercent = 20.0f;
        float performanceTargetMaxPercent = 40.0f;
        float observedImprovementPercent = 0.0f;
        std::string reorderingHint = "none";
        std::string disabledReason;
    } shaderExecutionReordering{};

    struct RayTracingMotionBlurReport {
        bool supported = false;
        bool enabled = false;
        bool extensionSupported = false;
        bool rayTracingMotionBlurFeature = false;
        bool rayTracingMotionBlurPipelineTraceRaysIndirect = false;
        bool motionInstancesActive = false;
        uint32_t motionInstanceCount = 0;
        uint32_t movingInstanceCount = 0;
        uint32_t staticInstanceCount = 0;
        uint32_t tlasRefitCount = 0;
        float maxTransformDelta = 0.0f;
        bool hasMovingAndStaticInstances = false;
        std::string disabledReason;
    } rayTracingMotionBlur{};

    struct PipelineStatistics {
        uint64_t rayInvocations = 0;
        uint64_t triangleHits = 0;
        uint64_t aabbHits = 0;
        bool supported = false;
        bool valid = false;
        std::string unavailableReason;
    } pipelineStatistics{};

    struct RayTracingDiagnosticCounterReport {
        static constexpr size_t kRawSlotCount = 64;
        std::array<uint64_t, kRawSlotCount> rawSlots{};
        uint64_t cameraAnyHitInvocations = 0;
        uint64_t cameraAnyHitIgnored = 0;
        uint64_t cameraAnyHitAccepted = 0;
        uint64_t shadowAnyHitInvocations = 0;
        uint64_t shadowAnyHitIgnored = 0;
        uint64_t shadowAnyHitAccepted = 0;
        uint64_t surfaceTraceRays = 0;
        uint64_t shadowTraceRays = 0;
        uint64_t closestHitInvocations = 0;
        uint64_t closestHitAlphaMaterials = 0;
        uint64_t causticShadowAttempts = 0;
        uint64_t causticTransmissiveHits = 0;
        uint64_t causticTransmissiveVisible = 0;
        uint64_t causticShadowBlocked = 0;
        uint64_t primarySurfaceTraceRays = 0;
        uint64_t terminalSurfaceTraceRays = 0;
        uint64_t shadowSurfaceTraceRays = 0;
        uint64_t envDirectShadowRays = 0;
        uint64_t sunDirectShadowRays = 0;
        uint64_t emissiveDirectShadowRays = 0;
        uint64_t transmissiveShadowSurfaceTraces = 0;
        uint64_t fastShadowTransmittanceUsed = 0;
        uint64_t fullShadowTransmittanceUsed = 0;
        uint64_t terminalFastDirectUsed = 0;
        uint64_t terminalGenericDirectUsed = 0;
        uint64_t terminalMaterialFullDecode = 0;
        uint64_t terminalMaterialHeaderOnly = 0;
        uint64_t primaryAnyHitOpaque = 0;
        uint64_t primaryAnyHitAlphaTested = 0;
        uint64_t primaryAnyHitBlended = 0;
        uint64_t terminalAnyHitInvocations = 0;
        uint64_t terminalAnyHitOpaque = 0;
        uint64_t terminalAnyHitAlphaTested = 0;
        uint64_t terminalAnyHitBlended = 0;
        uint64_t closestHitPrimary = 0;
        uint64_t closestHitTerminal = 0;
        uint64_t causticBlockerOpaque = 0;
        uint64_t causticBlockerAlphaTested = 0;
        uint64_t causticBlockerBlended = 0;
        uint64_t terminalFastDirectFlagDisabled = 0;
        uint64_t terminalFastDirectSceneLights = 0;
        uint64_t terminalFastDirectTransmissiveScene = 0;
        uint64_t terminalFastDirectVolume = 0;
        uint64_t terminalFastDirectDebug = 0;
        uint64_t terminalFastDirectMaterialTransmissive = 0;
        uint64_t terminalDirectSkippedEmissiveOrUnlit = 0;
    } rayTracingDiagnosticCounters{};

    struct AlphaAnyHitMaterialReport {
        uint32_t materialIndex = 0;
        uint64_t primaryAnyHit = 0;
        uint64_t terminalAnyHit = 0;
        uint64_t shadowAnyHit = 0;
        uint64_t closestHit = 0;
        uint64_t total = 0;
    };
    std::vector<AlphaAnyHitMaterialReport> alphaAnyHitTopMaterials;

    std::vector<uint64_t> restirDiCounters;
    std::vector<uint64_t> restirGiCounters;
    bool restirDiHistoryValid = false;
    bool restirGiHistoryValid = false;

    struct RayTracingGeometryReport {
        uint32_t opaquePrimitiveCount = 0;
        uint32_t alphaTestedPrimitiveCount = 0;
        uint32_t blendedPrimitiveCount = 0;
        uint32_t opaqueTriangleCount = 0;
        uint32_t alphaTestedTriangleCount = 0;
        uint32_t blendedTriangleCount = 0;
        uint32_t meshCountWithOnlyOpaqueGeometry = 0;
        uint32_t meshCountWithAlphaTestedGeometry = 0;
        uint32_t meshCountWithBlendedGeometry = 0;
        uint32_t blasGeometryCount = 0;
        uint32_t blasOpaqueGeometryCount = 0;
        uint32_t blasAlphaTestedGeometryCount = 0;
        uint32_t blasBlendedGeometryCount = 0;
        uint32_t blasOpacityMicromapGeometryCount = 0;
        uint32_t blasBuildBatchCount = 0;
        uint64_t cullableTriangleCount = 0;
        uint64_t cullDisabledTriangleCount = 0;
        uint32_t splitMeshCount = 0;
        uint32_t duplicatedTlasInstanceCount = 0;
        uint32_t actualBlasCount = 0;
        uint64_t splitBlasBytes = 0;
        bool hardwareBackfaceCullingEnabled = false;
        uint32_t transmissiveShadowCasterCount = 0;
        bool fastShadowTransmittanceEligible = false;
    } rayTracingGeometry{};

    struct AnimatedGeometryReport {
        uint32_t meshInstanceCount = 0;
        uint32_t staticMeshInstanceCount = 0;
        uint32_t transformOnlyCandidateInstanceCount = 0;
        uint32_t deformingInstanceCount = 0;
        uint32_t morphDeformingInstanceCount = 0;
        uint32_t skinnedDeformingInstanceCount = 0;
        uint32_t combinedMorphSkinnedInstanceCount = 0;
        uint32_t runtimeMeshInstanceCount = 0;
        uint32_t materialOverrideRuntimeMeshInstanceCount = 0;
        uint32_t missingMeshInstanceCount = 0;
        uint32_t totalPrimitiveCount = 0;
        uint32_t totalTriangleCount = 0;
        uint32_t gpuSkinningCandidateInstanceCount = 0;
        uint32_t gpuSkinningMorphPreSkinInstanceCount = 0;
        uint32_t gpuSkinningJointMatrixCount = 0;
        uint64_t gpuSkinningJointUploadBytes = 0;
        uint64_t gpuSkinningPreviousJointUploadBytes = 0;
        uint64_t gpuSkinningSourceVertexUploadBytes = 0;
        uint64_t gpuSkinningMorphDeltaUploadBytes = 0;
        uint32_t gpuSkinningCurrentVertexCount = 0;
        uint32_t gpuSkinningPreviousVertexCount = 0;
        uint64_t gpuSkinningCurrentVertexBufferBytes = 0;
        uint64_t gpuSkinningPreviousVertexBufferBytes = 0;
        uint32_t gpuSkinningCpuFallbackInstanceCount = 0;
        uint32_t gpuSkinningDispatchRecordCount = 0;
        bool gpuSkinningRendererPlanActive = false;
        uint64_t gpuSkinningRendererJointUploadBytes = 0;
        uint64_t gpuSkinningRendererPreviousJointUploadBytes = 0;
        uint64_t gpuSkinningRendererSourceVertexUploadBytes = 0;
        uint64_t gpuSkinningRendererMorphDeltaUploadBytes = 0;
        uint64_t gpuSkinningRendererCurrentVertexBufferBytes = 0;
        uint64_t gpuSkinningRendererPreviousVertexBufferBytes = 0;
        bool gpuSkinningRendererBuffersAllocated = false;
        bool gpuSkinningRendererSourceVertexBufferUploaded = false;
        bool gpuSkinningRendererMorphDeltaBufferUploaded = false;
        bool gpuSkinningRendererJointBufferUploaded = false;
        bool gpuSkinningRendererPreviousJointBufferUploaded = false;
        bool gpuSkinningRendererJointPayloadRefreshUploaded = false;
        bool gpuSkinningRendererJointPayloadRefreshStaged = false;
        bool gpuSkinningRendererJointPayloadRefreshCopyRecorded = false;
        uint32_t gpuSkinningRendererJointPayloadRefreshCount = 0;
        uint32_t gpuSkinningRendererJointPayloadRefreshCopyRecordCount = 0;
        uint64_t gpuSkinningRendererJointPayloadRefreshBytes = 0;
        uint64_t gpuSkinningRendererJointPayloadRefreshStagedBytes = 0;
        uint64_t gpuSkinningRendererJointPayloadRefreshCopyBytes = 0;
        bool gpuSkinningRendererComputePipelineCreated = false;
        bool gpuSkinningRendererDescriptorsBound = false;
        bool gpuSkinningRendererComputeDispatchEnabled = false;
        uint32_t gpuSkinningRendererComputeDispatchRecordCount = 0;
        uint32_t gpuSkinningRendererComputeMorphDispatchRecordCount = 0;
        bool gpuSkinningRendererOutputReadbackBufferAllocated = false;
        bool gpuSkinningRendererOutputReadbackCopyRecorded = false;
        bool gpuSkinningRendererOutputReadbackValidationPassed = false;
        uint32_t gpuSkinningRendererOutputReadbackVertexCount = 0;
        float gpuSkinningRendererOutputReadbackMaxPositionError = 0.0f;
        bool gpuSkinningRendererInitialComputeDispatchSubmitted = false;
        bool gpuSkinningRayTracingGeometryInputEnabled = false;
        uint32_t gpuSkinningRayTracingGeometryInputMeshCount = 0;
        uint32_t gpuSkinningRayTracingGeometryInputGeometryCount = 0;
        bool gpuSkinningRayTracingDescriptorInputEnabled = false;
        bool gpuSkinningRayTracingDescriptorInputMixedSceneEnabled = false;
        uint32_t gpuSkinningRayTracingDescriptorInputMeshCount = 0;
        uint32_t gpuSkinningRayTracingDescriptorInputBindingCount = 0;
        bool gpuSkinningSkinnedMotionVectorsEnabled = false;
        bool gpuSkinningSkinnedMotionVectorsPreviousVertexInputEnabled = false;
        bool gpuSkinningSkinnedMotionVectorsDescriptorBound = false;
        uint32_t rayTracingBlasCount = 0;
        uint32_t rayTracingInstanceCount = 0;
        uint32_t tlasRefitCount = 0;
        float lastTlasRefitMs = 0.0f;
        uint32_t dynamicBlasUpdateCount = 0;
        float dynamicBlasUpdateRecordMs = 0.0f;
        float dynamicBlasUpdateMs = 0.0f;
        bool dynamicBlasUpdateSupported = false;
        uint32_t dynamicBlasUnsupportedInstanceCount = 0;
        uint32_t dynamicBlasCpuFallbackInstanceCount = 0;
        std::string dynamicBlasFallbackPolicy = "not_reported";
        std::vector<std::string> dynamicBlasFallbackWarnings;
        float dynamicBlasBudgetMs = 0.0f;
        bool dynamicBlasBudgetExceeded = false;
        uint32_t dynamicBlasOverBudgetFrameCount = 0;
        std::string dynamicBlasBudgetPolicy = "not_reported";
        std::string transformOnlyPolicy;
        std::string deformingPolicy;
        std::string morphPolicy;
        std::string skinningPolicy;
        std::string gpuSkinningDataPolicy;
        std::string gpuSkinningJointUploadPolicy;
        std::string gpuSkinningBufferPolicy;
        std::string gpuSkinningComputeShader;
        std::string tlasRefitPolicy = "not_recorded";
        std::string dynamicBlasTimingPolicy = "not_implemented";
    } animatedGeometry{};

    struct SceneLightReport {
        struct Sample {
            uint32_t recordIndex = 0;
            uint32_t type = 0;
            uint32_t sourceIndex = 0;
            std::array<float, 3> positionOrDirection{0.0f, 0.0f, 0.0f};
            float radiusOrSize = 0.0f;
            float weight = 0.0f;
        };

        uint32_t recordCount = 0;
        uint32_t emissiveCount = 0;
        uint32_t authoredCount = 0;
        uint32_t directionalCount = 0;
        uint32_t pointCount = 0;
        uint32_t areaCount = 0;
        uint32_t spotCount = 0;
        uint32_t authoredPositionalCount = 0;
        uint32_t authoredDistinctPositionCount = 0;
        bool authoredPositionsCollapsed = false;
        std::vector<Sample> emissiveSamples;
        std::vector<Sample> authoredSamples;
    } sceneLights{};

    struct WavefrontQueueReport {
        bool buffersAllocated = false;
        bool clearValidationPassed = false;
        uint32_t maxPathDepth = 0;
        uint32_t rayQueueCapacity = 0;
        uint32_t compactedRayQueueCapacity = 0;
        uint32_t sortedRayQueueCapacity = 0;
        uint32_t hitQueueCapacity = 0;
        uint32_t shadowQueueCapacity = 0;
        uint32_t pixelStateCapacity = 0;
        uint32_t rayQueueCount = 0;
        uint32_t hitQueueCount = 0;
        uint32_t shadowQueueCount = 0;
        uint32_t pixelStateCount = 0;
        uint32_t clearValidationCounter = 0;
        bool primaryGenerationEnabled = false;
        bool primaryGenerationValidationPassed = false;
        uint32_t expectedPrimaryRayCount = 0;
        uint32_t sampledPrimaryRayCount = 0;
        float firstRayDirectionError = 0.0f;
        float centerRayDirectionError = 0.0f;
        float cornerRayDirectionError = 0.0f;
        float maxRayDirectionError = 0.0f;
        bool traceEnabled = false;
        bool traceValidationPassed = false;
        bool traceRaysIndirectSupported = false;
        bool secondaryTraceIndirectEnabled = false;
        uint32_t traceCheckedPixels = 0;
        uint32_t traceHitMismatchCount = 0;
        uint32_t traceInstanceMismatchCount = 0;
        uint32_t traceDepthMismatchCount = 0;
        uint32_t traceNormalMismatchCount = 0;
        bool shadeEnabled = false;
        bool shadeValidationPassed = false;
        uint32_t shadeCheckedPixels = 0;
        uint32_t shadeHitCount = 0;
        uint32_t shadeMissCount = 0;
        uint32_t shadeTerminatedCount = 0;
        uint32_t shadeShadowRayCount = 0;
        uint32_t shadeSecondaryRayCount = 0;
        uint32_t shadeMaterialCount = 0;
        uint32_t shadeRestirReservoirWriteCount = 0;
        uint32_t shadeRestirValidCandidateCount = 0;
        uint32_t shadeRestirTemporalMergeCount = 0;
        uint32_t shadeRestirInvalidCandidateCount = 0;
        uint32_t shadeRestirGiReservoirWriteCount = 0;
        uint32_t shadeRestirGiValidCandidateCount = 0;
        uint32_t shadeRestirGiTemporalMergeCount = 0;
        uint32_t shadeRestirGiInvalidCandidateCount = 0;
        bool secondaryShadeEnabled = false;
        bool secondaryShadeValidationPassed = false;
        uint32_t secondaryShadeCheckedRays = 0;
        uint32_t secondaryShadeHitCount = 0;
        uint32_t secondaryShadeMissCount = 0;
        uint32_t secondaryShadeTerminatedCount = 0;
        uint32_t secondaryShadeShadowRayCount = 0;
        uint32_t secondaryShadeSecondaryRayCount = 0;
        uint32_t secondaryShadeMaterialCount = 0;
        float secondaryShadeCostMs = 0.0f;
        float secondaryShadeMicrosecondsPerRay = 0.0f;
        float secondaryPathCostMs = 0.0f;
        float secondaryPathMicrosecondsPerRay = 0.0f;
        bool sortedShadeEnabled = false;
        bool sortedShadeValidationPassed = false;
        uint32_t sortedShadeCheckedRays = 0;
        uint32_t sortedShadeHitCount = 0;
        uint32_t sortedShadeMissCount = 0;
        uint32_t sortedShadeTerminatedCount = 0;
        uint32_t sortedShadeShadowRayCount = 0;
        uint32_t sortedShadeSecondaryRayCount = 0;
        uint32_t sortedShadeMaterialCount = 0;
        float sortedShadeCostMs = 0.0f;
        float sortedShadeMicrosecondsPerRay = 0.0f;
        float sortedPathCostMs = 0.0f;
        float sortedPathMicrosecondsPerRay = 0.0f;
        bool sortNetBenefitEvidenceAvailable = false;
        bool sortNetBenefitPassed = false;
        float sortNetBenefitMs = 0.0f;
        bool compactEnabled = false;
        bool compactValidationPassed = false;
        uint32_t compactInputRayCount = 0;
        uint32_t compactScannedRayCount = 0;
        uint32_t compactLiveRayCount = 0;
        uint32_t compactOutputRayCount = 0;
        uint32_t compactDroppedInvalidCount = 0;
        uint32_t compactOverflowCount = 0;
        uint32_t compactInvalidPixelCount = 0;
        uint32_t compactMappingMismatchCount = 0;
        float compactQueueOccupancy = 0.0f;
        float compactSurvivalRatio = 0.0f;
        float compactCostMs = 0.0f;
        float compactMicrosecondsPerRay = 0.0f;
        float primaryQueueOccupancy = 0.0f;
        float traceHitQueueOccupancy = 0.0f;
        float shadeSecondaryQueueOccupancy = 0.0f;
        float sortOutputQueueOccupancy = 0.0f;
        float secondaryShadeShadowQueueOccupancy = 0.0f;
        float secondaryShadeSecondaryQueueOccupancy = 0.0f;
        float sortedShadeShadowQueueOccupancy = 0.0f;
        float sortedShadeSecondaryQueueOccupancy = 0.0f;
        bool queueBalanceValidationPassed = false;
        bool queueStarvationDetected = false;
        uint32_t queueOverflowCount = 0;
        bool sortEnabled = false;
        bool finalOutputEnabled = false;
        bool sortValidationPassed = false;
        uint32_t sortInputRayCount = 0;
        uint32_t sortOutputRayCount = 0;
        uint32_t sortActiveBucketCount = 0;
        uint32_t sortVerifiedRayCount = 0;
        uint32_t sortBucketCount = 0;
        uint32_t sortOverflowCount = 0;
        uint32_t sortInvalidPixelCount = 0;
        uint32_t sortOrderViolationCount = 0;
        float sortCostMs = 0.0f;
        float sortMicrosecondsPerRay = 0.0f;
        bool shadowTraceEnabled = false;
        bool shadowTraceValidationPassed = false;
        uint32_t shadowTraceCheckedRays = 0;
        uint32_t shadowTraceVisibleCount = 0;
        uint32_t shadowTraceOccludedCount = 0;
        uint32_t shadowTraceAppliedCount = 0;
        bool directLightingParityPassed = false;
        uint32_t directLightingCheckedPixels = 0;
        uint32_t directLightingMismatchCount = 0;
        float directLightingMaxAbsError = 0.0f;
        float directLightingMaxRelativeError = 0.0f;
        float shadowQueueOccupancy = 0.0f;
        float shadowTraceRaysPerPixel = 0.0f;
        float shadowTraceVisibleRatio = 0.0f;
        float shadowTraceOccludedRatio = 0.0f;
        float shadowTraceCostMs = 0.0f;
        float shadowTraceMicrosecondsPerRay = 0.0f;
        uint64_t rayQueueBytes = 0;
        uint64_t compactedRayQueueBytes = 0;
        uint64_t sortedRayQueueBytes = 0;
        uint64_t hitQueueBytes = 0;
        uint64_t shadowQueueBytes = 0;
        uint64_t pixelStateBytes = 0;
        uint64_t totalBytes = 0;
        uint64_t transientArenaUsedBytes = 0;
        uint64_t transientArenaHighWaterBytes = 0;
        uint64_t transientArenaCapacityBytes = 0;
    } wavefrontQueues{};

    struct WavefrontValidationReport {
        bool enabled = false;
        std::string mode;
        bool allRequiredPassed = false;
        bool primaryGenerationPassed = false;
        bool tracePassed = false;
        bool shadePassed = false;
        bool compactPassed = false;
        bool secondaryShadePassed = false;
        bool shadowTracePassed = false;
        bool directLightingParityPassed = false;
        bool queueBalancePassed = false;
        uint32_t checkedPixels = 0;
        uint32_t checkedSecondaryRays = 0;
        uint32_t checkedShadowRays = 0;
        uint32_t directLightingMismatchCount = 0;
        float directLightingMaxAbsError = 0.0f;
        float directLightingMaxRelativeError = 0.0f;
        float wavefrontProbeGpuMs = 0.0f;
        float secondaryShadeGpuMs = 0.0f;
        float shadowTraceGpuMs = 0.0f;
    } wavefrontValidation{};

    struct MemoryReport {
        uint64_t texturesBytes = 0;
        uint64_t buffersBytes = 0;
        uint64_t accelerationStructureBytes = 0;
        uint64_t hardwareAccelerationStructureBytes = 0;
        uint64_t gpuSceneBufferBytes = 0;
        uint64_t gpuSceneGeometryBytes = 0;
        uint64_t gpuSceneSoftwareBvhBytes = 0;
        uint64_t gpuSceneLightBytes = 0;
        uint64_t gpuSceneParameterBytes = 0;
        uint64_t temporalHistoryBytes = 0;
        uint64_t restirReservoirBytes = 0;
        uint64_t restirDiCurrentBytes = 0;
        uint64_t restirDiInitialBytes = 0;
        uint64_t restirDiTemporalBytes = 0;
        uint64_t restirDiPreviousBytes = 0;
        uint64_t restirDiSpatialBytes = 0;
        uint64_t restirDiFinalBytes = 0;
        uint64_t restirDiReceiverBytes = 0;
        uint64_t restirDiPreviousReceiverBytes = 0;
        uint64_t restirDiCountersBytes = 0;
        uint64_t restirDiPhysicalBytes = 0;
        uint64_t restirDiAliasSavingsBytes = 0;
        uint64_t restirDiReservoirPixelCount = 0;
        bool restirDiCheckerboardCompact = false;
        uint64_t restirGiCurrentBytes = 0;
        uint64_t restirGiPreviousBytes = 0;
        uint64_t restirGiSpatialBytes = 0;
        uint64_t restirGiProductionTemporalBytes = 0;
        uint64_t restirGiProductionSpatialBytes = 0;
        uint64_t restirGiProductionPreviousBytes = 0;
        uint64_t restirGiProductionUpsampledBytes = 0;
        uint64_t restirGiActiveTileMaskBytes = 0;
        uint64_t restirGiCountersBytes = 0;
        uint64_t restirGiReceiverBytes = 0;
        uint64_t restirGiPreviousReceiverBytes = 0;
        bool restirGiEffectiveHalfResolution = false;
        uint64_t restirPtInitialBytes = 0;
        uint64_t restirPtTemporalBytes = 0;
        uint64_t restirPtCurrentBytes = 0;
        uint64_t restirPtPreviousBytes = 0;
        uint64_t stagingUploadTotalBytes = 0;
        uint64_t stagingUploadPeakBytes = 0;
        uint64_t stagingUploadLastBytes = 0;
        uint32_t stagingUploadCount = 0;
        uint32_t stagingBufferUploadCount = 0;
        uint32_t stagingImageUploadCount = 0;
        uint32_t stagingBatchUploadCount = 0;

        struct HeapBudget {
            uint32_t heapIndex = 0;
            uint64_t usageBytes = 0;
            uint64_t budgetBytes = 0;
            uint64_t allocationBytes = 0;
            uint64_t blockBytes = 0;
            uint32_t allocationCount = 0;
            uint32_t blockCount = 0;
            float usageRatio = 0.0f;
            std::string pressure;
        };

        struct VmaBudgetReport {
            bool supported = false;
            uint64_t totalUsageBytes = 0;
            uint64_t totalBudgetBytes = 0;
            uint64_t totalAllocationBytes = 0;
            uint64_t totalBlockBytes = 0;
            uint64_t peakUsageBytes = 0;
            int64_t usageDeltaBytes = 0;
            uint32_t allocationCount = 0;
            uint32_t blockCount = 0;
            float maxUsageRatio = 0.0f;
            std::string pressure;
            bool overrideActive = false;
            std::vector<HeapBudget> heaps;
            std::vector<std::string> warnings;
        } vmaBudget{};

        struct DescriptorPoolReport {
            uint32_t setsPerPool = 0;
            uint32_t maxPools = 0;
            uint32_t usedPools = 0;
            uint32_t freePools = 0;
            uint32_t poolCount = 0;
            uint32_t capacitySets = 0;
            uint32_t allocatedSets = 0;
            uint32_t peakAllocatedSets = 0;
            uint32_t failedAllocations = 0;
            uint32_t fragmentedPoolFailures = 0;
            uint32_t poolGrowthCount = 0;
        } descriptors{};

        struct BindlessTextureHeapReport {
            bool initialized = false;
            uint32_t capacity = 0;
            uint32_t descriptorCount = 0;
            uint32_t patchCount = 0;
        } bindlessTextureHeap{};

        struct UiReport {
            bool present = false;
            uint32_t descriptorMaxSets = 0;
            uint32_t combinedImageSamplerDescriptors = 0;
            uint32_t sampledImageDescriptors = 0;
            uint32_t samplerDescriptors = 0;
            uint32_t viewportDescriptorAllocated = 0;
        } ui{};
    } memory{};

    struct AdaptiveQualityReport {
        float smoothedGpuMs = 0.0f;
        uint32_t tier = 0;
        uint32_t overBudgetFrames = 0;
        uint32_t effectiveMaxBounces = 0;
        uint32_t effectiveEnvironmentSamples = 0;
        uint32_t effectiveAtrousIterations = 0;
        bool skipRestirSpatial = false;
        bool skipDenoiser = false;
    } adaptiveQuality{};

    struct MemoryPressureQualityReport {
        bool active = false;
        bool overrideActive = false;
        uint32_t tier = 0;
        float usageRatio = 0.0f;
        std::string pressure = "normal";
        float effectiveRenderScale = 1.0f;
        bool limitSamplesPerPixel = false;
        bool restirGiHalfResolution = false;
        uint32_t denoiserMaxHistoryLength = 0;
    } memoryPressureQuality{};

    struct AdaptiveSamplingReport {
        bool active = false;
        bool statsValid = false;
        float requestedBudgetSpp = 0.0f;
        float averageDensity = 0.0f;
        float desiredSamplesPerPixel = 0.0f;
        float actualSamplesPerPixel = 0.0f;
        float budgetErrorPercent = 0.0f;
        uint32_t pixelCount = 0;
        uint32_t actualSampleCount = 0;
    } adaptiveSampling{};

    struct RegirGridReport {
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
        uint32_t environmentGeneration = 0;
        uint32_t lightGeneration = 0;
        uint64_t environmentBankBytes = 0;
        bool environmentEffective = false;
        bool sunEffective = false;
        bool temporalHistoryValid = false;
    } regirGrid{};

    struct NvidiaIntegrationReport {
        struct StreamlineFeatureReport {
            bool requestable = false;
            bool supported = false;
            std::string unavailableReason;
            std::string requirements;
        };

        struct StreamlineTagReport {
            uint32_t expected = 0;
            uint32_t tagged = 0;
            uint32_t failed = 0;
            uint32_t missingFrameToken = 0;
            uint32_t missingImage = 0;
            uint32_t invalidLayout = 0;
            uint32_t invalidFormat = 0;
            uint32_t invalidExtent = 0;
            uint32_t emptyRole = 0;
            uint32_t runtimeRejected = 0;
            uint64_t frameIndex = 0;
            bool attempted = false;
        };

        struct StreamlineEvaluationReport {
            uint32_t attempted = 0;
            uint32_t succeeded = 0;
            uint32_t failed = 0;
            uint32_t skippedUnsupported = 0;
            uint32_t skippedMissingTags = 0;
            uint64_t frameIndex = 0;
            std::string lastError;
        };

        struct StreamlineReflexMarkerReport {
            uint32_t attempted = 0;
            uint32_t succeeded = 0;
            uint32_t failed = 0;
            uint32_t skippedUnavailable = 0;
            uint64_t frameIndex = 0;
        };

        struct GpuCrashDumpReport {
            bool buildAvailable = false;
            bool requested = false;
            bool enabled = false;
            std::filesystem::path outputDirectory;
            std::string unavailableReason;
            uint32_t enableResult = 0;
            uint32_t dumpCount = 0;
            uint32_t shaderDebugInfoCount = 0;
        };

        bool nrdSdkConfigured = false;
        bool nrdRequestable = false;
        bool nrdAvailable = false;
        std::string nrdUnavailableReason;
        bool nrdDirectRuntimeResourcesReady = false;
        bool nrdHistoryConfidenceInputsAllocated = false;
        bool nrdHistoryConfidenceAvailable = false;
        bool nrdValidationOutputAllocated = false;
        bool nrdValidationOutputEnabled = false;
        std::string nrdGuideContractReason;
        std::string nrdBackendPolicy = "disabled";
        std::string nrdBackendPolicyReason;
        bool nrdBackendsMutuallyExclusive = true;
        std::string requestedDenoiserBackend = "engine";
        std::string effectiveDenoiserBackend = "engine";
        bool dlssSdkConfigured = false;
        bool dlssAvailable = false;
        std::string dlssUnavailableReason;
        bool dlssRayReconstructionAvailable = false;
        std::string dlssRayReconstructionUnavailableReason;
        bool requestedDlssRayReconstruction = false;
        bool effectiveDlssRayReconstruction = false;
        bool dlssRayReconstructionGuidePassReady = false;
        bool dlssRayReconstructionGuideImagesAllocated = false;
        bool dlssRayReconstructionPsrGuideBufferAllocated = false;
        bool dlssRayReconstructionPsrHistorySignaturesAllocated = false;
        bool dlssRayReconstructionUsesPsrGuides = false;
        uint32_t dlssRayReconstructionGuideImageCount = 0;
        std::string dlssRayReconstructionGuideMode;
        bool dlssFrameGenerationAvailable = false;
        std::string dlssFrameGenerationUnavailableReason;
        bool requestedDlssFrameGeneration = false;
        bool effectiveDlssFrameGeneration = false;
        bool dlssAutoExposureEnabled = false;
        bool dlssExposureBufferAvailable = false;
        bool dlssExposureBufferPassedToSdk = false;
        float dlssManualExposure = 1.0f;
        float dlssPreExposure = 1.0f;
        float dlssExposureScale = 1.0f;
        float dlssSharpeningStrength = 0.0f;
        std::string requestedTemporalUpscaler = "taa-tsr";
        std::string effectiveTemporalUpscaler = "taa-tsr";
        bool streamlineSdkConfigured = false;
        bool streamlineRuntimeConfigured = false;
        bool streamlineInitialized = false;
        bool streamlineVulkanInfoSet = false;
        bool vulkanDebugUtilsEnabled = false;
        bool vulkanDebugLabelsAvailable = false;
        bool vulkanDebugObjectNamesAvailable = false;
        std::string streamlineRuntimeDirectory;
        std::string streamlineUnavailableReason;
        std::vector<std::string> streamlineLogMessages;
        bool requestedStreamlineReflex = false;
        bool effectiveStreamlineReflex = false;
        bool requestedStreamlineNvPerf = false;
        bool effectiveStreamlineNvPerf = false;
        StreamlineFeatureReport streamlineDlss;
        StreamlineFeatureReport streamlineDlssRayReconstruction;
        StreamlineFeatureReport streamlineDlssFrameGeneration;
        StreamlineFeatureReport streamlineReflex;
        StreamlineFeatureReport streamlineNis;
        StreamlineFeatureReport streamlineNrd;
        StreamlineFeatureReport streamlineNvPerf;
        StreamlineTagReport streamlineDlssTags;
        StreamlineTagReport streamlineDlssRayReconstructionTags;
        StreamlineEvaluationReport streamlineDlssEvaluation;
        StreamlineEvaluationReport streamlineDlssRayReconstructionEvaluation;
        StreamlineEvaluationReport ngxDlssRayReconstructionEvaluation;
        StreamlineEvaluationReport streamlineNvPerfEvaluation;
        StreamlineReflexMarkerReport streamlineReflexMarkers;
        GpuCrashDumpReport gpuCrashDumps;
        NsightPerfDiagnosticsReport nsightPerfSdk;
    } nvidiaIntegrations{};

    struct SceneUpdateRouteReport {
        std::string kind;
        std::string action;
        uint64_t count = 0;
        double totalCpuMs = 0.0;
        double lastCpuMs = 0.0;
        double averageCpuMs = 0.0;
        double minCpuMs = 0.0;
        double maxCpuMs = 0.0;
    };
    std::vector<SceneUpdateRouteReport> sceneUpdateRoutes;

    struct SchedulerQueueReport {
        std::string queue;
        std::string job;
        std::string status;
        uint64_t generation = 0;
        uint64_t count = 0;
        double totalCpuMs = 0.0;
        double lastCpuMs = 0.0;
        double averageCpuMs = 0.0;
        double minCpuMs = 0.0;
        double maxCpuMs = 0.0;
        uint64_t frameBudgetViolationCount = 0;
        uint64_t submittedBytes = 0;
        uint64_t completedBytes = 0;
    };
    std::vector<SchedulerQueueReport> schedulerQueues;

    std::vector<GpuUploadTicketSnapshot> gpuUploadTickets;
    uint64_t gpuUploadNextTimelineValue = 0;
    std::vector<MainThreadApplyTicketSnapshot> mainThreadApplyTickets;
    std::vector<TopologyRebuildTicketSnapshot> topologyRebuildTickets;
    uint64_t topologyRebuildLatestGeneration = 0;
    uint64_t topologyRebuildNextTimelineValue = 0;

    bool validationEnabled = false;
    uint32_t validationErrorCount = 0;
    std::vector<std::string> warnings;
    nlohmann::json textureDiagnostics = nlohmann::json::object();
    nlohmann::json optimizationHints = nlohmann::json::array();
    nlohmann::json diagnosticReadiness = nlohmann::json::object();
    nlohmann::json nsightAnalysisPlan = nlohmann::json::object();
    nlohmann::json rayTracingShaderMap = nlohmann::json::object();
    nlohmann::json accelerationStructureDiagnostics = nlohmann::json::object();
    nlohmann::json barrierSyncDiagnostics = nlohmann::json::object();

    RendererSettings settings{};
};

struct ValidationSceneResult {
    std::string name;
    std::string status;
    float gpuMsTotal = 0.0f;
    bool validationEnabled = false;
    uint32_t validationErrors = 0;
    uint32_t framesRendered = 0;
    bool wavefrontValidationEnabled = false;
    bool wavefrontValidationPassed = false;
    uint32_t wavefrontCheckedPixels = 0;
    uint32_t wavefrontCheckedSecondaryRays = 0;
    uint32_t wavefrontCheckedShadowRays = 0;
    uint32_t wavefrontDirectLightingMismatches = 0;
    float wavefrontProbeGpuMs = 0.0f;
};

struct ValidationSuiteSummary {
    std::vector<ValidationSceneResult> scenes;
    uint32_t totalPass = 0;
    uint32_t totalFail = 0;
    uint32_t wavefrontValidationPass = 0;
    uint32_t wavefrontValidationFail = 0;
};

class HeadlessDiagnostics {
public:
    explicit HeadlessDiagnostics(const HeadlessDiagnosticsConfig& config);
    ~HeadlessDiagnostics();

    ProfileReport run(Application& app);
    void writeProfileJson(const std::filesystem::path& path) const;
    void writeRenderGraphJson(const std::filesystem::path& path);
    void exportDebugViews(Application& app, const std::filesystem::path& dir);
    void exportFrameSequence(Application& app, const std::filesystem::path& dir);
    void makeDebugPackage(Application& app, const std::filesystem::path& dir, const std::filesystem::path& scenePath);
    ValidationSuiteSummary runValidationSuite();

    void captureStdout();
    std::string releaseStdout();

    [[nodiscard]] const ProfileReport& profileReport() const { return profileReport_; }
    [[nodiscard]] ProfileReport& profileReport() { return profileReport_; }

    [[nodiscard]] const std::filesystem::path& profileJsonPath() const { return profileJsonPath_; }

private:
    void collectValidationLog(Application& app);

    HeadlessDiagnosticsConfig config_;
    ProfileReport profileReport_;
    std::filesystem::path profileJsonPath_;
    std::unique_ptr<std::ostringstream> logCapture_;
    std::streambuf* oldCout_ = nullptr;
    std::streambuf* oldCerr_ = nullptr;
};

} // namespace rtv
