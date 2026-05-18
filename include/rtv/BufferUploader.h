#pragma once

#include "rtv/Image.h"

#include <Volk/volk.h>

#include <cstddef>
#include <span>

namespace rtv {

class Buffer;
class ResourceAllocator;
class UploadContext;

class BufferUploader {
public:
    BufferUploader(ResourceAllocator& allocator, UploadContext& uploadContext);

    void uploadToBuffer(Buffer& destination, const void* data, VkDeviceSize byteSize, VkDeviceSize dstOffset = 0);
    void uploadToImage2D(Image& image, const void* rgbaBytes, VkDeviceSize byteSize, VkImageLayout finalLayout);

    template <typename T>
    void uploadToBuffer(Buffer& destination, std::span<const T> values, VkDeviceSize dstOffset = 0) {
        uploadToBuffer(destination, values.data(), sizeof(T) * values.size(), dstOffset);
    }

    [[nodiscard]] ResourceAllocator& allocator() { return allocator_; }
    [[nodiscard]] UploadContext& uploadContext() { return uploadContext_; }

private:
    ResourceAllocator& allocator_;
    UploadContext& uploadContext_;
};

} // namespace rtv
