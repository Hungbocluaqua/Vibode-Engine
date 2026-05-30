#include "rtv/BatchUploader.h"

#include "rtv/Buffer.h"
#include "rtv/BufferUploader.h"
#include "rtv/Image.h"
#include "rtv/ImageBarrier.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/UploadContext.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace rtv {

namespace {

std::vector<VkBufferImageCopy> makeMipCopyRegions(
    const std::vector<TextureMipLevel>& mipData,
    uint32_t imageMipLevels,
    VkDeviceSize baseOffset,
    VkExtent3D baseExtent) {
    std::vector<VkBufferImageCopy> regions;
    if (mipData.empty()) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = baseOffset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = baseExtent;
        regions.push_back(copy);
        return regions;
    }

    regions.reserve(mipData.size());
    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(mipData.size()), imageMipLevels);
    for (uint32_t mip = 0; mip < count; ++mip) {
        const TextureMipLevel& level = mipData[mip];
        if (level.size == 0) {
            continue;
        }
        VkBufferImageCopy copy{};
        copy.bufferOffset = baseOffset + static_cast<VkDeviceSize>(level.offset);
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
    if (regions.empty()) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = baseOffset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = baseExtent;
        regions.push_back(copy);
    }
    return regions;
}

} // namespace

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
    enqueueImageUpload(image, data, byteSize, {}, finalLayout);
}

void BatchUploader::enqueueImageUpload(
    Image& image,
    const void* data,
    VkDeviceSize byteSize,
    std::vector<TextureMipLevel> mipData,
    VkImageLayout finalLayout) {
    if (data == nullptr || byteSize == 0) {
        return;
    }

    PendingImageOp op;
    op.image = &image;
    op.byteSize = byteSize;
    op.finalLayout = finalLayout;
    op.mipData = std::move(mipData);
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

        const std::vector<VkBufferImageCopy> copies = makeMipCopyRegions(
            op.mipData,
            op.image->mipLevels(),
            currentOffset,
            op.image->extent());
        vkCmdCopyBufferToImage(
            cmd,
            staging.handle(),
            op.image->handle(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(copies.size()),
            copies.data());

        currentOffset += op.byteSize;

        if (op.image->mipLevels() > 1 && op.mipData.empty()) {
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
    uploader_.recordBatchUpload(totalBufferSize);
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
