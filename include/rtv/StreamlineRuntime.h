#pragma once

#include <Volk/volk.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

enum class StreamlineFeature : uint32_t {
    Dlss,
    DlssRayReconstruction,
    DlssFrameGeneration,
    Reflex,
    Nis,
    Nrd,
    NvPerf,
};

enum class StreamlineReflexMarker : uint32_t {
    SimulationStart,
    SimulationEnd,
    RenderSubmitStart,
    RenderSubmitEnd,
    PresentStart,
    PresentEnd,
};

struct StreamlineFeatureStatus {
    bool requestable = false;
    bool supported = false;
    std::string unavailableReason;
    std::string requirements;
};

struct StreamlineStatus {
    bool sdkConfigured = false;
    bool runtimeConfigured = false;
    bool initialized = false;
    bool vulkanInfoSet = false;
    bool manualVulkanIntegration = true;
    bool interposerIntegration = false;
    std::string runtimeDirectory;
    std::string runtimeDll;
    std::string unavailableReason;
    std::vector<std::string> resolvedEntryPoints;
    std::vector<std::string> missingEntryPoints;
    std::vector<std::string> logMessages;
    StreamlineFeatureStatus dlss;
    StreamlineFeatureStatus dlssRayReconstruction;
    StreamlineFeatureStatus dlssFrameGeneration;
    StreamlineFeatureStatus reflex;
    StreamlineFeatureStatus nis;
    StreamlineFeatureStatus nrd;
    StreamlineFeatureStatus nvperf;
};

struct StreamlineInitDesc {
    std::filesystem::path runtimeDirectory;
    bool allowInterposer = false;
    bool enableOtaUpdates = false;
    bool enableLogging = true;
};

struct StreamlineVulkanInfo {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
    uint32_t graphicsQueueIndex = 0;
    uint32_t computeQueueFamily = 0;
    uint32_t computeQueueIndex = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
};

struct StreamlineFrameDesc {
    uint64_t frameIndex = 0;
    uint32_t viewportId = 0;
    VkExtent2D renderExtent{};
    VkExtent2D outputExtent{};
};

struct StreamlineConstantsDesc {
    VkExtent2D renderExtent{};
    VkExtent2D outputExtent{};
    std::array<float, 16> cameraViewToClip{};
    std::array<float, 16> clipToCameraView{};
    std::array<float, 16> clipToPrevClip{};
    std::array<float, 16> prevClipToClip{};
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;
    std::array<float, 3> cameraPosition{};
    std::array<float, 3> cameraUp{0.0f, 1.0f, 0.0f};
    std::array<float, 3> cameraRight{1.0f, 0.0f, 0.0f};
    std::array<float, 3> cameraForward{0.0f, 0.0f, -1.0f};
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraFovRadians = 0.0f;
    float cameraAspectRatio = 0.0f;
    bool reset = false;
    bool hdr = true;
    bool depthInverted = true;
    bool cameraMotionIncluded = true;
    bool motionVectors3D = false;
    bool matricesValid = false;
    bool nvPerfEnabled = false;
};

struct StreamlineResourceTagDesc {
    StreamlineFeature feature = StreamlineFeature::Dlss;
    std::string role;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    std::string producerPass;
};

enum class StreamlineDlssQualityMode : uint32_t {
    Dlaa,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

struct StreamlineDlssOptionsDesc {
    VkExtent2D outputExtent{};
    StreamlineDlssQualityMode qualityMode = StreamlineDlssQualityMode::Balanced;
    float sharpness = 0.0f;
    float preExposure = 1.0f;
    float exposureScale = 1.0f;
    bool hdr = true;
    bool useAutoExposure = false;
    bool alphaUpscaling = false;
};

struct StreamlineDlssRayReconstructionOptionsDesc {
    VkExtent2D outputExtent{};
    StreamlineDlssQualityMode qualityMode = StreamlineDlssQualityMode::Balanced;
    float sharpness = 0.0f;
    float preExposure = 1.0f;
    float exposureScale = 1.0f;
    bool hdr = true;
    bool alphaUpscaling = false;
    bool normalRoughnessPacked = false;
    std::array<float, 16> worldToCameraView{};
    std::array<float, 16> cameraViewToWorld{};
    bool matricesValid = false;
};

struct StreamlineVulkanRequirements {
    bool available = false;
    bool initialized = false;
    std::string unavailableReason;
    std::vector<std::string> instanceExtensions;
    std::vector<std::string> deviceExtensions;
    std::vector<std::string> features12;
    std::vector<std::string> features13;
    uint32_t computeQueuesRequired = 0;
    uint32_t graphicsQueuesRequired = 0;
    uint32_t maxViewports = 0;
    std::vector<std::string> requiredTags;
};

class StreamlineRuntime {
public:
    StreamlineRuntime();
    ~StreamlineRuntime();

    StreamlineRuntime(const StreamlineRuntime&) = delete;
    StreamlineRuntime& operator=(const StreamlineRuntime&) = delete;

    bool initialize(const StreamlineInitDesc& desc);
    void shutdown();
    bool setVulkanInfo(const StreamlineVulkanInfo& info);
    bool beginFrame(const StreamlineFrameDesc& desc);
    void endFrame();
    void queryCapabilities();
    [[nodiscard]] StreamlineFeatureStatus queryFeatureRequirements(StreamlineFeature feature) const;
    [[nodiscard]] const StreamlineStatus& featureStatus() const { return status_; }
    bool setConstants(const StreamlineConstantsDesc& desc);
    bool setDlssOptions(const StreamlineDlssOptionsDesc& desc);
    bool setDlssRayReconstructionOptions(const StreamlineDlssRayReconstructionOptionsDesc& desc);
    bool setNvPerfEnabled(bool enabled);
    bool tagResourceForFrame(const StreamlineResourceTagDesc& desc);
    bool evaluateFeature(StreamlineFeature feature, VkCommandBuffer commandBuffer);
    bool setReflexMarker(StreamlineReflexMarker marker);
    void freeFeatureResources(StreamlineFeature feature);
    void releaseResourcesForSwapchain();
    void releaseResourcesForRenderer();

    [[nodiscard]] static StreamlineStatus compileTimeStatus();
    [[nodiscard]] static StreamlineVulkanRequirements collectVulkanRequirements();

private:
    void refreshCompileTimeStatus();
    void clearFrameState();
    void clearRuntimeDispatch();
    [[nodiscard]] StreamlineFeatureStatus& mutableFeatureStatus(StreamlineFeature feature);
    [[nodiscard]] const StreamlineFeatureStatus& featureStatus(StreamlineFeature feature) const;

    StreamlineStatus status_{};
    StreamlineVulkanInfo vulkanInfo_{};
    StreamlineFrameDesc frame_{};
    uint64_t frameToken_ = 0;
    void* nativeFrameToken_ = nullptr;
    bool frameActive_ = false;
    std::vector<StreamlineResourceTagDesc> frameTags_;
    void* runtimeModule_ = nullptr;
    void* dispatch_ = nullptr;
};

} // namespace rtv
