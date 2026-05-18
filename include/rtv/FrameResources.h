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
    Buffer& uniformRing() { return uniformRing_; }

private:
    DescriptorAllocator descriptors_;
    Buffer uniformRing_;
};

} // namespace rtv
