#include "rtv/FrameResources.h"

namespace rtv {

FrameResources::FrameResources(VkDevice device, ResourceAllocator& allocator, VkDeviceSize transientUniformBytes)
    : descriptors_(device),
      uniformRing_(allocator, {
          .size = transientUniformBytes,
          .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          .memory = BufferMemory::Upload,
          .persistentMapped = true,
          .debugName = "frame uniform ring",
      }) {}

void FrameResources::beginFrame() {
    descriptors_.resetPools();
}

} // namespace rtv
