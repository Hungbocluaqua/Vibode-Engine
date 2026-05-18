#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <cstdint>

namespace rtv {

class UploadContext final : private NonCopyable {
public:
    UploadContext(VkDevice device, VkQueue queue, uint32_t queueFamilyIndex);
    ~UploadContext();

    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] VkQueue queue() const { return queue_; }
    [[nodiscard]] uint32_t queueFamilyIndex() const { return queueFamilyIndex_; }

    VkCommandBuffer begin();
    void submitAndWait(VkCommandBuffer commandBuffer);
    void waitIdle() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

} // namespace rtv
