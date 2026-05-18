#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "rtv/TextureLoader.h"

#include "rtv/BufferUploader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rtv {

TextureData TextureLoader::loadRgba8(std::string_view path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* loaded = stbi_load(std::string(path).c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (loaded == nullptr) {
        throw std::runtime_error(std::string("stbi_load failed for ") + std::string(path));
    }

    TextureData result;
    result.width = width;
    result.height = height;
    result.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::memcpy(result.pixels.data(), loaded, result.pixels.size());
    stbi_image_free(loaded);
    return result;
}

Image TextureLoader::createTexture2D(
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    const TextureData& texture,
    bool mipmapped,
    const char* debugName) {
    if (texture.width <= 0 || texture.height <= 0 || texture.pixels.empty()) {
        throw std::runtime_error("TextureData is empty");
    }

    uint32_t mipLevels = 1;
    if (mipmapped) {
        const int longest = std::max(texture.width, texture.height);
        mipLevels = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(longest)))) + 1u;
    }

    Image image(allocator, {
        .width = static_cast<uint32_t>(texture.width),
        .height = static_cast<uint32_t>(texture.height),
        .mipLevels = mipLevels,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT,
        .debugName = debugName,
    });

    uploader.uploadToImage2D(image, texture.pixels.data(), texture.pixels.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return image;
}

} // namespace rtv
