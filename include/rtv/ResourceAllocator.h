#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

class VulkanContext;

class ResourceAllocator final : private NonCopyable {
public:
    struct HeapBudget {
        uint32_t heapIndex = 0;
        uint64_t usageBytes = 0;
        uint64_t budgetBytes = 0;
        uint64_t allocationBytes = 0;
        uint64_t blockBytes = 0;
        uint32_t allocationCount = 0;
        uint32_t blockCount = 0;
        float usageRatio = 0.0f;
        std::string pressure;
    };

    struct MemoryBudgetReport {
        bool supported = false;
        uint64_t totalUsageBytes = 0;
        uint64_t totalBudgetBytes = 0;
        uint64_t totalAllocationBytes = 0;
        uint64_t totalBlockBytes = 0;
        uint64_t peakUsageBytes = 0;
        int64_t usageDeltaBytes = 0;
        uint32_t allocationCount = 0;
        uint32_t blockCount = 0;
        float maxUsageRatio = 0.0f;
        std::string pressure;
        bool overrideActive = false;
        std::vector<HeapBudget> heaps;
        std::vector<std::string> warnings;
    };

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
    [[nodiscard]] bool supportsMemoryBudget() const { return supportsMemoryBudget_; }
    [[nodiscard]] MemoryBudgetReport memoryBudgetReport() const;

    void setDebugName(VkObjectType objectType, uint64_t objectHandle, const char* name) const;

private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    bool supportsDeviceAddress_ = false;
    bool supportsSamplerAnisotropy_ = false;
    bool supportsMemoryBudget_ = false;
    float maxSamplerAnisotropy_ = 1.0f;
    VkSharingMode graphicsComputeSharingMode_ = VK_SHARING_MODE_EXCLUSIVE;
    uint32_t graphicsComputeQueueFamilyCount_ = 0;
    std::array<uint32_t, 2> graphicsComputeQueueFamilies_{};
    mutable uint64_t previousBudgetUsageBytes_ = 0;
    mutable uint64_t peakBudgetUsageBytes_ = 0;
    mutable bool hasPreviousBudgetUsage_ = false;
};

} // namespace rtv
