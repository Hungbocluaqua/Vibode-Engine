#pragma once

#include <Volk/volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace rtv {

class ResourceAllocator;

struct ImageDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool createDefaultView = true;
    const char* debugName = nullptr;
};

class Image {
public:
    Image() = default;
    Image(ResourceAllocator& allocator, const ImageDesc& desc);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    void create(ResourceAllocator& allocator, const ImageDesc& desc);
    void destroy();
    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkImage handle() const { return image_; }
    [[nodiscard]] VkImageView view() const { return view_; }
    [[nodiscard]] VkFormat format() const { return desc_.format; }
    [[nodiscard]] VkExtent3D extent() const { return {desc_.width, desc_.height, desc_.depth}; }
    [[nodiscard]] uint32_t width() const { return desc_.width; }
    [[nodiscard]] uint32_t height() const { return desc_.height; }
    [[nodiscard]] uint32_t mipLevels() const { return desc_.mipLevels; }
    [[nodiscard]] VkImageLayout layout() const { return layout_; }
    [[nodiscard]] VkImageSubresourceRange fullRange() const;
    [[nodiscard]] VkDescriptorImageInfo sampledDescriptor(VkSampler sampler) const;
    [[nodiscard]] VkDescriptorImageInfo storageDescriptor() const;

    void setLayout(VkImageLayout layout) const { layout_ = layout; }
    void generateMipmaps(VkCommandBuffer commandBuffer);

private:
    ResourceAllocator* allocator_ = nullptr;
    ImageDesc desc_{};
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    mutable VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
private:
};

} // namespace rtv
