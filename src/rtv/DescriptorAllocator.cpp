#include "rtv/DescriptorAllocator.h"

#include "rtv/Check.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>

namespace rtv {

DescriptorAllocator::DescriptorAllocator(VkDevice device, uint32_t setsPerPool)
    : device_(device), setsPerPool_(setsPerPool) {}

DescriptorAllocator::~DescriptorAllocator() {
    for (VkDescriptorPool pool : usedPools_) {
        vkDestroyDescriptorPool(device_, pool, nullptr);
    }
    for (VkDescriptorPool pool : freePools_) {
        vkDestroyDescriptorPool(device_, pool, nullptr);
    }
}

DescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    if (currentPool_ == VK_NULL_HANDLE) {
        currentPool_ = freePools_.empty() ? createPool() : freePools_.back();
        if (!freePools_.empty()) {
            freePools_.pop_back();
        }
        usedPools_.push_back(currentPool_);
    }

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = currentPool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(device_, &allocateInfo, &set);
    if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
        ++failedAllocations_;
        if (result == VK_ERROR_FRAGMENTED_POOL) {
            ++fragmentedPoolFailures_;
        }
        currentPool_ = createPool();
        usedPools_.push_back(currentPool_);
        allocateInfo.descriptorPool = currentPool_;
        result = vkAllocateDescriptorSets(device_, &allocateInfo, &set);
    }
    checkVk(result, "vkAllocateDescriptorSets");
    ++allocatedSets_;
    peakAllocatedSets_ = std::max(peakAllocatedSets_, allocatedSets_);
    return {set, layout};
}

void DescriptorAllocator::resetPools() {
    for (VkDescriptorPool pool : usedPools_) {
        checkVk(vkResetDescriptorPool(device_, pool, 0), "vkResetDescriptorPool");
        freePools_.push_back(pool);
    }
    usedPools_.clear();
    currentPool_ = VK_NULL_HANDLE;
    allocatedSets_ = 0;
}

DescriptorAllocator::Stats DescriptorAllocator::stats() const {
    const uint32_t poolCount = static_cast<uint32_t>(usedPools_.size() + freePools_.size());
    return {
        .setsPerPool = setsPerPool_,
        .maxPools = maxPools_,
        .usedPools = static_cast<uint32_t>(usedPools_.size()),
        .freePools = static_cast<uint32_t>(freePools_.size()),
        .poolCount = poolCount,
        .capacitySets = poolCount * setsPerPool_,
        .allocatedSets = allocatedSets_,
        .peakAllocatedSets = peakAllocatedSets_,
        .failedAllocations = failedAllocations_,
        .fragmentedPoolFailures = fragmentedPoolFailures_,
        .poolGrowthCount = poolGrowthCount_,
    };
}

VkDescriptorPool DescriptorAllocator::createPool() {
    const uint32_t poolCount = static_cast<uint32_t>(usedPools_.size() + freePools_.size());
    if (poolCount >= maxPools_) {
        std::ostringstream message;
        message << "Descriptor pool hard cap reached (" << maxPools_
                << " pools, " << setsPerPool_ << " sets per pool)";
        throw std::runtime_error(message.str());
    }

    // One renderer frame typically allocates one bindless ray tracing set plus
    // a small number of pass-local sets. Keep pool sizes tied to those feature
    // counts instead of multiplying every set by the bindless array length.
    constexpr uint32_t kBindlessCombinedImageSamplers = 1024;
    const std::array<VkDescriptorPoolSize, 7> sizes = {{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setsPerPool_ * 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setsPerPool_ * 32},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kBindlessCombinedImageSamplers + setsPerPool_ * 16},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, setsPerPool_ * 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setsPerPool_ * 8},
        {VK_DESCRIPTOR_TYPE_SAMPLER, setsPerPool_ * 4},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, setsPerPool_ * 2},
    }};

    VkDescriptorPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    createInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    createInfo.maxSets = setsPerPool_;
    createInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    createInfo.pPoolSizes = sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorPool(device_, &createInfo, nullptr, &pool), "vkCreateDescriptorPool");
    ++poolGrowthCount_;
    return pool;
}

} // namespace rtv
