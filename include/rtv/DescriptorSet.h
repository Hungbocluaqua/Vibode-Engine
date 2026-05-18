#pragma once

#include <Volk/volk.h>

namespace rtv {

class DescriptorSet {
public:
    DescriptorSet() = default;
    DescriptorSet(VkDescriptorSet set, VkDescriptorSetLayout layout)
        : set_(set), layout_(layout) {}

    [[nodiscard]] VkDescriptorSet handle() const { return set_; }
    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_; }
    [[nodiscard]] explicit operator bool() const { return set_ != VK_NULL_HANDLE; }

private:
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
};

} // namespace rtv
