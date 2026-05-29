#include "rtv/Image.h"

#include "rtv/Check.h"
#include "rtv/ImageBarrier.h"
#include "rtv/ResourceAllocator.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rtv {

Image::Image(ResourceAllocator& allocator, const ImageDesc& desc) {
    create(allocator, desc);
}

Image::~Image() {
    destroy();
}

Image::Image(Image&& other) noexcept {
    *this = std::move(other);
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        destroy();
        allocator_ = other.allocator_;
        desc_ = other.desc_;
        image_ = other.image_;
        allocation_ = other.allocation_;
        view_ = other.view_;
        layout_ = other.layout_;
        other.allocator_ = nullptr;
        other.image_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.view_ = VK_NULL_HANDLE;
        other.layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        other.desc_ = {};
    }
    return *this;
}

void Image::create(ResourceAllocator& allocator, const ImageDesc& desc) {
    destroy();
    allocator_ = &allocator;
    desc_ = desc;
    desc_.width = std::max(desc_.width, 1u);
    desc_.height = std::max(desc_.height, 1u);
    desc_.depth = std::max(desc_.depth, 1u);
    desc_.mipLevels = std::max(desc_.mipLevels, 1u);
    VkImageType imageType = desc_.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    VkImageViewType viewType = desc_.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = imageType;
    imageInfo.extent = {desc_.width, desc_.height, desc_.depth};
    imageInfo.mipLevels = desc_.mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = desc_.format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = desc_.initialLayout;
    imageInfo.usage = desc_.usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = allocator.graphicsComputeSharingMode();
    if (imageInfo.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        imageInfo.queueFamilyIndexCount = allocator.graphicsComputeQueueFamilyCount();
        imageInfo.pQueueFamilyIndices = allocator.graphicsComputeQueueFamilies();
    }

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    checkVk(vmaCreateImage(allocator.handle(), &imageInfo, &allocationInfo, &image_, &allocation_, nullptr), "vmaCreateImage");
    layout_ = desc_.initialLayout;
    allocator.setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image_), desc_.debugName);

    if (desc_.createDefaultView) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image_;
        viewInfo.viewType = viewType;
        viewInfo.format = desc_.format;
        viewInfo.subresourceRange = fullRange();
        checkVk(vkCreateImageView(allocator.device(), &viewInfo, nullptr, &view_), "vkCreateImageView(resource)");
        allocator.setDebugName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(view_), desc_.debugName);
    }
}

void Image::destroy() {
    if (allocator_ != nullptr) {
        if (view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(allocator_->device(), view_, nullptr);
        }
        if (image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_->handle(), image_, allocation_);
        }
    }

    allocator_ = nullptr;
    desc_ = {};
    image_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void Image::resize(uint32_t width, uint32_t height) {
    if (allocator_ == nullptr) {
        throw std::runtime_error("Cannot resize an uninitialized image");
    }
    ImageDesc next = desc_;
    next.width = width;
    next.height = height;
    ResourceAllocator& allocator = *allocator_;
    create(allocator, next);
}

VkImageSubresourceRange Image::fullRange() const {
    VkImageSubresourceRange range{};
    range.aspectMask = desc_.aspect;
    range.baseMipLevel = 0;
    range.levelCount = desc_.mipLevels;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    return range;
}

VkDescriptorImageInfo Image::sampledDescriptor(VkSampler sampler) const {
    VkDescriptorImageInfo info{};
    info.sampler = sampler;
    info.imageView = view_;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

VkDescriptorImageInfo Image::storageDescriptor() const {
    VkDescriptorImageInfo info{};
    info.imageView = view_;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return info;
}

void Image::generateMipmaps(VkCommandBuffer commandBuffer) {
    if (desc_.mipLevels <= 1) {
        return;
    }
    if (allocator_ == nullptr) {
        throw std::runtime_error("Cannot generate mipmaps for an uninitialized image");
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(allocator_->physicalDevice(), desc_.format, &formatProperties);
    if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
        throw std::runtime_error("Image format does not support linear blit mip generation");
    }

    int32_t mipWidth = static_cast<int32_t>(desc_.width);
    int32_t mipHeight = static_cast<int32_t>(desc_.height);

    for (uint32_t mip = 1; mip < desc_.mipLevels; ++mip) {
        barrier::cmdTransitionImage(commandBuffer, {
            .image = image_,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .range = barrier::colorRange(mip - 1, 1),
            .srcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
        });

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mip;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {std::max(mipWidth / 2, 1), std::max(mipHeight / 2, 1), 1};

        vkCmdBlitImage(
            commandBuffer,
            image_,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blit,
            VK_FILTER_LINEAR);

        mipWidth = std::max(mipWidth / 2, 1);
        mipHeight = std::max(mipHeight / 2, 1);
    }

    barrier::cmdTransitionImage(commandBuffer, {
        .image = image_,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .range = barrier::colorRange(desc_.mipLevels - 1, 1),
        .srcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
    });
    layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

} // namespace rtv
