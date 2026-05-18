#pragma once

#include "rtv/DescriptorSet.h"

#include <Volk/volk.h>

#include <vector>
#include <cstdint>

namespace rtv {

class DescriptorWriter {
public:
    DescriptorWriter& writeBuffer(uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo& bufferInfo);
    DescriptorWriter& writeImage(uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo& imageInfo);
    DescriptorWriter& writeImageArray(uint32_t binding, VkDescriptorType type, const std::vector<VkDescriptorImageInfo>& imageInfos);
    DescriptorWriter& writeAccelerationStructure(uint32_t binding, VkAccelerationStructureKHR accelerationStructure);
    void update(VkDevice device, DescriptorSet set) const;

private:
    struct PendingWrite {
        uint32_t binding = 0;
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        uint32_t index = 0;
        uint32_t count = 1;
        enum class Kind : uint32_t {
            Buffer,
            Image,
            AccelerationStructure,
        } kind = Kind::Buffer;
    };

    std::vector<VkDescriptorBufferInfo> buffers_;
    std::vector<VkDescriptorImageInfo> images_;
    std::vector<VkAccelerationStructureKHR> accelerationStructures_;
    std::vector<PendingWrite> writes_;
};

} // namespace rtv
