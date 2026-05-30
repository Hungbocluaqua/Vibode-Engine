#include "rtv/BufferUploader.h"

#include "rtv/Buffer.h"
#include "rtv/ImageBarrier.h"
#include "rtv/UploadContext.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace rtv {

namespace {

std::vector<VkBufferImageCopy> makeMipCopyRegions(std::span<const TextureMipLevel> mipData, uint32_t imageMipLevels) {
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mipData.size());
    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(mipData.size()), imageMipLevels);
    for (uint32_t mip = 0; mip < count; ++mip) {
        const TextureMipLevel& level = mipData[mip];
        if (level.size == 0) {
            continue;
        }
        VkBufferImageCopy copy{};
        copy.bufferOffset = static_cast<VkDeviceSize>(level.offset);
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = mip;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {
            std::max(level.width, 1u),
            std::max(level.height, 1u),
            1u,
        };
        regions.push_back(copy);
    }
    return regions;
}

} // namespace

BufferUploader::BufferUploader(ResourceAllocator& allocator, UploadContext& uploadContext)
    : allocator_(allocator), uploadContext_(uploadContext) {}

void BufferUploader::recordUpload(VkDeviceSize byteSize) {
    stats_.totalUploadedBytes += static_cast<uint64_t>(byteSize);
    stats_.lastStagingBytes = static_cast<uint64_t>(byteSize);
    stats_.stagingPeakBytes = std::max(stats_.stagingPeakBytes, stats_.lastStagingBytes);
    ++stats_.uploadCount;
}

void BufferUploader::recordBatchUpload(VkDeviceSize byteSize) {
    if (byteSize == 0) {
        return;
    }
    recordUpload(byteSize);
    ++stats_.batchUploadCount;
}

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
    recordUpload(byteSize);
    ++stats_.bufferUploadCount;
}

void BufferUploader::uploadToImage2D(Image& image, const void* rgbaBytes, VkDeviceSize byteSize, VkImageLayout finalLayout) {
    uploadToImage2D(image, rgbaBytes, byteSize, {}, finalLayout);
}

void BufferUploader::uploadToImage2D(
    Image& image,
    const void* rgbaBytes,
    VkDeviceSize byteSize,
    std::span<const TextureMipLevel> mipData,
    VkImageLayout finalLayout) {
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

    std::vector<VkBufferImageCopy> copies = makeMipCopyRegions(mipData, image.mipLevels());
    VkBufferImageCopy baseCopy{};
    if (copies.empty()) {
        baseCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        baseCopy.imageSubresource.layerCount = 1;
        baseCopy.imageExtent = image.extent();
        copies.push_back(baseCopy);
    }
    vkCmdCopyBufferToImage(
        cmd,
        staging.handle(),
        image.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(copies.size()),
        copies.data());

    if (image.mipLevels() > 1 && mipData.empty()) {
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
    recordUpload(byteSize);
    ++stats_.imageUploadCount;
}

} // namespace rtv
