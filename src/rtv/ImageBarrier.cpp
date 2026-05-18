#include "rtv/ImageBarrier.h"

namespace rtv::barrier {

VkImageSubresourceRange colorRange(uint32_t baseMip, uint32_t mipCount) {
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = baseMip;
    range.levelCount = mipCount;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    return range;
}

void cmdTransitionImage(VkCommandBuffer commandBuffer, const ImageTransition& transition) {
    VkImageMemoryBarrier2 imageBarrier{};
    imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imageBarrier.srcStageMask = transition.srcStage;
    imageBarrier.srcAccessMask = transition.srcAccess;
    imageBarrier.dstStageMask = transition.dstStage;
    imageBarrier.dstAccessMask = transition.dstAccess;
    imageBarrier.oldLayout = transition.oldLayout;
    imageBarrier.newLayout = transition.newLayout;
    imageBarrier.srcQueueFamilyIndex = transition.srcQueueFamily;
    imageBarrier.dstQueueFamilyIndex = transition.dstQueueFamily;
    imageBarrier.image = transition.image;
    imageBarrier.subresourceRange = transition.range;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &imageBarrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void cmdBufferBarrier(VkCommandBuffer commandBuffer, const BufferTransition& transition) {
    VkBufferMemoryBarrier2 bufferBarrier{};
    bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    bufferBarrier.srcStageMask = transition.srcStage;
    bufferBarrier.srcAccessMask = transition.srcAccess;
    bufferBarrier.dstStageMask = transition.dstStage;
    bufferBarrier.dstAccessMask = transition.dstAccess;
    bufferBarrier.srcQueueFamilyIndex = transition.srcQueueFamily;
    bufferBarrier.dstQueueFamilyIndex = transition.dstQueueFamily;
    bufferBarrier.buffer = transition.buffer;
    bufferBarrier.offset = transition.offset;
    bufferBarrier.size = transition.size;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &bufferBarrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

} // namespace rtv::barrier
