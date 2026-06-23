#include "rtv/BatchUploader.h"

#include "rtv/Buffer.h"
#include "rtv/BufferUploader.h"
#include "rtv/Image.h"
#include "rtv/ImageBarrier.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/UploadContext.h"

#include <algorithm>
#include <cstring>
#include <iostream>
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

BatchUploader::BatchUploader(BufferUploader& uploader, VkDeviceSize maxStagingBytes)
    : uploader_(uploader),
      maxStagingBytes_(std::max<VkDeviceSize>(maxStagingBytes, 4ull * 1024ull * 1024ull)) {}

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

    pendingBuffers_.push_back(PendingBufferOp{
        .destination = &destination,
        .data = data,
        .byteSize = byteSize,
        .dstOffset = dstOffset,
    });
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

    pendingImages_.push_back(PendingImageOp{
        .image = &image,
        .data = data,
        .byteSize = byteSize,
        .mipData = std::move(mipData),
        .finalLayout = finalLayout,
    });
}

void BatchUploader::submit() {
    if (!recording_) {
        return;
    }

    if (pendingBuffers_.empty() && pendingImages_.empty()) {
        reset();
        return;
    }

    struct ChunkBufferOp {
        const PendingBufferOp* op = nullptr;
        VkDeviceSize srcOffset = 0;
        VkDeviceSize byteSize = 0;
        VkDeviceSize stagingOffset = 0;
    };
    struct ChunkImageOp {
        const PendingImageOp* op = nullptr;
        VkDeviceSize stagingOffset = 0;
    };

    std::vector<ChunkBufferOp> chunkBuffers;
    std::vector<ChunkImageOp> chunkImages;
    VkDeviceSize chunkSize = 0;
    uint32_t submittedChunks = 0;

    auto flushChunk = [&]() {
        if (chunkSize == 0) {
            return;
        }

        Buffer staging(uploader_.allocator(), {
            .size = chunkSize,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .memory = BufferMemory::Upload,
            .persistentMapped = true,
            .debugName = "batch upload staging buffer",
        });

        uint8_t* mappedData = static_cast<uint8_t*>(staging.mappedData());
        for (const ChunkBufferOp& chunk : chunkBuffers) {
            const auto* bytes = static_cast<const uint8_t*>(chunk.op->data);
            std::memcpy(
                mappedData + chunk.stagingOffset,
                bytes + chunk.srcOffset,
                static_cast<size_t>(chunk.byteSize));
        }
        for (const ChunkImageOp& chunk : chunkImages) {
            std::memcpy(
                mappedData + chunk.stagingOffset,
                chunk.op->data,
                static_cast<size_t>(chunk.op->byteSize));
        }

        staging.flush(chunkSize);

        VkCommandBuffer cmd = uploader_.uploadContext().begin();

        for (const ChunkBufferOp& chunk : chunkBuffers) {
            const PendingBufferOp& op = *chunk.op;
        VkBufferCopy copy{};
            copy.size = chunk.byteSize;
            copy.srcOffset = chunk.stagingOffset;
            copy.dstOffset = op.dstOffset + chunk.srcOffset;
        vkCmdCopyBuffer(cmd, staging.handle(), op.destination->handle(), 1, &copy);

        barrier::cmdBufferBarrier(cmd, {
            .buffer = op.destination->handle(),
                .offset = op.dstOffset + chunk.srcOffset,
                .size = chunk.byteSize,
            .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        });
    }

        for (const ChunkImageOp& chunk : chunkImages) {
            const PendingImageOp& op = *chunk.op;
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
                chunk.stagingOffset,
            op.image->extent());
        vkCmdCopyBufferToImage(
            cmd,
            staging.handle(),
            op.image->handle(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(copies.size()),
            copies.data());

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
        uploader_.recordBatchUpload(chunkSize);
        ++submittedChunks;

        chunkBuffers.clear();
        chunkImages.clear();
        chunkSize = 0;
    };

    auto reserveBytes = [&](VkDeviceSize bytes) {
        if (chunkSize > 0 && chunkSize + bytes > maxStagingBytes_) {
            flushChunk();
        }
    };

    for (const PendingBufferOp& op : pendingBuffers_) {
        VkDeviceSize consumed = 0;
        while (consumed < op.byteSize) {
            if (chunkSize == maxStagingBytes_) {
                flushChunk();
            }
            const VkDeviceSize available = maxStagingBytes_ - chunkSize;
            const VkDeviceSize bytes = std::min(op.byteSize - consumed, available);
            chunkBuffers.push_back(ChunkBufferOp{
                .op = &op,
                .srcOffset = consumed,
                .byteSize = bytes,
                .stagingOffset = chunkSize,
            });
            chunkSize += bytes;
            consumed += bytes;
        }
    }

    for (const PendingImageOp& op : pendingImages_) {
        reserveBytes(op.byteSize);
        if (op.byteSize > maxStagingBytes_ && chunkSize > 0) {
            flushChunk();
        }
        chunkImages.push_back(ChunkImageOp{
            .op = &op,
            .stagingOffset = chunkSize,
        });
        chunkSize += op.byteSize;
        if (chunkSize >= maxStagingBytes_) {
            flushChunk();
        }
    }

    flushChunk();
    if (submittedChunks > 1) {
        std::cout << "Batch upload chunked: chunks=" << submittedChunks
                  << " max_chunk_bytes=" << maxStagingBytes_ << '\n';
    }
    reset();
}

void BatchUploader::reset() {
    pendingBuffers_.clear();
    pendingImages_.clear();
    recording_ = false;
}

} // namespace rtv
