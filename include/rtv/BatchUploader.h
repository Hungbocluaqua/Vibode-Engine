#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <cstdint>
#include <vector>

namespace rtv {

class Buffer;
class Image;
class BufferUploader;
class ResourceAllocator;

class BatchUploader final : private NonCopyable {
public:
    explicit BatchUploader(BufferUploader& uploader);
    ~BatchUploader();

    void begin();
    void enqueueBufferUpload(Buffer& destination, const void* data, VkDeviceSize byteSize, VkDeviceSize dstOffset = 0);
    void enqueueImageUpload(Image& image, const void* data, VkDeviceSize byteSize, VkImageLayout finalLayout);
    void submit();
    void reset();

    [[nodiscard]] ResourceAllocator& allocator();

private:
    struct PendingBufferOp {
        Buffer* destination = nullptr;
        const void* data = nullptr;
        VkDeviceSize byteSize = 0;
        VkDeviceSize dstOffset = 0;
    };

    struct PendingImageOp {
        Image* image = nullptr;
        const void* data = nullptr;
        VkDeviceSize byteSize = 0;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };

    BufferUploader& uploader_;
    std::vector<PendingBufferOp> pendingBuffers_;
    std::vector<PendingImageOp> pendingImages_;
    bool recording_ = false;
};

} // namespace rtv
