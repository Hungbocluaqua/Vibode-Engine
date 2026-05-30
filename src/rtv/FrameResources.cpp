#include "rtv/FrameResources.h"

#include <algorithm>
#include <stdexcept>

namespace rtv {

FrameResources::FrameResources(VkDevice device, ResourceAllocator& allocator, VkDeviceSize transientUniformBytes)
    : allocator_(allocator),
      descriptors_(device),
      uniformRing_(allocator, {
          .size = transientUniformBytes,
          .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          .memory = BufferMemory::Upload,
          .persistentMapped = true,
          .debugName = "frame uniform ring",
      }) {}

void FrameResources::beginFrame() {
    descriptors_.resetPools();
    wavefrontTransientOffset_ = 0;
}

Buffer FrameResources::allocateWavefrontTransientBuffer(const BufferDesc& desc, VkDeviceSize alignment) {
    if (desc.size == 0) {
        throw std::runtime_error("Wavefront transient buffer size must be greater than zero");
    }
    const VkDeviceSize alignedOffset = Buffer::alignUp(wavefrontTransientOffset_, std::max<VkDeviceSize>(alignment, 1));
    const VkDeviceSize requiredBytes = alignedOffset + desc.size;
    ensureWavefrontTransientCapacity(requiredBytes);

    Buffer alias;
    alias.aliasFrom(wavefrontTransientArena_, desc, alignedOffset);
    wavefrontTransientOffset_ = requiredBytes;
    wavefrontTransientHighWater_ = std::max(wavefrontTransientHighWater_, wavefrontTransientOffset_);
    return alias;
}

void FrameResources::reserveWavefrontTransientBytes(VkDeviceSize requiredBytes) {
    ensureWavefrontTransientCapacity(requiredBytes);
}

void FrameResources::ensureWavefrontTransientCapacity(VkDeviceSize requiredBytes) {
    if (requiredBytes <= wavefrontTransientArena_.size()) {
        return;
    }
    VkDeviceSize nextCapacity = std::max<VkDeviceSize>(requiredBytes, 1024 * 1024);
    if (wavefrontTransientArena_.handle() != VK_NULL_HANDLE) {
        nextCapacity = std::max(nextCapacity, wavefrontTransientArena_.size() * 2);
    }
    nextCapacity = Buffer::alignUp(nextCapacity, 256);
    wavefrontTransientUsage_ =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    wavefrontTransientArena_.create(allocator_, BufferDesc{
        .size = nextCapacity,
        .usage = wavefrontTransientUsage_,
        .memory = BufferMemory::GpuOnly,
        .debugName = "frame wavefront transient arena",
    });
}

} // namespace rtv
