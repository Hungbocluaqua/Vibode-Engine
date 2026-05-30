#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <array>
#include <cstdint>
#include <vector>

namespace rtv {

class VulkanContext;
class Swapchain;
class ResourceDemo;
class PipelineDemo;
class PathTracerRenderer;
class UiOverlay;

class CommandSystem final : private NonCopyable {
public:
    static constexpr uint32_t framesInFlight = 3;

    CommandSystem(const VulkanContext& context, Swapchain& swapchain, bool disableAsyncCompute = false, bool singleQueueFallback = false);
    ~CommandSystem();

    void setResourceDemo(ResourceDemo* demo) { resourceDemo_ = demo; }
    void setPipelineDemo(PipelineDemo* demo) { pipelineDemo_ = demo; }
    void setPathTracer(PathTracerRenderer* renderer) { pathTracer_ = renderer; }
    void setUiOverlay(UiOverlay* overlay) { uiOverlay_ = overlay; }
    void setHeadless(bool headless) { headless_ = headless; }
    void drawFrame(float clearPhase, float deltaSeconds);
    void waitIdle() const;

private:
    struct FrameResources {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkCommandBuffer postCommandBuffer = VK_NULL_HANDLE;
        VkCommandPool computeCommandPool = VK_NULL_HANDLE;
        VkCommandBuffer computeCommandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };

    void createFrameResources();
    void createPresentSemaphores();
    void destroyFrameResources();
    void destroyPresentSemaphores();
    void recreateSwapchainResources();
    void waitForFrameFences() const;
    [[nodiscard]] bool canRecordAsyncCompute() const;
    [[nodiscard]] bool recordAsyncComputeCommands(FrameResources& frame) const;
    void submitFrame(FrameResources& frame, uint32_t imageIndex, bool asyncComputeRecorded) const;
    void recordWorkCommands(VkCommandBuffer commandBuffer, uint32_t imageIndex, float clearPhase) const;
    void recordPresentationCommands(VkCommandBuffer commandBuffer, uint32_t imageIndex, float clearPhase) const;
    void transitionImage(
        VkCommandBuffer commandBuffer,
        VkImage image,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2 dstAccess) const;

    const VulkanContext& context_;
    Swapchain& swapchain_;
    bool headless_ = false;
    bool asyncComputeEnabled_ = false;
    uint32_t headlessImageIndex_ = 0;
    ResourceDemo* resourceDemo_ = nullptr;
    PipelineDemo* pipelineDemo_ = nullptr;
    PathTracerRenderer* pathTracer_ = nullptr;
    UiOverlay* uiOverlay_ = nullptr;
    std::array<FrameResources, framesInFlight> frames_{};
    std::vector<VkSemaphore> imageRenderFinished_;
    mutable uint64_t asyncTimelineValue_ = 0;
    mutable uint64_t asyncHistoryCompleteValue_ = 0;
    uint32_t frameIndex_ = 0;
};

} // namespace rtv
