#pragma once

#include <Volk/volk.h>

namespace rtv::barrier {

struct ImageTransition {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageSubresourceRange range{};
    VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 dstAccess = VK_ACCESS_2_NONE;
    uint32_t srcQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t dstQueueFamily = VK_QUEUE_FAMILY_IGNORED;
};

struct BufferTransition {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = VK_WHOLE_SIZE;
    VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 dstAccess = VK_ACCESS_2_NONE;
    uint32_t srcQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t dstQueueFamily = VK_QUEUE_FAMILY_IGNORED;
};

[[nodiscard]] VkImageSubresourceRange colorRange(uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS);

void cmdTransitionImage(VkCommandBuffer commandBuffer, const ImageTransition& transition);
void cmdBufferBarrier(VkCommandBuffer commandBuffer, const BufferTransition& transition);

} // namespace rtv::barrier
