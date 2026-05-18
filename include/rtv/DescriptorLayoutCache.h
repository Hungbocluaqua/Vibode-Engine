#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <map>
#include <vector>

namespace rtv {

class DescriptorLayoutCache final : private NonCopyable {
public:
    explicit DescriptorLayoutCache(VkDevice device);
    ~DescriptorLayoutCache();

    VkDescriptorSetLayout createLayout(
        std::vector<VkDescriptorSetLayoutBinding> bindings,
        VkDescriptorSetLayoutCreateFlags flags = 0,
        std::vector<VkDescriptorBindingFlags> bindingFlags = {});

private:
    struct LayoutKey {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        VkDescriptorSetLayoutCreateFlags flags = 0;
        std::vector<VkDescriptorBindingFlags> bindingFlags;

        [[nodiscard]] bool operator<(const LayoutKey& other) const;
    };

    VkDevice device_ = VK_NULL_HANDLE;
    std::map<LayoutKey, VkDescriptorSetLayout> cache_;
};

} // namespace rtv
