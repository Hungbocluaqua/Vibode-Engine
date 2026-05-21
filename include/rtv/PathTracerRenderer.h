#pragma once

#include "rtv/FrameResources.h"
#include "rtv/GpuProfiler.h"
#include "rtv/GpuValidation.h"
#include "rtv/GpuScene.h"
#include "rtv/Image.h"
#include "rtv/RendererBackend.h"
#include "rtv/RendererDebug.h"

#include <Volk/volk.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace rtv {

class BufferUploader;
class ComputePipeline;
class DescriptorLayoutCache;
class GraphicsPipeline;
class PipelineCache;
class RayTracingPipeline;
class RayTracingScene;
class ResourceAllocator;
class ShaderModule;
class TemporalSystem;
class VulkanContext;
class AssetManager;
class AtmosphereLutSystem;
struct AtmosphereLutStats;
struct SceneAsset;

struct RendererSettings {
    bool pathTracingEnabled = true;
    bool cameraJitterEnabled = true;
    bool denoiserEnabled = true;
    bool denoiseWhileMoving = false;
    bool taaEnabled = true;
    float taaFeedback = 0.08f;
    bool sunlightEnabled = true;
    bool directLightingEnabled = true;
    bool environmentEnabled = true;
    uint32_t maxBounces = 8;
    uint32_t atrousIterations = 4;
    uint32_t environmentDirectSamples = 1;
    float denoiserStrength = 1.0f;
    float sunIntensity = 1.0f;
    float skyIntensity = 0.8f;
    float sunElevation = 0.97f;
    ToneMapper toneMapper = ToneMapper::ACES;
    float exposure = 2.0f;
    float gamma = 2.2f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float brightness = 0.0f;
    float whitePoint = 4.0f;
    bool autoExposureEnabled = false;
    float targetLuminance = 0.18f;
    float minExposure = 0.25f;
    float maxExposure = 8.0f;
    float adaptationSpeed = 2.0f;
    float histogramMinLogLuminance = -10.0f;
    float histogramMaxLogLuminance = 10.0f;
    float histogramLowPercentile = 0.05f;
    float histogramHighPercentile = 0.95f;
    float histogramTargetPercentile = 0.60f;
    float sunAngularRadius = 0.0093f;
    float indirectStrength = 1.0f;
    RestirMode restirMode = RestirMode::ClassicNee;
    float environmentIntensity = 1.0f;
    float environmentRotation = 0.0f;
    float environmentBackgroundIntensity = 0.35f;
    float renderResolutionScale = 1.0f;
    RendererBackend requestedBackend = RendererBackend::Auto;
    RendererDebugView debugView = RendererDebugView::Beauty;
    float debugScale = 1.0f;
};

struct RayTracingRendererStats {
    bool active = false;
    uint32_t blasCount = 0;
    uint32_t instanceCount = 0;
    VkDeviceSize accelerationStructureBytes = 0;
    VkDeviceSize sbtBytes = 0;
    float lastTlasRefitMs = 0.0f;
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
    BackendChanged,
};

[[nodiscard]] const char* accumulationResetReasonName(AccumulationResetReason reason);

