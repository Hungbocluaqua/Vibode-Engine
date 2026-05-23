#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "rtv/TextureLoader.h"

#include "rtv/BufferUploader.h"

#include <ktx.h>
#include <ktxvulkan.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace rtv {

namespace {

constexpr uint8_t ktx2Magic[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
};

[[nodiscard]] bool isKtx2File(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) {
        return false;
    }
    uint8_t header[12] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    return file.gcount() == sizeof(header) && std::memcmp(header, ktx2Magic, sizeof(ktx2Magic)) == 0;
}

} // namespace

CompressedTextureKind detectCompressedTextureKind(std::string_view path) {
    if (isKtx2File(path)) {
        return CompressedTextureKind::Ktx2;
    }
    return CompressedTextureKind::Unknown;
}

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

namespace {

[[nodiscard]] uint32_t readU32(const uint8_t* ptr, size_t offset) {
    return *reinterpret_cast<const uint32_t*>(ptr + offset);
}

[[nodiscard]] uint64_t readU64(const uint8_t* ptr, size_t offset) {
    return *reinterpret_cast<const uint64_t*>(ptr + offset);
}

[[nodiscard]] bool isBcFormat(uint32_t vkFormat) {
    switch (vkFormat) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:  case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:      case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:      case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:      case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:      case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] TextureData transcodeBasisKtx2(const std::vector<uint8_t>& raw, size_t size) {
    ktxTexture2* texture = nullptr;
    KTX_error_code result = ktxTexture2_CreateFromMemory(
        raw.data(), size,
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &texture);

    if (result != KTX_SUCCESS || texture == nullptr) {
        throw std::runtime_error("KTX2: failed to parse Basis Universal texture");
    }

    ktx_transcode_fmt_e targetFormat = KTX_TTF_BC7_RGBA;
    result = ktxTexture2_TranscodeBasis(texture, targetFormat, 0);
    if (result != KTX_SUCCESS) {
        targetFormat = KTX_TTF_BC3_RGBA;
        result = ktxTexture2_TranscodeBasis(texture, targetFormat, 0);
    }
    if (result != KTX_SUCCESS) {
        targetFormat = KTX_TTF_RGBA32;
        result = ktxTexture2_TranscodeBasis(texture, targetFormat, 0);
    }
    if (result != KTX_SUCCESS) {
        ktxTexture2_Destroy(texture);
        throw std::runtime_error("KTX2: Basis Universal transcode failed");
    }

    VkFormat outFormat = ktxTexture2_GetVkFormat(texture);
    if (outFormat == VK_FORMAT_UNDEFINED && targetFormat == KTX_TTF_RGBA32) {
        outFormat = VK_FORMAT_R8G8B8A8_UNORM;
    }

    TextureData tex;
    tex.width = static_cast<int>(texture->baseWidth);
    tex.height = static_cast<int>(texture->baseHeight);
    tex.mipLevels = static_cast<int>(texture->numLevels);
    tex.isCompressed = (targetFormat != KTX_TTF_RGBA32);
    tex.compressedFormat = outFormat;

    ktx_size_t mipOffset = 0;
    ktxTexture_GetImageOffset(reinterpret_cast<ktxTexture*>(texture), 0, 0, 0, &mipOffset);
    ktx_size_t mipSize = ktxTexture_GetImageSize(reinterpret_cast<ktxTexture*>(texture), 0);

    ktx_uint8_t* imageData = ktxTexture_GetData(reinterpret_cast<ktxTexture*>(texture));
    if (imageData == nullptr) {
        ktxTexture2_Destroy(texture);
        throw std::runtime_error("KTX2: no transcoded image data");
    }

    tex.pixels.resize(mipSize);
    std::memcpy(tex.pixels.data(), imageData + mipOffset, mipSize);

    ktxTexture2_Destroy(texture);
    return tex;
}

} // namespace

