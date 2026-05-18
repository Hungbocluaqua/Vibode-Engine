#include "rtv/BufferUploader.h"

#include "rtv/Buffer.h"
#include "rtv/ImageBarrier.h"
#include "rtv/UploadContext.h"

#include <cstring>
#include <stdexcept>

namespace rtv {

BufferUploader::BufferUploader(ResourceAllocator& allocator, UploadContext& uploadContext)
    : allocator_(allocator), uploadContext_(uploadContext) {}

void BufferUploader::uploadToBuffer(Buffer& destination, const void* data, VkDeviceSize byteSize, VkDeviceSize dstOffset) {
    if (data == nullptr || byteSize == 0) {
        return;
    }
    if ((destination.usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0) {
        throw std::runtime_error("Destination buffer must include VK_BUFFER_USAGE_TRANSFER_DST_BIT");
    }
    if (dstOffset + byteSize > destination.size()) {
        throw std::runtime_error("Upload exceeds destination buffer size");
    }

    Buffer staging(allocator_, {
        .size = byteSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "upload staging buffer",
    });
    staging.write(data, byteSize);
    staging.flush(byteSize);

    VkCommandBuffer cmd = uploadContext_.begin();
    VkBufferCopy copy{};
    copy.size = byteSize;
    copy.dstOffset = dstOffset;
    vkCmdCopyBuffer(cmd, staging.handle(), destination.handle(), 1, &copy);

    barrier::cmdBufferBarrier(cmd, {
        .buffer = destination.handle(),
        .offset = dstOffset,
        .size = byteSize,
        .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    });
    uploadContext_.submitAndWait(cmd);
}

void BufferUploader::uploadToImage2D(Image& image, const void* rgbaBytes, VkDeviceSize byteSize, VkImageLayout finalLayout) {
    if (rgbaBytes == nullptr || byteSize == 0) {
        return;
    }

    Buffer staging(allocator_, {
        .size = byteSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "texture upload staging buffer",
    });
    staging.write(rgbaBytes, byteSize);
    staging.flush(byteSize);

    VkCommandBuffer cmd = uploadContext_.begin();
    barrier::cmdTransitionImage(cmd, {
        .image = image.handle(),
        .oldLayout = image.layout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .range = image.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_NONE,
        .srcAccess = VK_ACCESS_2_NONE,
        .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    });
    image.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = image.extent();
    vkCmdCopyBufferToImage(cmd, staging.handle(), image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    if (image.mipLevels() > 1) {
        image.generateMipmaps(cmd);
    }

    barrier::cmdTransitionImage(cmd, {
        .image = image.handle(),
        .oldLayout = image.layout(),
        .newLayout = finalLayout,
        .range = image.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    });
    image.setLayout(finalLayout);

    uploadContext_.submitAndWait(cmd);
}

} // namespace rtv
