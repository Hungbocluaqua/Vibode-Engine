#include "rtv/UploadContext.h"

#include "rtv/Check.h"

namespace rtv {

UploadContext::UploadContext(VkDevice device, VkQueue queue, uint32_t queueFamilyIndex)
    : device_(device), queue_(queue), queueFamilyIndex_(queueFamilyIndex) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex_;
    checkVk(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool(upload)");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &fence_), "vkCreateFence(upload)");
}

UploadContext::~UploadContext() {
    waitIdle();
    if (fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, fence_, nullptr);
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
    }
}

VkCommandBuffer UploadContext::begin() {
    checkVk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(upload)");
    checkVk(vkResetFences(device_, 1, &fence_), "vkResetFences(upload)");
    checkVk(vkResetCommandPool(device_, commandPool_, 0), "vkResetCommandPool(upload)");

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    checkVk(vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer), "vkAllocateCommandBuffers(upload)");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(upload)");
    return commandBuffer;
}

void UploadContext::submitAndWait(VkCommandBuffer commandBuffer) {
    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(upload)");

    VkCommandBufferSubmitInfo commandBufferInfo{};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = commandBuffer;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;

    checkVk(vkQueueSubmit2(queue_, 1, &submitInfo, fence_), "vkQueueSubmit2(upload)");
    checkVk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(upload submit)");
}

void UploadContext::waitIdle() const {
    if (device_ != VK_NULL_HANDLE && fence_ != VK_NULL_HANDLE) {
        (void)vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    }
}

} // namespace rtv
