#pragma once

#include "rtv/FrameResources.h"
#include "rtv/GpuProfiler.h"
#include "rtv/GpuValidation.h"
#include "rtv/GpuScene.h"
#include "rtv/Image.h"
#include "rtv/PhysicalCamera.h"
#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"
#include "rtv/RayTracingScene.h"
#include "rtv/StreamlineRuntime.h"

#include <Volk/volk.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace rtv {

class BufferUploader;
class ComputePipeline;
class DescriptorLayoutCache;
class DescriptorSet;
class DescriptorWriter;
class GraphicsPipeline;
class PipelineCache;
class RayTracingPipeline;
class RayTracingScene;
class ResourceAllocator;
class ShaderCompiler;
class ShaderModule;
class TemporalSystem;
class VulkanContext;
class AssetManager;
class AtmosphereLutSystem;
class PhysicalCamera;
struct OpacityMicromapDeviceInfo;
struct SerDeviceInfo;
struct RayTracingMotionBlurDeviceInfo;
struct AtmosphereLutStats;
struct SceneAsset;

struct RayTracingRendererStats {
    bool active = false;
    uint32_t blasCount = 0;
    uint32_t instanceCount = 0;
    VkDeviceSize accelerationStructureBytes = 0;
    VkDeviceSize sbtBytes = 0;
    float lastTlasRefitMs = 0.0f;
    uint32_t dynamicBlasUpdateCount = 0;
    float lastDynamicBlasUpdateRecordMs = 0.0f;
    RayTracingGeometryStats geometry{};
    RayTracingBlasGeometryStats blasGeometry{};
    RayTracingMotionInstanceStats motionInstances{};
    OpacityMicromapPreprocessStats opacityMicromapPreprocess{};
    OpacityMicromapBuildStats opacityMicromapBuild{};
};

struct RayTracingDiagnosticCounters {
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
};

enum class AccumulationResetReason : uint32_t {
    Startup,
    Resize,
    CameraMoved,
    Manual,
    RenderSettingsChanged,
    LightingChanged,
    EnvironmentChanged,
    DenoiserSettingsChanged,
    DebugViewChanged,
    SceneChanged,
    MaterialChanged,
    ShaderReloaded,
};

[[nodiscard]] const char* accumulationResetReasonName(AccumulationResetReason reason);

class PathTracerRenderer {
public:
    struct alignas(16) GpuSkinningSourceVertex {
        glm::vec4 positionUvX{};
        glm::vec4 normalUvY{};
        glm::vec4 tangent{};
        glm::vec4 color{1.0f};
        glm::vec4 texcoord1{};
        glm::uvec4 joints{};
        glm::vec4 weights{};
    };

    struct GpuSkinningDispatchRecord {
        uint32_t meshHandleIndex = 0xffffffffu;
        uint32_t vertexCount = 0;
        uint32_t jointMatrixOffset = 0;
        uint32_t sourceVertexOffset = 0;
        uint32_t currentVertexOffset = 0;
        uint32_t previousVertexOffset = 0;
        uint32_t morphDeltaOffset = 0;
        uint32_t morphDeltaCount = 0;
        float morphWeight = 0.0f;
        bool morphBeforeSkinning = false;
    };

    struct GpuSkinningResourcePlan {
        uint32_t candidateInstanceCount = 0;
        uint32_t dispatchRecordCount = 0;
        uint32_t jointMatrixCount = 0;
        uint64_t jointUploadBytes = 0;
        uint64_t previousJointUploadBytes = 0;
        uint64_t sourceVertexUploadBytes = 0;
        uint64_t morphDeltaUploadBytes = 0;
        uint64_t currentVertexBufferBytes = 0;
        uint64_t previousVertexBufferBytes = 0;
        std::vector<GpuSkinningDispatchRecord> dispatchRecords;
        std::vector<glm::mat4> jointMatrices;
        std::vector<glm::mat4> previousJointMatrices;
        std::vector<GpuSkinningSourceVertex> sourceVertices;
        std::vector<GpuLocalVertex> morphDeltas;
        bool cpuFallbackActive = false;
        bool rendererBuffersAllocated = false;
        bool sourceVertexBufferUploaded = false;
        bool morphDeltaBufferUploaded = false;
        bool jointBufferUploaded = false;
        bool previousJointBufferUploaded = false;
        bool jointPayloadRefreshUploaded = false;
        bool jointPayloadRefreshStaged = false;
        bool jointPayloadRefreshCopyPending = false;
        bool jointPayloadRefreshCopyRecorded = false;
        uint32_t jointPayloadRefreshCount = 0;
        uint32_t jointPayloadRefreshCopyRecordCount = 0;
        uint64_t jointPayloadRefreshBytes = 0;
        uint64_t jointPayloadRefreshStagedBytes = 0;
        uint64_t jointPayloadRefreshCopyBytes = 0;
        bool computePipelineCreated = false;
        bool descriptorsBound = false;
        bool computeDispatchEnabled = false;
        uint32_t computeDispatchRecordCount = 0;
        uint32_t computeMorphDispatchRecordCount = 0;
        bool outputReadbackBufferAllocated = false;
        bool outputReadbackCopyRecorded = false;
        bool outputReadbackValidationPassed = false;
        uint32_t outputReadbackVertexCount = 0;
        float outputReadbackMaxPositionError = 0.0f;
        bool initialComputeDispatchSubmitted = false;
        bool rayTracingGeometryInputEnabled = false;
        uint32_t rayTracingGeometryInputMeshCount = 0;
        uint32_t rayTracingGeometryInputGeometryCount = 0;
        bool rayTracingDescriptorInputEnabled = false;
        bool rayTracingDescriptorInputMixedSceneEnabled = false;
        uint32_t rayTracingDescriptorInputMeshCount = 0;
        uint32_t rayTracingDescriptorInputBindingCount = 0;
        bool skinnedMotionVectorsEnabled = false;
        bool skinnedMotionVectorsPreviousVertexInputEnabled = false;
        bool skinnedMotionVectorsDescriptorBound = false;
    };

    struct StreamingResetMaskReport {
        uint64_t generation = 0;
        uint64_t lastFrame = 0;
        uint32_t pendingTemporalEntityCount = 0;
        uint32_t pendingRestirEntityCount = 0;
        uint32_t pendingDenoiserEntityCount = 0;
        uint32_t totalTemporalEntityCount = 0;
        uint32_t totalRestirEntityCount = 0;
        uint32_t totalDenoiserEntityCount = 0;
        uint64_t denoiserHistoryInvalidationGeneration = 0;
        bool denoiserHistoryInvalidated = false;
        uint32_t gpuRecordCount = 0;
        uint32_t gpuRecordCapacity = 0;
        uint64_t gpuBufferBytes = 0;
        bool gpuBufferAllocated = false;
        uint32_t gpuInstanceMaskCount = 0;
        uint32_t gpuInstanceMaskCapacity = 0;
        uint64_t gpuInstanceMaskBufferBytes = 0;
        bool gpuInstanceMaskBufferAllocated = false;
        std::vector<uint64_t> temporalEntityUuids;
        std::vector<uint64_t> restirEntityUuids;
        std::vector<uint32_t> temporalInstanceIndices;
        std::vector<uint32_t> restirInstanceIndices;
    };

    struct alignas(16) StreamingResetMaskGpuRecord {
        uint64_t entityUuid = 0;
        uint32_t flags = 0;
        uint32_t generation = 0;
    };

    PathTracerRenderer(
        const VulkanContext& context,
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        VkFormat swapchainFormat,
        const std::filesystem::path& shaderDirectory,
        const std::filesystem::path& shaderOutputDirectory,
        RendererDebugView debugView = RendererDebugView::Beauty,
        const SceneAsset* importedScene = nullptr,
        const AssetManager* assets = nullptr,
        std::optional<std::filesystem::path> environmentPath = std::nullopt,
        std::optional<std::filesystem::path> sceneCachePath = std::nullopt,
        bool resourceAliasingEnabled = true,
        GpuSkinningResourcePlan gpuSkinningResourcePlan = {},
        const RendererSettings* initialSettings = nullptr,
        uint32_t materialTextureMaxDimension = 0);
    ~PathTracerRenderer();
    void releaseExclusiveRuntimeForRendererReplacement();

