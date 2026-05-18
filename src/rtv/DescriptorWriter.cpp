#include "rtv/DescriptorWriter.h"

namespace rtv {

DescriptorWriter& DescriptorWriter::writeBuffer(uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo& bufferInfo) {
    buffers_.push_back(bufferInfo);

    writes_.push_back({binding, type, static_cast<uint32_t>(buffers_.size() - 1), 1, PendingWrite::Kind::Buffer});
    return *this;
}

DescriptorWriter& DescriptorWriter::writeImage(uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo& imageInfo) {
    images_.push_back(imageInfo);

    writes_.push_back({binding, type, static_cast<uint32_t>(images_.size() - 1), 1, PendingWrite::Kind::Image});
    return *this;
}

DescriptorWriter& DescriptorWriter::writeImageArray(uint32_t binding, VkDescriptorType type, const std::vector<VkDescriptorImageInfo>& imageInfos) {
    const uint32_t first = static_cast<uint32_t>(images_.size());
    images_.insert(images_.end(), imageInfos.begin(), imageInfos.end());
    writes_.push_back({binding, type, first, static_cast<uint32_t>(imageInfos.size()), PendingWrite::Kind::Image});
    return *this;
}

DescriptorWriter& DescriptorWriter::writeAccelerationStructure(uint32_t binding, VkAccelerationStructureKHR accelerationStructure) {
    accelerationStructures_.push_back(accelerationStructure);
    writes_.push_back({
        binding,
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        static_cast<uint32_t>(accelerationStructures_.size() - 1),
        1,
        PendingWrite::Kind::AccelerationStructure,
    });
    return *this;
}

void DescriptorWriter::update(VkDevice device, DescriptorSet set) const {
    std::vector<VkWriteDescriptorSet> patched;
    patched.reserve(writes_.size());
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelerationStructureWrites;
    accelerationStructureWrites.reserve(accelerationStructures_.size());
    for (const PendingWrite& pending : writes_) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set.handle();
        write.dstBinding = pending.binding;
        write.descriptorCount = pending.count;
        write.descriptorType = pending.type;
        if (pending.kind == PendingWrite::Kind::Image) {
            write.pImageInfo = &images_.at(pending.index);
        } else if (pending.kind == PendingWrite::Kind::Buffer) {
            write.pBufferInfo = &buffers_.at(pending.index);
        } else {
            VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
            asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            asWrite.accelerationStructureCount = pending.count;
            asWrite.pAccelerationStructures = &accelerationStructures_.at(pending.index);
            accelerationStructureWrites.push_back(asWrite);
            write.pNext = &accelerationStructureWrites.back();
        }
        patched.push_back(write);
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(patched.size()), patched.data(), 0, nullptr);
}

} // namespace rtv
