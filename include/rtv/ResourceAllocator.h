#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>
#include <vk_mem_alloc.h>

#include <array>

namespace rtv {

class VulkanContext;

class ResourceAllocator final : private NonCopyable {
public:
    explicit ResourceAllocator(const VulkanContext& context);
    ~ResourceAllocator();

    [[nodiscard]] VmaAllocator handle() const { return allocator_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    [[nodiscard]] bool supportsDeviceAddress() const { return supportsDeviceAddress_; }
    [[nodiscard]] bool supportsSamplerAnisotropy() const { return supportsSamplerAnisotropy_; }
    [[nodiscard]] float maxSamplerAnisotropy() const { return maxSamplerAnisotropy_; }
    [[nodiscard]] VkSharingMode graphicsComputeSharingMode() const { return graphicsComputeSharingMode_; }
    [[nodiscard]] uint32_t graphicsComputeQueueFamilyCount() const { return graphicsComputeQueueFamilyCount_; }
    [[nodiscard]] const uint32_t* graphicsComputeQueueFamilies() const { return graphicsComputeQueueFamilies_.data(); }

    void setDebugName(VkObjectType objectType, uint64_t objectHandle, const char* name) const;

private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    bool supportsDeviceAddress_ = false;
    bool supportsSamplerAnisotropy_ = false;
    float maxSamplerAnisotropy_ = 1.0f;
    VkSharingMode graphicsComputeSharingMode_ = VK_SHARING_MODE_EXCLUSIVE;
    uint32_t graphicsComputeQueueFamilyCount_ = 0;
    std::array<uint32_t, 2> graphicsComputeQueueFamilies_{};
};

} // namespace rtv
