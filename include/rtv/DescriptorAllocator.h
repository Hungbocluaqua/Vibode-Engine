#pragma once

#include "rtv/DescriptorSet.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <vector>

namespace rtv {

class DescriptorAllocator final : private NonCopyable {
public:
    struct Stats {
        uint32_t setsPerPool = 0;
        uint32_t maxPools = 0;
        uint32_t usedPools = 0;
        uint32_t freePools = 0;
        uint32_t poolCount = 0;
        uint32_t capacitySets = 0;
        uint32_t allocatedSets = 0;
        uint32_t peakAllocatedSets = 0;
        uint32_t failedAllocations = 0;
        uint32_t fragmentedPoolFailures = 0;
        uint32_t poolGrowthCount = 0;
    };

    explicit DescriptorAllocator(VkDevice device, uint32_t setsPerPool = 256);
    ~DescriptorAllocator();

    DescriptorSet allocate(VkDescriptorSetLayout layout);
    void resetPools();
    [[nodiscard]] Stats stats() const;

private:
    VkDescriptorPool createPool();

    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t setsPerPool_ = 256;
    uint32_t maxPools_ = 64;
    VkDescriptorPool currentPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> usedPools_;
    std::vector<VkDescriptorPool> freePools_;
    uint32_t allocatedSets_ = 0;
    uint32_t peakAllocatedSets_ = 0;
    uint32_t failedAllocations_ = 0;
    uint32_t fragmentedPoolFailures_ = 0;
    uint32_t poolGrowthCount_ = 0;
};

} // namespace rtv
