#pragma once

#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"

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
    uint32_t warmupFrames = 0;
    uint32_t totalFrames = 120;
    std::optional<uint32_t> fixedSeed;
    bool profile = false;
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
        float restirSpatial = 0.0f;
        float restirSpatialCopy = 0.0f;
        float restirGiSpatial = 0.0f;
        float restirGiFinal = 0.0f;
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
        float historyCopy = 0.0f;
        float skipDenoiserCopy = 0.0f;
        float taa = 0.0f;
        float taaHistoryCopy = 0.0f;
        float autoExposureHistogramClear = 0.0f;
        float autoExposureHistogram = 0.0f;
        float autoExposureReduce = 0.0f;
        float toneMap = 0.0f;
        float selectionOutline = 0.0f;
        float fullscreen = 0.0f;
        float editorPresentation = 0.0f;
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
    } pipelineStatistics{};

    struct RayTracingDiagnosticCounterReport {
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
    } rayTracingDiagnosticCounters{};

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
    } rayTracingGeometry{};

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
        uint64_t temporalHistoryBytes = 0;
        uint64_t restirReservoirBytes = 0;
        uint64_t restirDiCurrentBytes = 0;
        uint64_t restirDiPreviousBytes = 0;
        uint64_t restirDiSpatialBytes = 0;
        uint64_t restirGiCurrentBytes = 0;
        uint64_t restirGiPreviousBytes = 0;
        uint64_t restirGiSpatialBytes = 0;
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

    uint32_t validationErrorCount = 0;
    std::vector<std::string> warnings;

    RendererSettings settings{};
};

struct ValidationSceneResult {
    std::string name;
    std::string status;
    float gpuMsTotal = 0.0f;
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
