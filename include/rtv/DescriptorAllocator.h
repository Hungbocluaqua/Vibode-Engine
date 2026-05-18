#pragma once

#include "rtv/DescriptorSet.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <vector>

namespace rtv {

class DescriptorAllocator final : private NonCopyable {
public:
    explicit DescriptorAllocator(VkDevice device, uint32_t setsPerPool = 256);
    ~DescriptorAllocator();

    DescriptorSet allocate(VkDescriptorSetLayout layout);
    void resetPools();

private:
    VkDescriptorPool createPool();

    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t setsPerPool_ = 256;
    VkDescriptorPool currentPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> usedPools_;
    std::vector<VkDescriptorPool> freePools_;
};

} // namespace rtv