class PathTracerRenderer {
public:
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
        RendererBackend requestedBackend = RendererBackend::Auto);
    ~PathTracerRenderer();

    void beginFrame(uint32_t frameIndex, VkExtent2D extent);
    void setFrameDeltaSeconds(float deltaSeconds) { frameDeltaSeconds_ = deltaSeconds; }
    void recordPathTrace(VkCommandBuffer commandBuffer);
    void recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent);
    void recordEditorPresentationStart(VkCommandBuffer commandBuffer);
    void recordEditorPresentationEnd(VkCommandBuffer commandBuffer);

    bool applySettings(const RendererSettings& settings);
    void setCameraPose(glm::vec3 position, glm::vec3 forward);
    void setCameraFovY(float fovY);
    void resetAccumulation(AccumulationResetReason reason = AccumulationResetReason::Manual);
    void loadEnvironment(const std::filesystem::path& path);
    bool updateMaterials(const SceneAsset& scene, const AssetManager& assets);
    bool updateSceneLights(const SceneAsset& scene);
    bool updateSceneTransforms(const SceneAsset& scene, const AssetManager& assets);
    bool updateSceneVisibility(const SceneAsset& scene, const AssetManager& assets);
    void setSelectedInstanceId(std::optional<uint32_t> instanceId);
    [[nodiscard]] std::optional<uint32_t> pickInstanceId(glm::vec2 viewportUv);

    [[nodiscard]] const RendererSettings& settings() const { return settings_; }
    [[nodiscard]] RendererBackend requestedBackend() const { return requestedBackend_; }
    [[nodiscard]] RendererBackend activeBackend() const { return activeBackend_; }
    [[nodiscard]] bool hardwareRayTracingAvailable() const;
    [[nodiscard]] RayTracingRendererStats rayTracingStats() const;
    [[nodiscard]] uint32_t sampleCount() const { return frameCount_; }
    [[nodiscard]] const GpuFrameTimings& timings() const;
    [[nodiscard]] AccumulationResetReason lastAccumulationResetReason() const { return lastResetReason_; }
    [[nodiscard]] const RendererValidationLog& validationLog() const { return validationLog_; }
    [[nodiscard]] RendererValidationLog& validationLog() { return validationLog_; }
    [[nodiscard]] const TemporalSystem* temporalSystem() const { return temporalSystem_.get(); }
    [[nodiscard]] AtmosphereLutStats atmosphereLutStats() const;
    [[nodiscard]] const GpuScene& scene() const { return scene_; }
    [[nodiscard]] VkDescriptorImageInfo viewportImageDescriptor() const;
    [[nodiscard]] VkExtent2D renderExtent() const { return extent_; }

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
        float maxExposure = 8.0f;
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
    };

    struct TaaParams {
        uint32_t enabled = 1;
        uint32_t frameCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        float feedback = 0.08f;
        float velocityScale = 64.0f;
        uint32_t resetHistory = 1;
        uint32_t padding = 0;
    };

    struct RestirSpatialParams {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameCount = 0;
        uint32_t enabled = 0;
    };

    struct RestirReservoirGpu {
        glm::uvec4 metadata{};
        glm::vec4 sampleValueConfidence{};
        glm::vec4 targetPdfWeightSumM{};
    };

    void createResolutionResources(VkExtent2D extent);
    void updateCamera();
    void recordPathTraceGraph(VkCommandBuffer commandBuffer);
    void recordPathTracePass(VkCommandBuffer commandBuffer);
    void recordRestirSpatial(VkCommandBuffer commandBuffer);
    void recordRestirSpatialPass(VkCommandBuffer commandBuffer);
    void recordRestirSpatialCopyPass(VkCommandBuffer commandBuffer);
    void recordDenoiser(VkCommandBuffer commandBuffer);
    void recordDenoiserPass(VkCommandBuffer commandBuffer);
    void recordTaa(VkCommandBuffer commandBuffer);
    void recordTaaPass(VkCommandBuffer commandBuffer);
    void recordTaaHistoryCopyPass(VkCommandBuffer commandBuffer);
    void recordAutoExposure(VkCommandBuffer commandBuffer);
    void recordAutoExposureHistogramPass(VkCommandBuffer commandBuffer);
    void recordAutoExposureReducePass(VkCommandBuffer commandBuffer);
    void recordToneMap(VkCommandBuffer commandBuffer);
    void recordToneMapPass(VkCommandBuffer commandBuffer);
    void recordSelectionOutline(VkCommandBuffer commandBuffer);
    void recordSelectionOutlinePass(VkCommandBuffer commandBuffer);
    void recordRenderGraphPlan();
    void copyHistoryResources(VkCommandBuffer commandBuffer);
    void copyHistoryResourcesPass(VkCommandBuffer commandBuffer);
    [[nodiscard]] bool shouldRunDenoiser() const;
    [[nodiscard]] bool shouldRunTaa() const;
    [[nodiscard]] bool shouldRunRestirSpatial() const;
    [[nodiscard]] const Image& postDenoiseImage() const;
    [[nodiscard]] const Image& hdrPostProcessImage() const;
    void skipDenoiserPass(VkCommandBuffer commandBuffer);
    void skipDenoiserCopyPass(VkCommandBuffer commandBuffer);
    void recordHardwarePathTrace(VkCommandBuffer commandBuffer);
    void recordComputePathTrace(VkCommandBuffer commandBuffer);
    [[nodiscard]] VkPipelineStageFlags2 pathTraceShaderStage() const;

    const VulkanContext& context_;
    ResourceAllocator& allocator_;
    BufferUploader& uploader_;
    GpuScene scene_;

    VkExtent2D extent_{};
    uint32_t frameCount_ = 0;
    float frameDeltaSeconds_ = 0.0f;
    bool cameraChangedThisFrame_ = false;
    AccumulationResetReason lastResetReason_ = AccumulationResetReason::Startup;
    CameraUniform camera_{};
    RendererSettings settings_{};
    DenoiserParams denoiserParams_{};
    TaaParams taaParams_{};
    RestirSpatialParams restirSpatialParams_{};
    PrevCameraUniform prevCamera_{};
    RendererDebugParams debugParams_{};
    RendererBackend requestedBackend_ = RendererBackend::Auto;
    RendererBackend activeBackend_ = RendererBackend::Compute;
    glm::mat4 previousViewProj_{1.0f};
    glm::vec4 previousCameraPos_{};
    glm::vec2 previousJitter_{0.0f};

    Image rawImage_;
    Image denoisedImage_;
    Image historyImage_;
    Image taaImage_;
    Image taaHistoryImage_;
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
    Buffer restirReservoirBuffer_;
    Buffer previousRestirReservoirBuffer_;
    Buffer restirSpatialReservoirBuffer_;
    Buffer selectionParamsBuffer_;
    Buffer histogramBuffer_;
    Buffer exposureBuffer_;

    VkSampler fullscreenSampler_ = VK_NULL_HANDLE;
    std::unique_ptr<DescriptorLayoutCache> layoutCache_;
    std::unique_ptr<PipelineCache> pipelineCache_;
    std::unique_ptr<AtmosphereLutSystem> atmosphereLutSystem_;
    std::unique_ptr<ShaderModule> pathTraceShader_;
    std::unique_ptr<ShaderModule> denoiserShader_;
    std::unique_ptr<ShaderModule> taaShader_;
    std::unique_ptr<ShaderModule> restirSpatialShader_;
    std::unique_ptr<ShaderModule> transmittanceShader_;
    std::unique_ptr<ShaderModule> multiScatterShader_;
    std::unique_ptr<ShaderModule> skyViewShader_;
    std::unique_ptr<ShaderModule> aerialPerspectiveShader_;
    std::unique_ptr<ShaderModule> selectionOutlineShader_;
    std::unique_ptr<ShaderModule> luminanceHistogramShader_;
    std::unique_ptr<ShaderModule> exposureReduceShader_;
    std::unique_ptr<ShaderModule> toneMapShader_;
    std::unique_ptr<ShaderModule> fullscreenVertexShader_;
    std::unique_ptr<ShaderModule> fullscreenFragmentShader_;
    std::unique_ptr<ShaderModule> raygenShader_;
    std::unique_ptr<ShaderModule> primaryMissShader_;
    std::unique_ptr<ShaderModule> shadowMissShader_;
    std::unique_ptr<ShaderModule> closestHitShader_;
    std::unique_ptr<ShaderModule> primaryAnyHitShader_;
    std::unique_ptr<ShaderModule> shadowAnyHitShader_;
    std::unique_ptr<ComputePipeline> pathTracePipeline_;
    std::unique_ptr<ComputePipeline> denoiserPipeline_;
    std::unique_ptr<ComputePipeline> taaPipeline_;
    std::unique_ptr<ComputePipeline> restirSpatialPipeline_;
    std::unique_ptr<ComputePipeline> selectionOutlinePipeline_;
    std::unique_ptr<ComputePipeline> luminanceHistogramPipeline_;
    std::unique_ptr<ComputePipeline> exposureReducePipeline_;
    std::unique_ptr<ComputePipeline> toneMapPipeline_;
    std::unique_ptr<GraphicsPipeline> graphicsPipeline_;
    std::unique_ptr<RayTracingPipeline> rayTracingPipeline_;
    std::unique_ptr<RayTracingScene> rayTracingScene_;
    std::unique_ptr<TemporalSystem> temporalSystem_;
    VkDescriptorSetLayout pathTraceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout atmosphereSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rayTracingSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout denoiserSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout taaSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout restirSpatialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout selectionOutlineSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout luminanceHistogramSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout exposureReduceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout toneMapSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout graphicsSetLayout_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<FrameResources>> frames_;
    std::vector<GpuProfiler> profilers_;
    FrameResources* currentFrame_ = nullptr;
    GpuProfiler* currentProfiler_ = nullptr;
    RendererValidationLog validationLog_;
    uint32_t selectedInstanceId_ = UINT32_MAX;
};

} // namespace rtv
