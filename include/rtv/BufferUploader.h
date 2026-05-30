#pragma once

#include "rtv/Image.h"
#include "rtv/TextureAsset.h"

#include <Volk/volk.h>

#include <cstddef>
#include <span>

namespace rtv {

class Buffer;
class ResourceAllocator;
class UploadContext;

class BufferUploader {
public:
    struct Stats {
        uint64_t totalUploadedBytes = 0;
        uint64_t stagingPeakBytes = 0;
        uint64_t lastStagingBytes = 0;
        uint32_t uploadCount = 0;
        uint32_t bufferUploadCount = 0;
        uint32_t imageUploadCount = 0;
        uint32_t batchUploadCount = 0;
    };

    BufferUploader(ResourceAllocator& allocator, UploadContext& uploadContext);

    void uploadToBuffer(Buffer& destination, const void* data, VkDeviceSize byteSize, VkDeviceSize dstOffset = 0);
    void uploadToImage2D(Image& image, const void* rgbaBytes, VkDeviceSize byteSize, VkImageLayout finalLayout);
    void uploadToImage2D(
        Image& image,
        const void* bytes,
        VkDeviceSize byteSize,
        std::span<const TextureMipLevel> mipData,
        VkImageLayout finalLayout);

    template <typename T>
    void uploadToBuffer(Buffer& destination, std::span<const T> values, VkDeviceSize dstOffset = 0) {
        uploadToBuffer(destination, values.data(), sizeof(T) * values.size(), dstOffset);
    }

    [[nodiscard]] ResourceAllocator& allocator() { return allocator_; }
    [[nodiscard]] UploadContext& uploadContext() { return uploadContext_; }
    [[nodiscard]] const Stats& stats() const { return stats_; }
    void recordBatchUpload(VkDeviceSize byteSize);

private:
    void recordUpload(VkDeviceSize byteSize);

    ResourceAllocator& allocator_;
    UploadContext& uploadContext_;
    Stats stats_{};
};

} // namespace rtv
