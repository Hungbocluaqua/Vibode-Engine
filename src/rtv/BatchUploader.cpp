#include "rtv/BatchUploader.h"

#include "rtv/Buffer.h"
#include "rtv/BufferUploader.h"
#include "rtv/Image.h"
#include "rtv/ImageBarrier.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/UploadContext.h"

#include <cstring>
#include <stdexcept>

namespace rtv {

BatchUploader::BatchUploader(BufferUploader& uploader)
    : uploader_(uploader) {}

BatchUploader::~BatchUploader() {
    reset();
}

ResourceAllocator& BatchUploader::allocator() {
    return uploader_.allocator();
}

void BatchUploader::begin() {
    pendingBuffers_.clear();
    pendingImages_.clear();
    recording_ = true;
}

void BatchUploader::enqueueBufferUpload(Buffer& destination, const void* data, VkDeviceSize byteSize, VkDeviceSize dstOffset) {
    if (data == nullptr || byteSize == 0) {
        return;
    }
    if ((destination.usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0) {
        throw std::runtime_error("Destination buffer must include VK_BUFFER_USAGE_TRANSFER_DST_BIT");
    }
    if (dstOffset + byteSize > destination.size()) {
        throw std::runtime_error("Upload exceeds destination buffer size");
    }

    PendingBufferOp op;
    op.destination = &destination;
    op.byteSize = byteSize;
    op.dstOffset = dstOffset;
    op.data = new uint8_t[static_cast<size_t>(byteSize)];
    std::memcpy(const_cast<void*>(op.data), data, static_cast<size_t>(byteSize));
    pendingBuffers_.push_back(op);
}

void BatchUploader::enqueueImageUpload(Image& image, const void* data, VkDeviceSize byteSize, VkImageLayout finalLayout) {
    if (data == nullptr || byteSize == 0) {
        return;
    }

    PendingImageOp op;
    op.image = &image;
    op.byteSize = byteSize;
    op.finalLayout = finalLayout;
    op.data = new uint8_t[static_cast<size_t>(byteSize)];
    std::memcpy(const_cast<void*>(op.data), data, static_cast<size_t>(byteSize));
    pendingImages_.push_back(op);
}

void BatchUploader::submit() {
    if (!recording_) {
        return;
    }

    if (pendingBuffers_.empty() && pendingImages_.empty()) {
        reset();
        return;
    }

    VkDeviceSize totalBufferSize = 0;
    for (const auto& op : pendingBuffers_) {
        totalBufferSize += op.byteSize;
    }
    for (const auto& op : pendingImages_) {
        totalBufferSize += op.byteSize;
    }

    Buffer staging(uploader_.allocator(), {
        .size = totalBufferSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "batch upload staging buffer",
    });

    uint8_t* mappedData = static_cast<uint8_t*>(staging.mappedData());

    VkDeviceSize currentOffset = 0;
    for (const auto& op : pendingBuffers_) {
        std::memcpy(mappedData + currentOffset, op.data, static_cast<size_t>(op.byteSize));
        currentOffset += op.byteSize;
    }
    for (const auto& op : pendingImages_) {
        std::memcpy(mappedData + currentOffset, op.data, static_cast<size_t>(op.byteSize));
        currentOffset += op.byteSize;
    }

    staging.flush(totalBufferSize);

    VkCommandBuffer cmd = uploader_.uploadContext().begin();

    currentOffset = 0;
    for (const auto& op : pendingBuffers_) {
        VkBufferCopy copy{};
        copy.size = op.byteSize;
        copy.srcOffset = currentOffset;
        copy.dstOffset = op.dstOffset;
        vkCmdCopyBuffer(cmd, staging.handle(), op.destination->handle(), 1, &copy);

        barrier::cmdBufferBarrier(cmd, {
            .buffer = op.destination->handle(),
            .offset = op.dstOffset,
            .size = op.byteSize,
            .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        });

        currentOffset += op.byteSize;
    }

    for (const auto& op : pendingImages_) {
        barrier::cmdTransitionImage(cmd, {
            .image = op.image->handle(),
            .oldLayout = op.image->layout(),
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .range = op.image->fullRange(),
            .srcStage = VK_PIPELINE_STAGE_2_NONE,
            .srcAccess = VK_ACCESS_2_NONE,
            .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        });
        op.image->setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy copy{};
        copy.bufferOffset = currentOffset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = op.image->extent();
        vkCmdCopyBufferToImage(cmd, staging.handle(), op.image->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        currentOffset += op.byteSize;

        if (op.image->mipLevels() > 1) {
            op.image->generateMipmaps(cmd);
        }

        barrier::cmdTransitionImage(cmd, {
            .image = op.image->handle(),
            .oldLayout = op.image->layout(),
            .newLayout = op.finalLayout,
            .range = op.image->fullRange(),
            .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
            .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        });
        op.image->setLayout(op.finalLayout);
    }

    uploader_.uploadContext().submitAndWait(cmd);
    reset();
}

void BatchUploader::reset() {
    for (auto& op : pendingBuffers_) {
        delete[] static_cast<const uint8_t*>(op.data);
        op.data = nullptr;
    }
    for (auto& op : pendingImages_) {
        delete[] static_cast<const uint8_t*>(op.data);
        op.data = nullptr;
    }
    pendingBuffers_.clear();
    pendingImages_.clear();
    recording_ = false;
}

} // namespace rtv
