#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>
#include <vk_mem_alloc.h>

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

    void setDebugName(VkObjectType objectType, uint64_t objectHandle, const char* name) const;

private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    bool supportsDeviceAddress_ = false;
    bool supportsSamplerAnisotropy_ = false;
    float maxSamplerAnisotropy_ = 1.0f;
};

} // namespace rtv
