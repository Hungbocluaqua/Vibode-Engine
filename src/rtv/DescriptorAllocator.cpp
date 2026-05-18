#include "rtv/DescriptorAllocator.h"

#include "rtv/Check.h"

#include <array>
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
        currentPool_ = createPool();
        usedPools_.push_back(currentPool_);
        allocateInfo.descriptorPool = currentPool_;
        result = vkAllocateDescriptorSets(device_, &allocateInfo, &set);
    }
    checkVk(result, "vkAllocateDescriptorSets");
    return {set, layout};
}

void DescriptorAllocator::resetPools() {
    for (VkDescriptorPool pool : usedPools_) {
        checkVk(vkResetDescriptorPool(device_, pool, 0), "vkResetDescriptorPool");
        freePools_.push_back(pool);
    }
    usedPools_.clear();
    currentPool_ = VK_NULL_HANDLE;
}

VkDescriptorPool DescriptorAllocator::createPool() {
    const std::array<VkDescriptorPoolSize, 7> sizes = {{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setsPerPool_},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setsPerPool_ * 4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setsPerPool_},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, setsPerPool_ * 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setsPerPool_ * 4},
        {VK_DESCRIPTOR_TYPE_SAMPLER, setsPerPool_},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, setsPerPool_},
    }};

    VkDescriptorPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    createInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    createInfo.maxSets = setsPerPool_;
    createInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    createInfo.pPoolSizes = sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorPool(device_, &createInfo, nullptr, &pool), "vkCreateDescriptorPool");
    return pool;
}

} // namespace rtv
