#pragma once

#include "rtv/Buffer.h"
#include "rtv/DescriptorAllocator.h"

#include <Volk/volk.h>

namespace rtv {

class ResourceAllocator;

class FrameResources {
public:
    FrameResources(VkDevice device, ResourceAllocator& allocator, VkDeviceSize transientUniformBytes);

    void beginFrame();
    DescriptorAllocator& descriptors() { return descriptors_; }
    [[nodiscard]] const DescriptorAllocator& descriptors() const { return descriptors_; }
    Buffer& uniformRing() { return uniformRing_; }
    void reserveWavefrontTransientBytes(VkDeviceSize requiredBytes);
    Buffer allocateWavefrontTransientBuffer(const BufferDesc& desc, VkDeviceSize alignment = 256);
    [[nodiscard]] VkDeviceSize wavefrontTransientUsedBytes() const { return wavefrontTransientOffset_; }
    [[nodiscard]] VkDeviceSize wavefrontTransientHighWaterBytes() const { return wavefrontTransientHighWater_; }
    [[nodiscard]] VkDeviceSize wavefrontTransientCapacityBytes() const { return wavefrontTransientArena_.size(); }

private:
    void ensureWavefrontTransientCapacity(VkDeviceSize requiredBytes);

    ResourceAllocator& allocator_;
    DescriptorAllocator descriptors_;
    Buffer uniformRing_;
    Buffer wavefrontTransientArena_;
    VkDeviceSize wavefrontTransientOffset_ = 0;
    VkDeviceSize wavefrontTransientHighWater_ = 0;
    VkBufferUsageFlags wavefrontTransientUsage_ = 0;
};

} // namespace rtv
