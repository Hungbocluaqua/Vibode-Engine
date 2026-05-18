#include "rtv/DescriptorLayoutCache.h"

#include "rtv/Check.h"

#include <algorithm>
#include <tuple>

namespace rtv {

DescriptorLayoutCache::DescriptorLayoutCache(VkDevice device)
    : device_(device) {}

DescriptorLayoutCache::~DescriptorLayoutCache() {
    for (const auto& [key, layout] : cache_) {
        (void)key;
        vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
}

VkDescriptorSetLayout DescriptorLayoutCache::createLayout(
    std::vector<VkDescriptorSetLayoutBinding> bindings,
    VkDescriptorSetLayoutCreateFlags flags,
    std::vector<VkDescriptorBindingFlags> bindingFlags) {
    std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) {
        return a.binding < b.binding;
    });
    if (!bindingFlags.empty() && bindingFlags.size() != bindings.size()) {
        std::vector<VkDescriptorBindingFlags> sortedFlags(bindings.size(), 0);
        for (size_t i = 0; i < bindings.size() && i < bindingFlags.size(); ++i) {
            sortedFlags[i] = bindingFlags[i];
        }
        bindingFlags = std::move(sortedFlags);
    }
    if (bindingFlags.empty()) {
        bindingFlags.resize(bindings.size(), 0);
    }

    LayoutKey key{bindings, flags, bindingFlags};
    const auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
    bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    bindingFlagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.pNext = bindingFlags.empty() ? nullptr : &bindingFlagsInfo;
    createInfo.flags = flags;
    createInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    createInfo.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorSetLayout(device_, &createInfo, nullptr, &layout), "vkCreateDescriptorSetLayout");
    cache_.emplace(std::move(key), layout);
    return layout;
}

bool DescriptorLayoutCache::LayoutKey::operator<(const LayoutKey& other) const {
    if (flags != other.flags) {
        return flags < other.flags;
    }
    if (bindings.size() != other.bindings.size()) {
        return bindings.size() < other.bindings.size();
    }
    if (bindingFlags != other.bindingFlags) {
        return bindingFlags < other.bindingFlags;
    }
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto a = std::tie(bindings[i].binding, bindings[i].descriptorType, bindings[i].descriptorCount, bindings[i].stageFlags);
        const auto b = std::tie(other.bindings[i].binding, other.bindings[i].descriptorType, other.bindings[i].descriptorCount, other.bindings[i].stageFlags);
        if (a != b) {
            return a < b;
        }
    }
    return false;
}

} // namespace rtv
