#pragma once

#include <Volk/volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

struct AccumulationInvalidationEvent {
    std::string reason;
    uint64_t frame = 0;
};

struct SceneUpdateRouteEvent {
    std::string kind;
    std::string action;
    uint64_t count = 0;
};

struct ResourceStateEvent {
    std::string resource;
    std::string beforePass;
    std::string afterPass;
    VkImageLayout beforeLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout afterLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 beforeStage = VK_PIPELINE_STAGE_2_NONE;
    VkPipelineStageFlags2 afterStage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 beforeAccess = VK_ACCESS_2_NONE;
    VkAccessFlags2 afterAccess = VK_ACCESS_2_NONE;
};

class RendererValidationLog {
public:
    void recordBarrier(std::string label, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
    void recordResourceState(ResourceStateEvent event);
    void recordAccumulationInvalidation(std::string reason, uint64_t frame);
    void recordSceneUpdateRoute(std::string kind, std::string action);
    void beginFrame(uint64_t frame);
    void recordPass(std::string label);

    [[nodiscard]] const std::vector<std::string>& barrierEvents() const { return barrierEvents_; }
    [[nodiscard]] const std::vector<AccumulationInvalidationEvent>& invalidations() const { return invalidations_; }
    [[nodiscard]] const std::vector<SceneUpdateRouteEvent>& sceneUpdateRoutes() const { return sceneUpdateRoutes_; }
    [[nodiscard]] const std::vector<std::string>& passEvents() const { return passEvents_; }
    [[nodiscard]] const std::vector<ResourceStateEvent>& resourceStateEvents() const { return resourceStateEvents_; }

private:
    std::vector<std::string> barrierEvents_;
    std::vector<AccumulationInvalidationEvent> invalidations_;
    std::vector<SceneUpdateRouteEvent> sceneUpdateRoutes_;
    std::vector<std::string> passEvents_;
    std::vector<ResourceStateEvent> resourceStateEvents_;
    uint64_t currentFrame_ = 0;
};

} // namespace rtv