    void beginFrame(uint32_t frameIndex, VkExtent2D renderExtent, VkExtent2D displayExtent);
    void setFrameDeltaSeconds(float deltaSeconds) { frameDeltaSeconds_ = deltaSeconds; }
    void recordPathTrace(VkCommandBuffer commandBuffer, bool deferPostTraceCompute = false);
    [[nodiscard]] bool recordAsyncComputeWork(VkCommandBuffer commandBuffer);
    void recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent);
    void markStreamlineReflexSimulationStart();
    void markStreamlineReflexSimulationEnd();
    void markStreamlineReflexRenderSubmitStart();
    void markStreamlineReflexRenderSubmitEnd();
    void markStreamlineReflexPresentStart();
    void markStreamlineReflexPresentEnd();
    void recordEditorPresentationStart(VkCommandBuffer commandBuffer);
    void recordEditorPresentationEnd(VkCommandBuffer commandBuffer);

    bool applySettings(const RendererSettings& settings);
    void setCameraPose(glm::vec3 position, glm::vec3 forward);
    void setCameraFovY(float fovY);
    void setCameraProjection(
        uint32_t projection,
        float fovY,
        float aspectRatio,
        float orthographicXmag,
        float orthographicYmag,
        float nearPlane,
        float farPlane);
    void resetAccumulation(AccumulationResetReason reason = AccumulationResetReason::Manual);
    void applyStreamingResetMasks(
        const std::vector<uint64_t>& temporalEntityUuids,
        const std::vector<uint64_t>& restirEntityUuids,
        const std::vector<uint32_t>& temporalInstanceIndices = {},
        const std::vector<uint32_t>& restirInstanceIndices = {});
    void loadEnvironment(const std::filesystem::path& path);
    [[nodiscard]] bool shadersNeedReload();
    bool updateMaterials(const SceneAsset& scene, const AssetManager& assets);
    bool updateSceneLights(const SceneAsset& scene, bool rebuildLightBvh = true);
    bool updateSceneTransforms(const SceneAsset& scene, const AssetManager& assets);
    bool updateSceneVisibility(const SceneAsset& scene, const AssetManager& assets);
    bool updateGpuSkinningJointPayloads(
        const std::vector<glm::mat4>& jointMatrices,
        const std::vector<glm::mat4>& previousJointMatrices);
    void setSelectedInstanceId(std::optional<uint32_t> instanceId);
    void requestPickInstanceId(glm::vec2 viewportUv);
    [[nodiscard]] std::optional<uint32_t> consumePickedInstanceId();
    [[nodiscard]] bool pickPending() const;

    [[nodiscard]] const RendererSettings& settings() const { return settings_; }
    [[nodiscard]] const StreamingResetMaskReport& streamingResetMaskReport() const { return streamingResetMaskReport_; }
    [[nodiscard]] const GpuSkinningResourcePlan& gpuSkinningResourcePlan() const { return gpuSkinningResourcePlan_; }
    void updateGpuSkinningOutputReadbackValidation();
    [[nodiscard]] const OpacityMicromapDeviceInfo& opacityMicromapInfo() const;
    [[nodiscard]] const SerDeviceInfo& serInfo() const;
    [[nodiscard]] const RayTracingMotionBlurDeviceInfo& rayTracingMotionBlurInfo() const;
    struct StreamlineTagSummary {
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
    struct StreamlineEvaluationSummary {
        uint32_t attempted = 0;
        uint32_t succeeded = 0;
        uint32_t failed = 0;
        uint32_t skippedUnsupported = 0;
        uint32_t skippedMissingTags = 0;
        uint64_t frameIndex = 0;
    };
    struct StreamlineReflexMarkerSummary {
        uint32_t attempted = 0;
        uint32_t succeeded = 0;
        uint32_t failed = 0;
        uint32_t skippedUnavailable = 0;
        uint64_t frameIndex = 0;
    };
    struct NvidiaIntegrationStatus {
        bool nrdSdkConfigured = false;
        bool nrdRequestable = false;
        bool nrdAvailable = false;
        std::string nrdUnavailableReason;
        bool dlssSdkConfigured = false;
        bool dlssRequestable = false;
        bool dlssAvailable = false;
        std::string dlssUnavailableReason;
        bool dlssRayReconstructionRequestable = false;
        bool dlssRayReconstructionAvailable = false;
        std::string dlssRayReconstructionUnavailableReason;
        bool dlssFrameGenerationRequestable = false;
        bool dlssFrameGenerationAvailable = false;
        std::string dlssFrameGenerationUnavailableReason;
        bool streamlineSdkConfigured = false;
        bool streamlineRuntimeConfigured = false;
        bool streamlineInitialized = false;
        bool streamlineVulkanInfoSet = false;
        std::string streamlineRuntimeDirectory;
        std::string streamlineUnavailableReason;
        StreamlineFeatureStatus streamlineDlss;
        StreamlineFeatureStatus streamlineDlssRayReconstruction;
        StreamlineFeatureStatus streamlineDlssFrameGeneration;
        StreamlineFeatureStatus streamlineReflex;
        StreamlineFeatureStatus streamlineNis;
        StreamlineFeatureStatus streamlineNrd;
        StreamlineTagSummary streamlineDlssTags;
        StreamlineTagSummary streamlineDlssRayReconstructionTags;
        StreamlineEvaluationSummary streamlineDlssEvaluation;
        StreamlineEvaluationSummary streamlineDlssRayReconstructionEvaluation;
        StreamlineReflexMarkerSummary streamlineReflexMarkers;
    };
    struct NrdRuntime;
    [[nodiscard]] NvidiaIntegrationStatus nvidiaIntegrationStatus() const;
    [[nodiscard]] DenoiserBackend effectiveDenoiserBackend() const;
    [[nodiscard]] TemporalUpscaler effectiveTemporalUpscaler() const;
    [[nodiscard]] bool dlssRayReconstructionActive() const;
    void refreshMemoryPressureQuality();
    [[nodiscard]] float effectiveRenderResolutionScale() const;
    [[nodiscard]] bool hardwareRayTracingAvailable() const;
    [[nodiscard]] RayTracingRendererStats rayTracingStats() const;
    [[nodiscard]] RayTracingDiagnosticCounters rayTracingDiagnosticCounters() const;
    [[nodiscard]] uint32_t effectiveSamplesPerPixel() const {
        return effectiveLimitSamplesPerPixel() ? 1u : std::max(1u, settings_.samplesPerPixel);
    }
    [[nodiscard]] uint32_t sampleCount() const { return frameCount_ * effectiveSamplesPerPixel(); }
    [[nodiscard]] const GpuFrameTimings& timings() const;
    [[nodiscard]] GpuPipelineStatistics pipelineStats() const;
    [[nodiscard]] AccumulationResetReason lastAccumulationResetReason() const { return lastResetReason_; }
    [[nodiscard]] const RendererValidationLog& validationLog() const { return validationLog_; }
    [[nodiscard]] RendererValidationLog& validationLog() { return validationLog_; }
    struct WavefrontQueueStats {
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
        bool sortedShadeEnabled = false;
        bool sortedShadeValidationPassed = false;
        uint32_t sortedShadeCheckedRays = 0;
        uint32_t sortedShadeHitCount = 0;
        uint32_t sortedShadeMissCount = 0;
        uint32_t sortedShadeTerminatedCount = 0;
        uint32_t sortedShadeShadowRayCount = 0;
        uint32_t sortedShadeSecondaryRayCount = 0;
        uint32_t sortedShadeMaterialCount = 0;
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
    };
    [[nodiscard]] WavefrontQueueStats wavefrontQueueStats() const;
    struct AdaptiveQualityState {
        float smoothedGpuMs = 0.0f;
        uint32_t tier = 0;
        uint32_t overBudgetFrames = 0;
        uint32_t effectiveMaxBounces = 0;
        uint32_t effectiveEnvironmentSamples = 0;
        uint32_t effectiveAtrousIterations = 0;
        bool skipRestirSpatial = false;
        bool skipDenoiser = false;
    };
    [[nodiscard]] AdaptiveQualityState adaptiveQualityState() const;
    struct MemoryPressureQualityState {
        bool active = false;
        bool overrideActive = false;
        uint32_t tier = 0;
        float usageRatio = 0.0f;
        std::string pressure = "normal";
        float effectiveRenderScale = 1.0f;
        bool limitSamplesPerPixel = false;
        bool restirGiHalfResolution = false;
        uint32_t denoiserMaxHistoryLength = 0;
    };
    [[nodiscard]] MemoryPressureQualityState memoryPressureQualityState() const;
    [[nodiscard]] const TemporalSystem* temporalSystem() const { return temporalSystem_.get(); }
    [[nodiscard]] AtmosphereLutStats atmosphereLutStats() const;
    [[nodiscard]] const GpuScene& scene() const { return scene_; }
    [[nodiscard]] VkDescriptorImageInfo viewportImageDescriptor() const;
    [[nodiscard]] VkImage presentationImage() const { return presentationImage_.handle(); }
    [[nodiscard]] VkExtent2D renderExtent() const { return renderExtent_; }
    [[nodiscard]] VkExtent2D displayExtent() const { return displayExtent_; }

    [[nodiscard]] VkDeviceSize estimatedTextureMemory() const;
    [[nodiscard]] VkDeviceSize estimatedBufferMemory() const;
    [[nodiscard]] VkDeviceSize temporalHistoryMemory() const;
    [[nodiscard]] VkDeviceSize restirReservoirMemory() const;
    [[nodiscard]] const char* restirGiReservoirLayoutName() const;
    struct RestirReservoirMemoryBreakdown {
        VkDeviceSize diCurrentBytes = 0;
        VkDeviceSize diPreviousBytes = 0;
        VkDeviceSize diSpatialBytes = 0;
        VkDeviceSize giCurrentBytes = 0;
        VkDeviceSize giPreviousBytes = 0;
        VkDeviceSize giSpatialBytes = 0;
    };
    [[nodiscard]] RestirReservoirMemoryBreakdown restirReservoirMemoryBreakdown() const;
    [[nodiscard]] DescriptorAllocator::Stats descriptorPoolStats() const;

    void setDumpRenderGraphPath(std::optional<std::filesystem::path> path) { dumpRenderGraphPath_ = std::move(path); }
    void setDumpRenderGraphDotPath(std::optional<std::filesystem::path> path) { dumpRenderGraphDotPath_ = std::move(path); }
    void setRayTracingDiagnosticCountersEnabled(bool enabled);

private:
    struct DenoiserParams {
        uint32_t enabled = 1;
        float strength = 1.0f;
        uint32_t frameCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t atrousIterations = 4;
        uint32_t debugView = 0;
        uint32_t resetHistory = 1;
        uint32_t framesSinceReset = 0;
    };

    struct MomentParams {
        uint32_t resetHistory = 1;
        uint32_t framesSinceReset = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t maxHistoryLength = 64;
        float adaptiveAlphaFloor = 0.02f;
        float adaptiveAlphaCeiling = 0.50f;
        float varianceScale = 4.0f;
        float validityThreshold = 0.25f;
    };

    struct PrevCameraUniform {
        glm::mat4 viewProj{1.0f};
        glm::mat4 invViewProj{1.0f};
        glm::mat4 prevViewProj{1.0f};
        glm::vec4 currentPos{};
        glm::vec4 prevPos{};
        glm::vec4 jitter{}; // xy = current subpixel jitter, zw = previous subpixel jitter
    };

    struct ToneMapParams {
        uint32_t toneMapper = static_cast<uint32_t>(ToneMapper::ACES);
        uint32_t debugView = 0;
        uint32_t autoExposureEnabled = 0;
        float exposure = 2.0f;
        float gamma = 2.2f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float brightness = 0.0f;
        float whitePoint = 4.0f;
    };

    struct HistogramParams {
        uint32_t width = 0;
        uint32_t height = 0;
        float minLogLuminance = -10.0f;
        float maxLogLuminance = 10.0f;
    };

    struct ExposureReduceParams {
        uint32_t pixelCount = 0;
        float targetLuminance = 0.18f;
        float minExposure = 0.25f;
        float maxExposure = 64.0f;
        float adaptationSpeed = 2.0f;
        float lowPercentile = 0.05f;
        float highPercentile = 0.95f;
        float targetPercentile = 0.60f;
        float deltaSeconds = 0.0f;
        float minLogLuminance = -10.0f;
        float maxLogLuminance = 10.0f;
    };

    struct SelectionParams {
        uint32_t selectedInstance = UINT32_MAX;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t enabled = 0;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
    };

    struct TaaParams {
        uint32_t enabled = 1;
        uint32_t frameCount = 0;
        uint32_t width = 0; // display width
        uint32_t height = 0; // display height
        float feedback = 0.08f;
        float velocityScale = 512.0f;
        uint32_t resetHistory = 1;
        float sharpeningStrength = 0.08f;
        uint32_t historyValid = 0;
        uint32_t cameraMoving = 0;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
    float motionFeedback = 0.90f;
    float reactiveFeedback = 0.98f;
    };

    struct RestirSpatialParams {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameCount = 0;
        uint32_t enabled = 0;
        uint32_t giSpatialRounds = 4;
        uint32_t giHalfResolution = 0;
        uint32_t giTemporalMaxAge = 32;
        uint32_t giVisibilityRayBudget = 0;
        float giSpatialRadius = 4.25f;
        float giDepthThresholdScale = 1.0f;
        float giSpatialCompatibilityThreshold = 0.0f;
        float rawOutputIsCurrentSample = 0.0f;
        glm::vec4 cameraPosition{};
    };

    struct RestirDiParams {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameIndex = 0;
        uint32_t enabled = 0;
        uint32_t temporalMaxAge = 32;
        uint32_t spatialRounds = 4;
        uint32_t spatialMaxM = 64;
        uint32_t visibilityPolicy = 0;
        float spatialRadius = 3.0f;
        float normalThreshold = 0.85f;
        float depthThreshold = 0.05f;
        float temporalLuminanceLimitFactor = 8.0f;
        float confidenceDecay = 0.96f;
        float lumClampNeighborAvgFactor = 6.0f;
        float lumClampNeighborMaxFactor = 3.0f;
        float fireflyClamp = 8.0f;
        float productionClampLuminance = 0.0f;
        uint32_t useFallbackInitial = 1;
        uint32_t spatialResultValid = 0;
        uint32_t padding0 = 0;
        uint32_t padding1 = 0;
    };

    struct FogParams {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t debugView = 0;
        uint32_t enabled = 0;
        float density = 0.0f;
        float heightFalloff = 5.0f;
        float maxDistance = 10000.0f;
        float padding = 0.0f;
        glm::vec4 color{0.65f, 0.72f, 0.85f, 0.0f};
    };

    struct RestirReservoirGpu {
        // metadata: sample type/signature plus packed pdf, age, visibility, sample count, and temporal weight.
        // The remaining fields store a reusable light sample so DI reuse can re-evaluate at the current receiver.
        glm::uvec4 metadata{};
        glm::vec4 sampleValueConfidence{};
        glm::vec4 samplePositionDistance{};
        glm::vec4 sampleRadianceTarget{};
        glm::vec4 sampleNormalWeight{};
        glm::uvec4 sampleMetadata{};
    };
    static_assert(sizeof(RestirReservoirGpu) == 96);

    // New ReSTIR DI data model (per plan Phase 1)
    struct RestirDiReceiverGpu {
        glm::vec4 worldPositionDepth{};      // xyz=world position, w=hit distance
        glm::vec4 normalRoughness{};         // xyz=shading normal, w=roughness
        glm::vec4 tangentMaterialId{};       // xyz=tangent, w=material ID
        glm::vec4 bitangentInstanceId{};     // xyz=bitangent, w=instance ID
        glm::vec4 viewDirectionHitDist{};    // xyz=view direction from surface, w=hit distance
        glm::uvec4 primitiveMeshFlags{};     // x=primitive ID, y=mesh ID, z=surface flags, w=padding
    };
    static_assert(sizeof(RestirDiReceiverGpu) == 96);

    struct RestirDiReservoirGpu {
        glm::uvec4 sampleMetadata{};          // x=lightID, y=lightKind, z=lightVersion, w=materialID
        glm::uvec4 reservoirMetadata{};       // x=packed age/M/vis/valid, y=packed pdf/prevWeight, z=compatSig, w=rejectFlags
        glm::vec4 samplePositionDistance{};   // xyz=world pos, w=distance
        glm::vec4 sampleDirectionPdf{};       // xyz=sample dir at orig receiver, w=cond PDF
        glm::vec4 sampleRadianceTarget{};     // rgb=radiance, w=target
        glm::vec4 sampleNormalWeightSum{};    // xyz=light normal, w=weight sum
        glm::vec4 contributionConfidence{};   // rgb=contribution (debug), w=confidence
    };
    static_assert(sizeof(RestirDiReservoirGpu) == 112);

    struct RestirGiReservoirGpu {
        glm::vec4 radianceWeightSum{};
        // metadata.x packs sample count, age, flags, and roughness. metadata.y packs octahedral normal.
        // metadata.z packs material id. metadata.w packs hit distance and target pdf as fp16 pairs.
        glm::uvec4 metadata{};
    };
    static_assert(sizeof(RestirGiReservoirGpu) == 32);

    struct RestirGiReservoirUncompressedGpu {
        glm::vec4 hitPositionTargetPdf{};
        glm::vec4 normalRoughness{};
        glm::vec4 radianceWeightSum{};
        glm::vec4 receiverPositionHitDistance{};
        glm::uvec4 metadata{};
    };
    static_assert(sizeof(RestirGiReservoirUncompressedGpu) == 80);

    struct PathDataGpu {
        glm::vec4 directDiffuse{};
        glm::vec4 directSpecular{};
        glm::vec4 indirectDiffuse{};
        glm::vec4 indirectSpecular{};
        glm::vec4 albedoRoughnessHitConfidence{};
        glm::vec4 materialSpecularAlbedo{};
        glm::vec4 denoiserHitDistance{};
        glm::vec4 diffuseRayDirectionHitDistance{};
        glm::vec4 specularRayDirectionHitDistance{};
        glm::vec4 emissiveResidual{};
    };

    struct alignas(16) WavefrontQueueHeaderGpu {
        // counters: x=count, y=capacity, z=read offset, w=write offset.
        glm::uvec4 counters{};
        // metadata: x=max path depth, y=frame index, z=clear validation counter, w=flags.
        glm::uvec4 metadata{};
    };
    static_assert(sizeof(WavefrontQueueHeaderGpu) == 32);

    struct alignas(16) WavefrontRayGpu {
        glm::vec4 originTMin{};
        glm::vec4 directionTMax{};
        glm::uvec4 pixelDepthRngFlags{};
    };
    static_assert(sizeof(WavefrontRayGpu) == 48);

    struct alignas(16) WavefrontHitGpu {
        glm::vec4 positionT{};
        glm::vec4 normalRoughness{};
        glm::vec4 barycentricsHitKind{};
        glm::vec4 geomNormal{};
        glm::vec4 tangent{};
        glm::vec4 vertexColor{1.0f};
        glm::vec4 uv1{};
        glm::uvec4 materialInstancePrimitive{};
        glm::uvec4 pixelDepthFlags{};
    };
    static_assert(sizeof(WavefrontHitGpu) == 144);

    struct alignas(16) WavefrontShadowRayGpu {
        glm::vec4 originTMin{};
        glm::vec4 directionTMax{};
        glm::uvec4 radiancePdfPixelLight{};
    };
    static_assert(sizeof(WavefrontShadowRayGpu) == 48);

    struct alignas(16) WavefrontPixelStateGpu {
        glm::vec4 radiance{};
        glm::vec4 throughput{};
        glm::vec4 directLighting{};
        glm::vec4 indirectLighting{};
        glm::vec4 atmosphereTransmittance{};
        glm::uvec4 rngDepthFlags{};
        glm::uvec4 materialInstancePrimitive{};
    };
    static_assert(sizeof(WavefrontPixelStateGpu) == 112);

    struct WavefrontQueueClearPush {
        uint32_t rayCapacity = 0;
        uint32_t hitCapacity = 0;
        uint32_t shadowCapacity = 0;
        uint32_t pixelCapacity = 0;
        uint32_t maxPathDepth = 0;
        uint32_t frameIndex = 0;
        uint32_t validationValue = 0;
        uint32_t flags = 0;
    };
    static_assert(sizeof(WavefrontQueueClearPush) == 32);

    struct WavefrontPrimaryGeneratePush {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameIndex = 0;
        uint32_t flags = 0;
        uint32_t cameraCut = 0;
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
        uint32_t reserved2 = 0;
    };
    static_assert(sizeof(WavefrontPrimaryGeneratePush) == 32);

    struct alignas(16) WavefrontTraceValidationGpu {
        // counters: x=checked pixels, y=hit mismatch, z=instance mismatch, w=depth mismatch.
        glm::uvec4 counters{};
        // metrics: x=normal mismatch, y=expected pixels, z=hit queue count, w=reserved.
        glm::uvec4 metrics{};
    };
    static_assert(sizeof(WavefrontTraceValidationGpu) == 32);

    struct WavefrontTraceValidationPush {
        uint32_t width = 0;
        uint32_t height = 0;
        float depthEpsilon = 0.005f;
        float normalDotThreshold = 0.999f;
    };
    static_assert(sizeof(WavefrontTraceValidationPush) == 16);

    struct alignas(16) WavefrontShadeValidationGpu {
        // counters: x=checked, y=hit, z=miss, w=terminated.
        glm::uvec4 counters{};
        // metrics: x=shadow rays, y=secondary rays, z=material shaded, w=reserved.
        glm::uvec4 metrics{};
        // restir: x=reservoir writes, y=valid candidates, z=temporal merges, w=invalid candidates.
        glm::uvec4 restir{};
        // restirGi: x=reservoir writes, y=valid candidates, z=temporal merges, w=invalid candidates.
        glm::uvec4 restirGi{};
    };
    static_assert(sizeof(WavefrontShadeValidationGpu) == 64);

    struct alignas(16) WavefrontCompactValidationGpu {
        // counters: x=input source count, y=scanned rays, z=live candidates, w=output count.
        glm::uvec4 counters{};
        // metrics: x=dropped invalid, y=overflow, z=invalid pixel, w=mapping mismatch.
        glm::uvec4 metrics{};
    };
    static_assert(sizeof(WavefrontCompactValidationGpu) == 32);

    static constexpr uint32_t kWavefrontSortBucketCount = 32;

    struct alignas(16) WavefrontSortValidationGpu {
        // counters: x=input source count, y=output count, z=active buckets, w=verified rays.
        glm::uvec4 counters{};
        // metrics: x=overflow, y=invalid pixel, z=order violations, w=bucket count.
        glm::uvec4 metrics{};
        uint32_t bucketCounts[kWavefrontSortBucketCount]{};
        uint32_t bucketOffsets[kWavefrontSortBucketCount]{};
    };
    static_assert(sizeof(WavefrontSortValidationGpu) == 288);

    struct WavefrontShadePush {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameIndex = 0;
        uint32_t maxDepth = 1;
        uint32_t flags = 0;
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
        uint32_t reserved2 = 0;
    };
    static_assert(sizeof(WavefrontShadePush) == 32);
    static constexpr uint32_t kWavefrontShadeFlagSortedInput = 1u << 0u;
    static constexpr uint32_t kWavefrontShadeFlagRestirGiCandidateWrite = 1u << 1u;

    struct WavefrontDebugWritePush {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t view = 0;
        uint32_t maxDepth = 1;
        uint32_t flags = 0;
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
        uint32_t reserved2 = 0;
    };
    static_assert(sizeof(WavefrontDebugWritePush) == 32);
    static constexpr uint32_t kWavefrontDebugWriteFlagFinalOutput = 1u << 0u;

    struct WavefrontCompactPush {
        uint32_t sourceCapacity = 0;
        uint32_t compactCapacity = 0;
        uint32_t pixelCapacity = 0;
        uint32_t mode = 0;
        uint32_t maxPathDepth = 0;
        uint32_t frameIndex = 0;
        uint32_t flags = 0;
        uint32_t reserved0 = 0;
    };
    static_assert(sizeof(WavefrontCompactPush) == 32);

    struct WavefrontSortPush {
        uint32_t sourceCapacity = 0;
        uint32_t sortCapacity = 0;
        uint32_t pixelCapacity = 0;
        uint32_t mode = 0;
        uint32_t bucketCount = 0;
        uint32_t frameIndex = 0;
        uint32_t flags = 0;
        uint32_t reserved0 = 0;
    };
    static_assert(sizeof(WavefrontSortPush) == 32);
    struct alignas(16) WavefrontShadowTraceValidationGpu {
        // counters: x=checked rays, y=visible, z=occluded, w=applied visible lighting.
        glm::uvec4 counters{};
        // metrics: x=direct-light checked pixels, y=direct-light mismatches,
        // z=max absolute error * 1e6, w=max relative error * 1e6.
        glm::uvec4 metrics{};
    };
    static_assert(sizeof(WavefrontShadowTraceValidationGpu) == 32);

    struct WavefrontDirectLightingValidationPush {
        uint32_t width = 0;
        uint32_t height = 0;
        float absoluteEpsilon = 1.0e-3f;
        float relativeEpsilon = 0.05f;
    };
    static_assert(sizeof(WavefrontDirectLightingValidationPush) == 16);

    struct GpuSkinningPush {
        uint32_t vertexCount = 0;
        uint32_t jointOffset = 0;
        uint32_t morphDeltaOffset = 0;
        uint32_t morphDeltaCount = 0;
        float morphWeight = 0.0f;
        uint32_t outputOffset = 0;
        uint32_t previousOutputOffset = 0;
        uint32_t sourceVertexOffset = 0;
        uint32_t flags = 0;
    };
    static_assert(sizeof(GpuSkinningPush) == 36);

    void createResolutionResources(VkExtent2D renderExtent, VkExtent2D displayExtent);
    void retireResolutionResources();
    void releaseRetiredResolutionResources();
    void updateCamera();
    void recordPathTraceGraph(VkCommandBuffer commandBuffer);
    [[nodiscard]] RayTracingSceneBuildOptions makeRayTracingSceneBuildOptions() const;
    [[nodiscard]] bool recordGpuSkinningPass(VkCommandBuffer commandBuffer);
    void bindWavefrontFrameResources();
    void recordPathTracePass(VkCommandBuffer commandBuffer);
    void recordRestirSpatial(VkCommandBuffer commandBuffer);
    void recordRestirSpatialPass(VkCommandBuffer commandBuffer);
    void recordRestirSpatialCopyPass(VkCommandBuffer commandBuffer);
    void recordRestirGiSpatialPass(VkCommandBuffer commandBuffer);
    void recordRestirGiFinalPass(VkCommandBuffer commandBuffer);
    void recordRestirDiTemporal(VkCommandBuffer commandBuffer);
    void recordRestirDiTemporalPass(VkCommandBuffer commandBuffer);
    void recordRestirDiSpatial(VkCommandBuffer commandBuffer);
    void recordRestirDiSpatialPass(VkCommandBuffer commandBuffer);
    void recordRestirDiFinal(VkCommandBuffer commandBuffer);
    void recordRestirDiFinalPass(VkCommandBuffer commandBuffer);
    void recordRestirDiHistoryCopy(VkCommandBuffer commandBuffer);
    void recordRestirDiHistoryCopyPass(VkCommandBuffer commandBuffer);
    void recordRestirDiCountersReadback(VkCommandBuffer commandBuffer);
    void recordHeightFog(VkCommandBuffer commandBuffer);
    void recordHeightFogPass(VkCommandBuffer commandBuffer);
    void recordPostTraceCompute(VkCommandBuffer commandBuffer, bool deferHistoryCopy = false);
    void recordDenoiser(VkCommandBuffer commandBuffer);
    void recordDenoiserPass(VkCommandBuffer commandBuffer);
    [[nodiscard]] bool recordNrdDenoiser(VkCommandBuffer commandBuffer);
    void recordNrdPreparePass(VkCommandBuffer commandBuffer);
    [[nodiscard]] bool recordNrdDispatches(VkCommandBuffer commandBuffer);
    void recordNrdResolvePass(VkCommandBuffer commandBuffer);
    void recordMomentUpdate(VkCommandBuffer commandBuffer);
    void recordMomentUpdatePass(VkCommandBuffer commandBuffer);
    void recordTaa(VkCommandBuffer commandBuffer, bool deferHistoryCopy = false);
    void recordTaaPass(VkCommandBuffer commandBuffer);
    void copyTaaHistory(VkCommandBuffer commandBuffer);
    void recordTaaHistoryCopyPass(VkCommandBuffer commandBuffer);
    void recordDlss(VkCommandBuffer commandBuffer);
    void recordDlssGuidesPass(VkCommandBuffer commandBuffer);
    void recordDlssPass(VkCommandBuffer commandBuffer);
    void recordDlssRayReconstruction(VkCommandBuffer commandBuffer);
    void recordDlssRayReconstructionGuidesPass(VkCommandBuffer commandBuffer);
    void recordDlssRayReconstructionPass(VkCommandBuffer commandBuffer);
    void recordAutoExposure(VkCommandBuffer commandBuffer);
    void recordAutoExposureHistogramPass(VkCommandBuffer commandBuffer);
    void recordAutoExposureReducePass(VkCommandBuffer commandBuffer);
    void recordToneMap(VkCommandBuffer commandBuffer);
    void recordToneMapPass(VkCommandBuffer commandBuffer);
    void recordSelectionOutline(VkCommandBuffer commandBuffer);
    void recordSelectionOutlinePass(VkCommandBuffer commandBuffer);
    void recordRenderGraphPlan();
    void updateAdaptiveQuality(const GpuFrameTimings& timings);
    void copyHistoryResources(VkCommandBuffer commandBuffer);
    void copyHistoryResourcesPass(VkCommandBuffer commandBuffer);
    void copyNrdHistoryResources(VkCommandBuffer commandBuffer);
    void copyNrdHistoryResourcesPass(VkCommandBuffer commandBuffer);
    void recordWavefrontQueueClearPass(VkCommandBuffer commandBuffer);
    void recordWavefrontPrimaryGeneratePass(VkCommandBuffer commandBuffer);
    void recordWavefrontShadePass(
        VkCommandBuffer commandBuffer,
        const Buffer* wavefrontRayQueueOverride = nullptr,
        const Buffer* validationBufferOverride = nullptr,
        const Buffer* validationReadbackBufferOverride = nullptr,
        GpuProfiler::Query startQuery = GpuProfiler::WavefrontShadeStart,
        GpuProfiler::Query endQuery = GpuProfiler::WavefrontShadeEnd,
        const char* label = nullptr,
        uint32_t flags = 0u,
        const Buffer* indirectDispatchBufferOverride = nullptr);
    void recordWavefrontDebugWritePass(VkCommandBuffer commandBuffer, bool finalOutput = false);
    void recordWavefrontCompactPass(VkCommandBuffer commandBuffer, bool profile = true);
    void recordWavefrontSortPass(VkCommandBuffer commandBuffer);
    void recordWavefrontHitQueueCountClearPass(VkCommandBuffer commandBuffer);
    void recordWavefrontShadowTracePass(VkCommandBuffer commandBuffer, bool profile = true);
    [[nodiscard]] uint32_t wavefrontMaxPathDepth() const;
    [[nodiscard]] uint32_t wavefrontQueueCapacityFor(VkDeviceSize pixelCount) const;
    [[nodiscard]] bool shouldRunDenoiser() const;
    [[nodiscard]] bool nrdRequested() const;
    [[nodiscard]] bool shouldRunNrdDenoiser() const;
    [[nodiscard]] bool isNonDenoiserDebugView() const;
    [[nodiscard]] bool shouldBypassTemporalUpscalerForDebugView() const;
    [[nodiscard]] bool shouldRunTaa() const;
    [[nodiscard]] bool dlssRequested() const;
    [[nodiscard]] bool shouldRunDlss() const;
    [[nodiscard]] bool shouldRunDlssRayReconstruction() const;
    [[nodiscard]] bool shouldRunRestirSpatial() const;
    [[nodiscard]] bool shouldUseRestirGiReservoirs() const;
    [[nodiscard]] bool shouldRunRestirGiFinal() const;
    [[nodiscard]] bool shouldUseNewRestirDi() const;
    [[nodiscard]] bool shouldRunRestirDiTemporal() const;
    [[nodiscard]] bool shouldRunRestirDiSpatial() const;
    [[nodiscard]] bool shouldRunRestirDiFinal() const;
    [[nodiscard]] bool shouldRunWavefrontDebugWrite() const;
    [[nodiscard]] bool shouldUseWavefrontFinalOutput() const;
    [[nodiscard]] bool effectiveLimitSamplesPerPixel() const;
    [[nodiscard]] bool effectiveRestirGiHalfResolution() const;
    [[nodiscard]] uint32_t effectiveDenoiserMaxHistoryLength() const;
    [[nodiscard]] VkDeviceSize restirGiReservoirStride() const;
    [[nodiscard]] const Image& postDenoiseImage() const;
    [[nodiscard]] const Image& hdrPostProcessImage() const;
    void initializeNrdRuntime();
    void shutdownNrdRuntime();
    void createNrdResolutionResources();
    void initializeDlssRuntime();
    void shutdownDlssRuntime();
    void initializeStreamlineRuntime();
    void shutdownStreamlineRuntime();
    [[nodiscard]] bool streamlineRequested() const;
    bool tagStreamlineImageResource(
        StreamlineFeature feature,
        const char* role,
        const Image& image,
        VkCommandBuffer commandBuffer,
        const char* producerPass,
        StreamlineTagSummary* summary = nullptr);
    bool evaluateStreamlineFeatureForCurrentFrame(
        StreamlineFeature feature,
        VkCommandBuffer commandBuffer,
        const StreamlineTagSummary& tags,
        StreamlineEvaluationSummary& evaluation,
        const char* passName);
    bool markStreamlineReflexLatencyBoundary(StreamlineReflexMarker marker, const char* label);
    void recordStreamlineEvaluationCommandStateBoundary(VkCommandBuffer commandBuffer, const char* passName);
    [[nodiscard]] bool streamlineFeatureSupported(StreamlineFeature feature) const;
    void releaseDlssFeature(bool waitIdleBeforeRelease = false);
    void releaseDlssRayReconstructionFeature(bool waitIdleBeforeRelease = false);
    [[nodiscard]] bool ensureDlssFeature(VkCommandBuffer commandBuffer);
    [[nodiscard]] bool ensureDlssRayReconstructionFeature(VkCommandBuffer commandBuffer);
    void createStbnResources(const std::filesystem::path& shaderDirectory);
    void createGpuSkinningResources();
    void createGpuSkinningRayTracingBindings();
    [[nodiscard]] bool recordGpuSkinningDispatch(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors);
    void dispatchInitialGpuSkinningForAccelerationStructures();
    void writeStbnDescriptors(DescriptorWriter& writer) const;
    [[nodiscard]] float stbnScalarSample(int32_t x, int32_t y, uint32_t frameIndex) const;
    void fallbackBlitPostDenoiseToTemporalOutput(VkCommandBuffer commandBuffer);
    void skipDenoiserPass(VkCommandBuffer commandBuffer);
    void skipDenoiserCopyPass(VkCommandBuffer commandBuffer);
    void recordHardwarePathTrace(VkCommandBuffer commandBuffer);
    void writeRayTracingDescriptors(
        DescriptorSet set,
        bool includeWavefrontQueues,
        const Buffer* wavefrontRayQueueOverride = nullptr);
    void recordWavefrontTracePass(
        VkCommandBuffer commandBuffer,
        const Buffer* wavefrontRayQueueOverride = nullptr,
        bool copyHitHeaderReadback = true,
        const Buffer* indirectTraceBuffer = nullptr,
        VkDeviceSize indirectTraceOffset = 0,
        GpuProfiler::Query profilerStart = GpuProfiler::WavefrontTraceStart,
        GpuProfiler::Query profilerEnd = GpuProfiler::WavefrontTraceEnd);
    void recordWavefrontDirectLightingValidationPass(VkCommandBuffer commandBuffer);
    void recordWavefrontTraceValidationPass(VkCommandBuffer commandBuffer);
    [[nodiscard]] VkPipelineStageFlags2 pathTraceShaderStage() const;

    struct PendingPickRequest {
        bool active = false;
        glm::vec2 viewportUv{};
        uint32_t requestFrame = 0;
        uint64_t sceneVersion = 0;
    };

    struct RetiredResolutionResources {
        uint32_t releaseFrame = 0;
        std::vector<Image> images;
        std::vector<Buffer> buffers;
    };
    const VulkanContext& context_;
    ResourceAllocator& allocator_;
    BufferUploader& uploader_;
    GpuScene scene_;
    GpuSkinningResourcePlan gpuSkinningResourcePlan_{};

    VkExtent2D renderExtent_{};
    VkExtent2D displayExtent_{};
    uint32_t frameCount_ = 0;
    uint32_t temporalFrameIndex_ = 0;
    uint32_t stillFrameCount_ = 0;
    float frameDeltaSeconds_ = 0.0f;
    bool cameraChangedThisFrame_ = false;
    float adaptiveSmoothedGpuMs_ = 0.0f;
    uint32_t adaptiveQualityTier_ = 0;
    uint32_t adaptiveOverBudgetFrames_ = 0;
    uint32_t adaptiveEffectiveMaxBounces_ = 8;
    uint32_t adaptiveEffectiveEnvironmentSamples_ = 1;
    uint32_t adaptiveEffectiveAtrousIterations_ = 4;
    bool adaptiveSkipRestirSpatial_ = false;
    bool adaptiveSkipDenoiser_ = false;
    bool memoryPressureActive_ = false;
    bool memoryPressureOverrideActive_ = false;
    bool memoryPressureQualityChanged_ = false;
    uint32_t memoryPressureTier_ = 0;
    float memoryPressureUsageRatio_ = 0.0f;
    std::string memoryPressureName_ = "normal";
    AccumulationResetReason lastResetReason_ = AccumulationResetReason::Startup;
    CameraUniform camera_{};
    RendererSettings settings_{};
    std::optional<std::filesystem::path> dumpRenderGraphPath_;
    std::optional<std::filesystem::path> dumpRenderGraphDotPath_;
    DenoiserParams denoiserParams_{};
    MomentParams momentParams_{};
    TaaParams taaParams_{};
    RestirSpatialParams restirSpatialParams_{};
    RestirDiParams restirDiParams_{};
    FogParams fogParams_{};
    PrevCameraUniform prevCamera_{};
    RendererDebugParams debugParams_{};
    bool rayTracingDiagnosticCountersEnabled_ = false;
    bool rayTracingDiagnosticCountersCleared_ = false;
    glm::mat4 previousViewProj_{1.0f};
    glm::mat4 nrdViewToClip_{1.0f};
    glm::mat4 nrdViewToClipPrev_{1.0f};
    glm::mat4 nrdWorldToView_{1.0f};
    glm::mat4 nrdWorldToViewPrev_{1.0f};
    glm::vec4 previousCameraPos_{};
    glm::vec2 previousJitter_{0.0f};
    bool denoiserHistoryValid_ = false;
    uint32_t denoiserFramesSinceReset_ = 0;
    bool taaHistoryValid_ = false;
    bool asyncHistoryCopyPending_ = false;
    bool asyncTaaHistoryCopyPending_ = false;
    bool asyncPostProcessPending_ = false;
    bool restirGiHistoryValid_ = false;
    bool restirDiHistoryValid_ = false;
    bool restirGiUncompressedLayout_ = false;
    bool resourceAliasingEnabled_ = true;
    uint64_t pickSceneVersion_ = 0;
    PendingPickRequest pendingPick_{};
    std::vector<RetiredResolutionResources> retiredResolutionResources_;

    Image rawImage_;
    Image denoisedImage_;
    Image historyImage_;
    Image diffuseResolvedImage_;
    Image specularResolvedImage_;
    Image diffuseHistoryImage_;
    Image specularHistoryImage_;
    Image directDiffuseMomentsImage_;
    Image directSpecularMomentsImage_;
    Image indirectDiffuseMomentsImage_;
    Image indirectSpecularMomentsImage_;
    Image historyLengthImage_;
    Image directDiffuseResolvedMomentsImage_;
    Image directSpecularResolvedMomentsImage_;
    Image indirectDiffuseResolvedMomentsImage_;
    Image indirectSpecularResolvedMomentsImage_;
    Image historyLengthResolvedImage_;
    Image momentDebugImage_;
    Image momentDebugResolvedImage_;
    Image taaImage_;
    Image taaHistoryImage_;
    Image dlssDepthImage_;
    Image dlssMotionVectorImage_;
    Image dlssDiffuseAlbedoImage_;
    Image dlssSpecularAlbedoImage_;
    Image dlssNormalImage_;
    Image dlssRoughnessImage_;
    Image dlssDiffuseHitDistanceImage_;
    Image dlssSpecularHitDistanceImage_;
    Image dlssReflectedAlbedoImage_;
    Image dlssDisocclusionMaskImage_;
    Image dlssDiffuseRayDirectionImage_;
    Image dlssSpecularRayDirectionImage_;
    Image dlssDiffuseRayDirectionHitDistanceImage_;
    Image dlssSpecularRayDirectionHitDistanceImage_;
    Image stbnScalarImage_;
    Image presentationImage_;
    Buffer cameraBuffer_;
    Buffer denoiserParamsBuffer_;
    Buffer prevCameraBuffer_;
    Buffer debugParamsBuffer_;
    Buffer accumulationBuffer_;
    Buffer varianceBuffer_;
    Buffer depthNormalBuffer_;
    Buffer worldPositionBuffer_;
    Buffer previousWorldPositionBuffer_;
    Buffer velocityBuffer_;
    Buffer entityIdBuffer_;
    Buffer pathDataBuffer_;
    Buffer streamingResetMaskBuffer_;
    Buffer streamingResetInstanceMaskBuffer_;
    Buffer rayTracingDiagnosticCountersBuffer_;
    Buffer rayTracingDiagnosticCountersReadbackBuffer_;
    Buffer gpuSkinningSourceVertexBuffer_;
    Buffer gpuSkinningMorphDeltaBuffer_;
    Buffer gpuSkinningJointMatrixBuffer_;
    Buffer gpuSkinningPreviousJointMatrixBuffer_;
    Buffer gpuSkinningJointMatrixUploadBuffer_;
    Buffer gpuSkinningPreviousJointMatrixUploadBuffer_;
    Buffer gpuSkinningDummyMorphDeltaBuffer_;
    Buffer gpuSkinningCurrentVertexBuffer_;
    Buffer gpuSkinningPreviousVertexBuffer_;
    Buffer gpuSkinningRayTracingBindingBuffer_;
    Buffer gpuSkinningOutputReadbackBuffer_;
    Buffer wavefrontRayQueueBuffer_;
    Buffer wavefrontCompactedRayQueueBuffer_;
    Buffer wavefrontSortedRayQueueBuffer_;
    Buffer wavefrontHitQueueBuffer_;
    Buffer wavefrontShadowQueueBuffer_;
    Buffer wavefrontPixelStateBuffer_;
    Buffer wavefrontQueueHeaderReadbackBuffer_;
    Buffer wavefrontRaySampleReadbackBuffer_;
    Buffer wavefrontTraceValidationBuffer_;
    Buffer wavefrontTraceValidationReadbackBuffer_;
    Buffer wavefrontShadeValidationBuffer_;
    Buffer wavefrontShadeValidationReadbackBuffer_;
    Buffer wavefrontSecondaryShadeValidationBuffer_;
    Buffer wavefrontSecondaryShadeValidationReadbackBuffer_;
    Buffer wavefrontSortedShadeValidationBuffer_;
    Buffer wavefrontSortedShadeValidationReadbackBuffer_;
    Buffer wavefrontCompactValidationBuffer_;
    Buffer wavefrontCompactValidationReadbackBuffer_;
    Buffer wavefrontSortValidationBuffer_;
    Buffer wavefrontSortValidationReadbackBuffer_;
    Buffer wavefrontSortDispatchBuffer_;
    Buffer wavefrontShadowTraceValidationBuffer_;
    Buffer wavefrontShadowTraceValidationReadbackBuffer_;
    uint32_t wavefrontRayQueueCapacity_ = 0;
    uint32_t wavefrontCompactedRayQueueCapacity_ = 0;
    uint32_t wavefrontSortedRayQueueCapacity_ = 0;
    uint32_t wavefrontHitQueueCapacity_ = 0;
    uint32_t wavefrontShadowQueueCapacity_ = 0;
    uint32_t wavefrontPixelStateCapacity_ = 0;
    Buffer restirReservoirBuffer_;
    Buffer wavefrontRestirReservoirBuffer_;
    Buffer previousRestirReservoirBuffer_;
    Buffer restirSpatialReservoirBuffer_;
    Buffer restirGiReservoirBuffer_;
    Buffer previousRestirGiReservoirBuffer_;
    Buffer restirGiSpatialReservoirBuffer_;
    Buffer wavefrontRestirGiReservoirBuffer_;

    // New ReSTIR DI buffers (plan Phase 1)
    Buffer restirDiReceiverBuffer_;
    Buffer restirDiInitialReservoirBuffer_;
    Buffer restirDiTemporalReservoirBuffer_;
    Buffer restirDiSpatialReservoirBuffer_;
    Buffer restirDiFinalReservoirBuffer_;
    Buffer previousRestirDiReservoirBuffer_;
    Buffer restirDiCountersBuffer_;
    Buffer restirDiCountersReadbackBuffer_;
    Buffer selectionParamsBuffer_;
    Buffer histogramBuffer_;
    Buffer exposureBuffer_;

    VkSampler fullscreenSampler_ = VK_NULL_HANDLE;
    VkSampler nearestSampler_ = VK_NULL_HANDLE;
    std::vector<uint8_t> stbnScalarAtlas_;
    std::unique_ptr<DescriptorLayoutCache> layoutCache_;
    std::unique_ptr<PipelineCache> pipelineCache_;
    std::unique_ptr<AtmosphereLutSystem> atmosphereLutSystem_;
    std::unique_ptr<ShaderModule> denoiserShader_;
    std::unique_ptr<ShaderModule> momentUpdateShader_;
    std::unique_ptr<ShaderModule> taaShader_;
    std::unique_ptr<ShaderModule> gpuSkinningShader_;
    std::unique_ptr<ShaderModule> dlssGuidesShader_;
    std::unique_ptr<ShaderModule> dlssRayReconstructionGuidesShader_;
    std::unique_ptr<ShaderModule> nrdPrepareShader_;
    std::unique_ptr<ShaderModule> nrdResolveShader_;
    std::unique_ptr<ShaderModule> restirSpatialShader_;
    std::unique_ptr<ShaderModule> restirGiSpatialShader_;
    std::unique_ptr<ShaderModule> restirGiFinalShader_;
    std::unique_ptr<ShaderModule> restirDiTemporalShader_;
    std::unique_ptr<ShaderModule> restirDiSpatialShader_;
    std::unique_ptr<ShaderModule> restirDiFinalShader_;
    std::unique_ptr<ShaderModule> fogShader_;
    std::unique_ptr<ShaderModule> transmittanceShader_;
    std::unique_ptr<ShaderModule> multiScatterShader_;
    std::unique_ptr<ShaderModule> skyViewShader_;
    std::unique_ptr<ShaderModule> skyReprojectShader_;
    std::unique_ptr<ShaderModule> aerialPerspectiveShader_;
    std::unique_ptr<ShaderModule> skyCdfShader_;
    std::unique_ptr<ShaderModule> selectionOutlineShader_;
    std::unique_ptr<ShaderModule> luminanceHistogramShader_;
    std::unique_ptr<ShaderModule> exposureReduceShader_;
    std::unique_ptr<ShaderModule> toneMapShader_;
    std::unique_ptr<ShaderModule> fullscreenVertexShader_;
    std::unique_ptr<ShaderModule> fullscreenFragmentShader_;
    std::unique_ptr<ShaderModule> raygenShader_;
    std::unique_ptr<ShaderModule> raygenMotionShader_;
    std::unique_ptr<ShaderModule> primaryMissShader_;
    std::unique_ptr<ShaderModule> shadowMissShader_;
    std::unique_ptr<ShaderModule> closestHitShader_;
    std::unique_ptr<ShaderModule> primaryAnyHitShader_;
    std::unique_ptr<ShaderModule> shadowAnyHitShader_;
    std::unique_ptr<ShaderModule> wavefrontQueueClearShader_;
    std::unique_ptr<ShaderModule> wavefrontPrimaryGenerateShader_;
    std::unique_ptr<ShaderModule> wavefrontTraceRaygenShader_;
    std::unique_ptr<ShaderModule> wavefrontTraceRaygenSerShader_;
    std::unique_ptr<ShaderModule> wavefrontShadowTraceRaygenShader_;
    std::unique_ptr<ShaderModule> wavefrontTraceValidateShader_;
    std::unique_ptr<ShaderModule> wavefrontDirectLightingValidateShader_;
    std::unique_ptr<ShaderModule> wavefrontShadeShader_;
    std::unique_ptr<ShaderModule> wavefrontDebugWriteShader_;
    std::unique_ptr<ShaderModule> wavefrontCompactShader_;
    std::unique_ptr<ShaderModule> wavefrontSortShader_;
    std::unique_ptr<ComputePipeline> denoiserPipeline_;
    std::unique_ptr<ComputePipeline> momentUpdatePipeline_;
    std::unique_ptr<ComputePipeline> taaPipeline_;
    std::unique_ptr<ComputePipeline> gpuSkinningPipeline_;
    std::unique_ptr<ComputePipeline> dlssGuidesPipeline_;
    std::unique_ptr<ComputePipeline> dlssRayReconstructionGuidesPipeline_;
    std::unique_ptr<ComputePipeline> nrdPreparePipeline_;
    std::unique_ptr<ComputePipeline> nrdResolvePipeline_;
    std::unique_ptr<ComputePipeline> restirSpatialPipeline_;
    std::unique_ptr<ComputePipeline> restirGiSpatialPipeline_;
    std::unique_ptr<ComputePipeline> restirGiFinalPipeline_;
    std::unique_ptr<ComputePipeline> restirDiTemporalPipeline_;
    std::unique_ptr<ComputePipeline> restirDiSpatialPipeline_;
    std::unique_ptr<ComputePipeline> restirDiFinalPipeline_;
    std::unique_ptr<ComputePipeline> fogPipeline_;
    std::unique_ptr<ComputePipeline> selectionOutlinePipeline_;
    std::unique_ptr<ComputePipeline> luminanceHistogramPipeline_;
    std::unique_ptr<ComputePipeline> exposureReducePipeline_;
    std::unique_ptr<ComputePipeline> toneMapPipeline_;
    std::unique_ptr<ComputePipeline> wavefrontQueueClearPipeline_;
    std::unique_ptr<ComputePipeline> wavefrontPrimaryGeneratePipeline_;
    std::unique_ptr<ComputePipeline> wavefrontTraceValidatePipeline_;
    std::unique_ptr<ComputePipeline> wavefrontDirectLightingValidatePipeline_;
    std::unique_ptr<ComputePipeline> wavefrontShadePipeline_;
    std::unique_ptr<ComputePipeline> wavefrontDebugWritePipeline_;
    std::unique_ptr<ComputePipeline> wavefrontCompactPipeline_;
    std::unique_ptr<ComputePipeline> wavefrontSortPipeline_;
    std::unique_ptr<GraphicsPipeline> graphicsPipeline_;
    std::unique_ptr<RayTracingPipeline> rayTracingPipeline_;
    std::unique_ptr<RayTracingPipeline> rayTracingMotionPipeline_;
    std::unique_ptr<RayTracingPipeline> wavefrontTracePipeline_;
    std::unique_ptr<RayTracingPipeline> wavefrontTraceSerPipeline_;
    std::unique_ptr<RayTracingScene> rayTracingScene_;
    std::unique_ptr<TemporalSystem> temporalSystem_;
    PhysicalCamera physicalCamera_;
    VkDescriptorSetLayout atmosphereSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rayTracingSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout denoiserSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout momentUpdateSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout taaSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout gpuSkinningSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dlssGuidesSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dlssRayReconstructionGuidesSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout nrdPrepareSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout nrdResolveSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout restirSpatialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout restirGiSpatialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout restirGiFinalSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout restirDiTemporalSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout restirDiSpatialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout restirDiFinalSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout fogSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout selectionOutlineSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout luminanceHistogramSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout exposureReduceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout toneMapSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout wavefrontQueueClearSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout wavefrontPrimaryGenerateSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout wavefrontTraceValidateSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout wavefrontDirectLightingValidateSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout wavefrontShadeSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout wavefrontDebugWriteSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout wavefrontCompactSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout wavefrontSortSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout graphicsSetLayout_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<FrameResources>> frames_;
    std::vector<GpuProfiler> profilers_;
    FrameResources* currentFrame_ = nullptr;
    GpuProfiler* currentProfiler_ = nullptr;
    RendererValidationLog validationLog_;
    std::unique_ptr<ShaderCompiler> shaderCompiler_;
    std::vector<std::filesystem::path> shaderSources_;
    std::filesystem::path shaderOutputDirectory_;
    uint32_t selectedInstanceId_ = UINT32_MAX;
    StreamingResetMaskReport streamingResetMaskReport_{};
    std::unique_ptr<NrdRuntime> nrdRuntime_;
    bool nrdAvailable_ = false;
    bool nrdCreationFailed_ = false;
    std::string nrdUnavailableReason_;
    bool dlssNgxInitialized_ = false;
    bool dlssCapabilityAvailable_ = false;
    bool dlssFeatureCreationFailed_ = false;
    std::string dlssUnavailableReason_;
    bool dlssRayReconstructionCapabilityAvailable_ = false;
    bool dlssRayReconstructionFeatureCreationFailed_ = false;
    std::string dlssRayReconstructionUnavailableReason_;
    bool dlssFrameGenerationCapabilityAvailable_ = false;
    bool dlssFrameGenerationPresentationAvailable_ = false;
    std::string dlssFrameGenerationUnavailableReason_;
    void* dlssParameters_ = nullptr;
    void* dlssHandle_ = nullptr;
    void* dlssRayReconstructionHandle_ = nullptr;
    VkExtent2D dlssFeatureRenderExtent_{};
    VkExtent2D dlssFeatureOutputExtent_{};
    VkExtent2D dlssRayReconstructionFeatureRenderExtent_{};
    VkExtent2D dlssRayReconstructionFeatureOutputExtent_{};
    StreamlineRuntime streamlineRuntime_;
    bool streamlineInitializationAttempted_ = false;
    bool streamlineFrameActive_ = false;
    StreamlineTagSummary streamlineDlssTags_{};
    StreamlineTagSummary streamlineDlssRayReconstructionTags_{};
    StreamlineEvaluationSummary streamlineDlssEvaluation_{};
    StreamlineEvaluationSummary streamlineDlssRayReconstructionEvaluation_{};
    StreamlineReflexMarkerSummary streamlineReflexMarkers_{};
};

} // namespace rtv
