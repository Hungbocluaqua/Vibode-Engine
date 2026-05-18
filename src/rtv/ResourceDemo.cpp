#include "rtv/ResourceDemo.h"

#include "rtv/BufferUploader.h"
#include "rtv/ImageBarrier.h"
#include "rtv/TextureLoader.h"

#include <array>
#include <cstdint>
#include <span>

namespace rtv {

namespace {

TextureData makeCheckerTexture() {
    constexpr int size = 128;
    TextureData texture;
    texture.width = size;
    texture.height = size;
    texture.pixels.resize(size * size * 4);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool checker = ((x / 16) + (y / 16)) % 2 == 0;
            const size_t i = static_cast<size_t>(y * size + x) * 4;
            texture.pixels[i + 0] = checker ? 235 : 28;
            texture.pixels[i + 1] = checker ? 180 : 65;
            texture.pixels[i + 2] = checker ? 72 : 130;
            texture.pixels[i + 3] = 255;
        }
    }
    return texture;
}

} // namespace

ResourceDemo::ResourceDemo(ResourceAllocator& allocator, BufferUploader& uploader)
    : uploadedTexture_(TextureLoader::createTexture2D(allocator, uploader, makeCheckerTexture(), false, "demo uploaded texture")),
      storageImage_(allocator, {
          .width = 256,
          .height = 256,
          .format = VK_FORMAT_R8G8B8A8_UNORM,
          .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          .debugName = "demo storage image",
      }),
      storageBuffer_(allocator, {
          .size = 1024,
          .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          .memory = BufferMemory::GpuOnly,
          .debugName = "demo storage buffer",
      }) {
    const std::array<uint32_t, 8> values = {1, 2, 3, 4, 5, 6, 7, 8};
    uploader.uploadToBuffer(storageBuffer_, std::span<const uint32_t>(values.data(), values.size()));
}

void ResourceDemo::record(VkCommandBuffer commandBuffer, VkImage swapchainImage, VkExtent2D swapchainExtent) {
    if (storageImage_.layout() != VK_IMAGE_LAYOUT_GENERAL) {
        barrier::cmdTransitionImage(commandBuffer, {
            .image = storageImage_.handle(),
            .oldLayout = storageImage_.layout(),
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .range = storageImage_.fullRange(),
            .srcStage = VK_PIPELINE_STAGE_2_NONE,
            .srcAccess = VK_ACCESS_2_NONE,
            .dstStage = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        });
        storageImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    }

    VkClearColorValue storageClear{};
    storageClear.float32[0] = 0.05f;
    storageClear.float32[1] = 0.10f;
    storageClear.float32[2] = 0.18f;
    storageClear.float32[3] = 1.0f;
    const VkImageSubresourceRange storageRange = storageImage_.fullRange();
    vkCmdClearColorImage(commandBuffer, storageImage_.handle(), VK_IMAGE_LAYOUT_GENERAL, &storageClear, 1, &storageRange);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = uploadedTexture_.handle(),
        .oldLayout = uploadedTexture_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .range = uploadedTexture_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_BLIT_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
    });
    uploadedTexture_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    const int32_t width = static_cast<int32_t>(swapchainExtent.width);
    const int32_t height = static_cast<int32_t>(swapchainExtent.height);
    const int32_t side = width < height ? width / 3 : height / 3;
    const int32_t x0 = (width - side) / 2;
    const int32_t y0 = (height - side) / 2;

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {static_cast<int32_t>(uploadedTexture_.width()), static_cast<int32_t>(uploadedTexture_.height()), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {x0, y0, 0};
    blit.dstOffsets[1] = {x0 + side, y0 + side, 1};

    vkCmdBlitImage(
        commandBuffer,
        uploadedTexture_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &blit,
        VK_FILTER_NEAREST);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = uploadedTexture_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = uploadedTexture_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_BLIT_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    uploadedTexture_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace rtv