TextureData TextureLoader::loadKtx2(std::string_view path) {
    std::string filepath(path);
    if (!isKtx2File(filepath)) {
        throw std::runtime_error(std::string("Not a KTX2 file: ") + filepath);
    }

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    const size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> raw(size);
    file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(size));
    if (!file) {
        throw std::runtime_error("Failed to read KTX2 file: " + filepath);
    }

    const uint32_t vkFormat      = readU32(raw.data(), 12);
    const uint32_t typeSize      = readU32(raw.data(), 16);
    const uint32_t pixelWidth    = readU32(raw.data(), 20);
    const uint32_t pixelHeight   = readU32(raw.data(), 24);
    const uint32_t pixelDepth    = readU32(raw.data(), 28);
    const uint32_t layerCount    = readU32(raw.data(), 32);
    const uint32_t faceCount     = readU32(raw.data(), 36);
    const uint32_t levelCount    = readU32(raw.data(), 40);
    const uint32_t supercompression = readU32(raw.data(), 44);

    const uint32_t dfdOffset     = readU32(raw.data(), 48);
    const uint32_t kvdOffset     = readU32(raw.data(), 56);
    const uint64_t sgdOffset     = readU64(raw.data(), 64);
    const uint64_t sgdLength     = readU64(raw.data(), 72);

    static_cast<void>(typeSize);
    static_cast<void>(pixelDepth);
    static_cast<void>(layerCount);
    static_cast<void>(faceCount);
    static_cast<void>(dfdOffset);
    static_cast<void>(kvdOffset);
    static_cast<void>(sgdOffset);
    static_cast<void>(sgdLength);

    if (supercompression != 0) {
        try {
            return transcodeBasisKtx2(raw, size);
        } catch (const std::runtime_error& e) {
            fprintf(stderr, "%s. Falling back to stb_image.\n", e.what());
            return loadRgba8(path);
        }
    }

    constexpr size_t kHeaderSize = 80;
    const size_t levelIndexOffset = kHeaderSize + (kvdOffset > 0 ? 0 : 0);

    const uint64_t mip0Offset = readU64(raw.data(), levelIndexOffset);
    const uint64_t mip0Length = readU64(raw.data(), levelIndexOffset + 8);

    if (isBcFormat(vkFormat)) {
        TextureData result;
        result.width  = static_cast<int>(pixelWidth);
        result.height = static_cast<int>(pixelHeight);
        result.depth  = static_cast<int>(pixelDepth);
        result.mipLevels = static_cast<int>(levelCount);
        result.isCompressed = true;
        result.compressedFormat = static_cast<VkFormat>(vkFormat);

        const size_t dataStart = static_cast<size_t>(mip0Offset);
        const size_t dataEnd   = dataStart + static_cast<size_t>(mip0Length);
        if (dataEnd > size) {
            throw std::runtime_error("KTX2: level data exceeds file bounds");
        }
        result.pixels.resize(mip0Length);
        std::memcpy(result.pixels.data(), raw.data() + dataStart, mip0Length);
        return result;
    }

    if (!isBcFormat(vkFormat)) {
        fprintf(stderr, "KTX2: format %u is not a direct GPU block-compressed format, falling back to stb_image\n", vkFormat);
    }

    return loadRgba8(path);
}

TextureData TextureLoader::load(std::string_view path) {
    if (isKtx2File(std::string(path))) {
        return loadKtx2(path);
    }
    return loadRgba8(path);
}

VkFormat TextureLoader::compressedFormatFor(VkFormat baseFormat, bool srgb) {
    switch (baseFormat) {
    case VK_FORMAT_BC7_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    default:
        return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
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

    const VkFormat format = texture.isCompressed ? texture.compressedFormat : VK_FORMAT_R8G8B8A8_UNORM;

    uint32_t mipLevels = texture.mipLevels > 1 ? static_cast<uint32_t>(texture.mipLevels) : 1u;
    if (mipLevels <= 1 && mipmapped) {
        const int longest = std::max(texture.width, texture.height);
        mipLevels = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(longest)))) + 1u;
    }

    Image image(allocator, {
        .width = static_cast<uint32_t>(texture.width),
        .height = static_cast<uint32_t>(texture.height),
        .mipLevels = mipLevels,
        .format = format,
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
