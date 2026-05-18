#pragma once

#include "rtv/Buffer.h"
#include "rtv/Image.h"

#include <Volk/volk.h>

namespace rtv {

class BufferUploader;
class ResourceAllocator;
class UploadContext;

class ResourceDemo {
public:
    ResourceDemo(ResourceAllocator& allocator, BufferUploader& uploader);

    void record(VkCommandBuffer commandBuffer, VkImage swapchainImage, VkExtent2D swapchainExtent);

private:
    Image uploadedTexture_;
    Image storageImage_;
    Buffer storageBuffer_;
};

} // namespace rtv
