#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "rtv/TextureLoader.h"

#include "rtv/BufferUploader.h"

#include <ktx.h>
#include <ktxvulkan.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace rtv {

namespace {

constexpr uint8_t ktx2Magic[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
};

constexpr uint32_t fourCc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
        (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8u) |
        (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16u) |
        (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

[[nodiscard]] bool isKtx2File(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) {
        return false;
    }
    uint8_t header[12] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    return file.gcount() == sizeof(header) && std::memcmp(header, ktx2Magic, sizeof(ktx2Magic)) == 0;
}

[[nodiscard]] bool isDdsFile(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) {
        return false;
    }
    uint8_t header[4] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    return file.gcount() == sizeof(header) && std::memcmp(header, "DDS ", sizeof(header)) == 0;
}

[[nodiscard]] bool isKtx2Buffer(const uint8_t* data, size_t size) {
    return data != nullptr && size >= sizeof(ktx2Magic) && std::memcmp(data, ktx2Magic, sizeof(ktx2Magic)) == 0;
}

[[nodiscard]] bool isDdsBuffer(const uint8_t* data, size_t size) {
    return data != nullptr && size >= 4u && std::memcmp(data, "DDS ", 4u) == 0;
}

[[nodiscard]] std::string lowerAscii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

} // namespace

CompressedTextureKind detectCompressedTextureKind(std::string_view path) {
    const std::string lower = lowerAscii(path);
    if (endsWith(lower, ".basis")) {
        return CompressedTextureKind::BasisStandalone;
    }
    if (isKtx2File(path)) {
        return CompressedTextureKind::Ktx2;
    }
    return CompressedTextureKind::Unknown;
}

std::string_view compressedTextureKindName(CompressedTextureKind kind) {
    switch (kind) {
    case CompressedTextureKind::Ktx2: return "ktx2";
    case CompressedTextureKind::BasisStandalone: return "basis-standalone";
    case CompressedTextureKind::Unknown:
    default: return "unknown";
    }
}

TextureData TextureLoader::loadRgba8(std::string_view path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    const std::string filepath(path);

    if (stbi_is_hdr(filepath.c_str())) {
        float* loaded = stbi_loadf(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (loaded == nullptr) {
            throw std::runtime_error(std::string("stbi_loadf failed for ") + filepath);
        }

        TextureData result;
        result.width = width;
        result.height = height;
        result.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        result.linearColorSpace = true;
        const size_t byteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * sizeof(float);
        result.pixels.resize(byteSize);
        std::memcpy(result.pixels.data(), loaded, byteSize);
        stbi_image_free(loaded);
        return result;
    }

    if (stbi_is_16_bit(filepath.c_str())) {
        stbi_us* loaded = stbi_load_16(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (loaded == nullptr) {
            throw std::runtime_error(std::string("stbi_load_16 failed for ") + filepath);
        }

        TextureData result;
        result.width = width;
        result.height = height;
        result.format = VK_FORMAT_R16G16B16A16_UNORM;
        const size_t byteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * sizeof(stbi_us);
        result.pixels.resize(byteSize);
        std::memcpy(result.pixels.data(), loaded, byteSize);
        stbi_image_free(loaded);
        return result;
    }

    unsigned char* loaded = stbi_load(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (loaded == nullptr) {
        throw std::runtime_error(std::string("stbi_load failed for ") + filepath);
    }

    TextureData result;
    result.width = width;
    result.height = height;
    result.format = VK_FORMAT_R8G8B8A8_UNORM;
    result.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::memcpy(result.pixels.data(), loaded, result.pixels.size());
    stbi_image_free(loaded);
    return result;
}

TextureData TextureLoader::loadRgba8(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        throw std::runtime_error("stb_image memory load failed: empty byte buffer");
    }
    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("stb_image memory load failed: byte buffer is too large");
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    const int byteCount = static_cast<int>(size);

    if (stbi_is_hdr_from_memory(data, byteCount)) {
        float* loaded = stbi_loadf_from_memory(data, byteCount, &width, &height, &channels, STBI_rgb_alpha);
        if (loaded == nullptr) {
            throw std::runtime_error("stbi_loadf_from_memory failed");
        }

        TextureData result;
        result.width = width;
        result.height = height;
        result.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        result.linearColorSpace = true;
        const size_t byteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * sizeof(float);
        result.pixels.resize(byteSize);
        std::memcpy(result.pixels.data(), loaded, byteSize);
        stbi_image_free(loaded);
        return result;
    }

    if (stbi_is_16_bit_from_memory(data, byteCount)) {
        stbi_us* loaded = stbi_load_16_from_memory(data, byteCount, &width, &height, &channels, STBI_rgb_alpha);
        if (loaded == nullptr) {
            throw std::runtime_error("stbi_load_16_from_memory failed");
        }

        TextureData result;
        result.width = width;
        result.height = height;
        result.format = VK_FORMAT_R16G16B16A16_UNORM;
        const size_t byteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * sizeof(stbi_us);
        result.pixels.resize(byteSize);
        std::memcpy(result.pixels.data(), loaded, byteSize);
        stbi_image_free(loaded);
        return result;
    }

    unsigned char* loaded = stbi_load_from_memory(data, byteCount, &width, &height, &channels, STBI_rgb_alpha);
    if (loaded == nullptr) {
        throw std::runtime_error("stbi_load_from_memory failed");
    }

    TextureData result;
    result.width = width;
    result.height = height;
    result.format = VK_FORMAT_R8G8B8A8_UNORM;
    result.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    std::memcpy(result.pixels.data(), loaded, result.pixels.size());
    stbi_image_free(loaded);
    return result;
}

namespace {

[[nodiscard]] uint32_t readU32(const uint8_t* ptr, size_t offset) {
    return *reinterpret_cast<const uint32_t*>(ptr + offset);
}

[[nodiscard]] uint32_t blockCompressedMipBytes(uint32_t width, uint32_t height, uint32_t blockBytes) {
    const uint32_t blocksWide = std::max(1u, (width + 3u) / 4u);
    const uint32_t blocksHigh = std::max(1u, (height + 3u) / 4u);
    return blocksWide * blocksHigh * blockBytes;
}

[[nodiscard]] uint64_t readU64(const uint8_t* ptr, size_t offset) {
    return *reinterpret_cast<const uint64_t*>(ptr + offset);
}

[[nodiscard]] std::string ktx2SupercompressionName(uint32_t scheme) {
    switch (scheme) {
    case 0: return "none";
    case 1: return "basis-lz";
    case 2: return "zstd";
    case 3: return "zlib";
    default: return "scheme-" + std::to_string(scheme);
    }
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

[[nodiscard]] uint32_t mipExtent(uint32_t base, uint32_t level) {
    return std::max(base >> level, 1u);
}

void appendTextureMip(TextureData& tex, const uint8_t* src, uint64_t srcOffset, uint64_t byteLength, uint32_t width, uint32_t height, size_t fileSize) {
    if (byteLength == 0) {
        return;
    }
    if (srcOffset > fileSize || byteLength > fileSize - srcOffset) {
        throw std::runtime_error("KTX2: level data exceeds file bounds");
    }
    const uint64_t dstOffset = static_cast<uint64_t>(tex.pixels.size());
    tex.pixels.resize(tex.pixels.size() + static_cast<size_t>(byteLength));
    std::memcpy(tex.pixels.data() + dstOffset, src + srcOffset, static_cast<size_t>(byteLength));
    tex.mipData.push_back(TextureMipLevel{
        .offset = dstOffset,
        .size = byteLength,
        .width = std::max(width, 1u),
        .height = std::max(height, 1u),
    });
}

[[nodiscard]] ktx_transcode_fmt_e ktxTargetForNativeSelection(const NativeTextureFormatSelection& selection) {
    switch (selection.selectedFormat) {
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
        return KTX_TTF_BC7_RGBA;
    case VK_FORMAT_BC5_UNORM_BLOCK:
        return KTX_TTF_BC5_RG;
    case VK_FORMAT_BC4_UNORM_BLOCK:
        return KTX_TTF_BC4_R;
    default:
        return KTX_TTF_RGBA32;
    }
}

[[nodiscard]] bool ktx2NeedsBasisTranscode(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }
    ktxTexture2* texture = nullptr;
    const KTX_error_code result = ktxTexture2_CreateFromMemory(
        data,
        size,
        KTX_TEXTURE_CREATE_NO_FLAGS,
        &texture);
    if (result != KTX_SUCCESS || texture == nullptr) {
        return false;
    }
    const bool needsTranscode = ktxTexture2_NeedsTranscoding(texture) == KTX_TRUE;
    ktxTexture2_Destroy(texture);
    return needsTranscode;
}

[[nodiscard]] TextureData transcodeBasisKtx2(const std::vector<uint8_t>& raw, size_t size, ktx_transcode_fmt_e preferredTargetFormat, bool allowLegacyBc3Fallback) {
    ktxTexture2* texture = nullptr;
    KTX_error_code result = ktxTexture2_CreateFromMemory(
        raw.data(), size,
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &texture);

    if (result != KTX_SUCCESS || texture == nullptr) {
        throw std::runtime_error("KTX2: failed to parse Basis Universal texture");
    }

    ktx_transcode_fmt_e targetFormat = preferredTargetFormat;
    result = ktxTexture2_TranscodeBasis(texture, targetFormat, 0);
    if (result != KTX_SUCCESS && allowLegacyBc3Fallback) {
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
    tex.format = outFormat;
    tex.compressedFormat = outFormat;
    tex.sourceContainerKind = "ktx2";
    tex.nativePayloadSource = "ktx2-basisu-transcode-output";
    tex.sourceContainerTranscoded = true;

    ktx_uint8_t* imageData = ktxTexture_GetData(reinterpret_cast<ktxTexture*>(texture));
    if (imageData == nullptr) {
        ktxTexture2_Destroy(texture);
        throw std::runtime_error("KTX2: no transcoded image data");
    }

    const uint32_t levelCount = std::max<uint32_t>(texture->numLevels, 1u);
    tex.mipLevels = static_cast<int>(levelCount);
    for (uint32_t level = 0; level < levelCount; ++level) {
        ktx_size_t mipOffset = 0;
        KTX_error_code offsetResult = ktxTexture_GetImageOffset(reinterpret_cast<ktxTexture*>(texture), level, 0, 0, &mipOffset);
        if (offsetResult != KTX_SUCCESS) {
            ktxTexture2_Destroy(texture);
            throw std::runtime_error("KTX2: failed to locate transcoded mip level");
        }
        const ktx_size_t mipSize = ktxTexture_GetImageSize(reinterpret_cast<ktxTexture*>(texture), level);
        appendTextureMip(
            tex,
            imageData,
            static_cast<uint64_t>(mipOffset),
            static_cast<uint64_t>(mipSize),
            mipExtent(texture->baseWidth, level),
            mipExtent(texture->baseHeight, level),
            static_cast<size_t>(ktxTexture_GetDataSize(reinterpret_cast<ktxTexture*>(texture))));
    }

    ktxTexture2_Destroy(texture);
    return tex;
}

[[nodiscard]] TextureData transcodeBasisKtx2(const std::vector<uint8_t>& raw, size_t size) {
    return transcodeBasisKtx2(raw, size, KTX_TTF_BC7_RGBA, true);
}

} // namespace

[[nodiscard]] TextureData parseDdsData(const std::vector<uint8_t>& raw, NativeTextureColorSpace colorSpace) {
    if (raw.size() < 128u || std::memcmp(raw.data(), "DDS ", 4u) != 0) {
        throw std::runtime_error("Not a DDS byte buffer");
    }
    if (readU32(raw.data(), 4) != 124u || readU32(raw.data(), 76) != 32u) {
        throw std::runtime_error("DDS: unsupported header layout");
    }

    const uint32_t height = readU32(raw.data(), 12);
    const uint32_t width = readU32(raw.data(), 16);
    const uint32_t mipCount = std::max(readU32(raw.data(), 28), 1u);
    const uint32_t pfFlags = readU32(raw.data(), 80);
    const uint32_t ddsFourCc = readU32(raw.data(), 84);
    if ((pfFlags & 0x4u) == 0u) {
        throw std::runtime_error("DDS: only FourCC block-compressed payloads are supported");
    }

    uint32_t dataOffset = 128u;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t blockBytes = 0;
    switch (ddsFourCc) {
    case fourCc('D', 'X', 'T', '1'):
        format = colorSpace == NativeTextureColorSpace::Srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        blockBytes = 8u;
        break;
    case fourCc('D', 'X', 'T', '5'):
        format = colorSpace == NativeTextureColorSpace::Srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        blockBytes = 16u;
        break;
    case fourCc('A', 'T', 'I', '2'):
    case fourCc('B', 'C', '5', 'U'):
        format = VK_FORMAT_BC5_UNORM_BLOCK;
        blockBytes = 16u;
        break;
    case fourCc('D', 'X', '1', '0'): {
        if (raw.size() < 148u) {
            throw std::runtime_error("DDS: truncated DX10 header");
        }
        const uint32_t dxgiFormat = readU32(raw.data(), 128);
        dataOffset = 148u;
        switch (dxgiFormat) {
        case 71u: format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; blockBytes = 8u; break;
        case 72u: format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; blockBytes = 8u; break;
        case 77u: format = VK_FORMAT_BC3_UNORM_BLOCK; blockBytes = 16u; break;
        case 78u: format = VK_FORMAT_BC3_SRGB_BLOCK; blockBytes = 16u; break;
        case 83u: format = VK_FORMAT_BC5_UNORM_BLOCK; blockBytes = 16u; break;
        case 98u: format = VK_FORMAT_BC7_UNORM_BLOCK; blockBytes = 16u; break;
        case 99u: format = VK_FORMAT_BC7_SRGB_BLOCK; blockBytes = 16u; break;
        default: break;
        }
        break;
    }
    default:
        break;
    }
    if (format == VK_FORMAT_UNDEFINED || blockBytes == 0u) {
        throw std::runtime_error("DDS: unsupported FourCC block-compressed format");
    }

    TextureData result;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.mipLevels = static_cast<int>(mipCount);
    result.isCompressed = true;
    result.format = format;
    result.compressedFormat = format;
    result.sourceContainerKind = "dds";
    result.nativePayloadSource = "dds-preserved-native-payload";
    result.sourceContainerPreserved = true;

    size_t srcOffset = dataOffset;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint32_t mipWidth = mipExtent(width, mip);
        const uint32_t mipHeight = mipExtent(height, mip);
        const uint32_t mipBytes = blockCompressedMipBytes(mipWidth, mipHeight, blockBytes);
        if (srcOffset > raw.size() || mipBytes > raw.size() - srcOffset) {
            throw std::runtime_error("DDS: mip payload exceeds file bounds");
        }
        appendTextureMip(result, raw.data(), srcOffset, mipBytes, mipWidth, mipHeight, raw.size());
        srcOffset += mipBytes;
    }
    result.mipLevels = std::max<int>(1, static_cast<int>(result.mipData.size()));
    return result;
}

Ktx2ContainerInfo inspectKtx2Container(const uint8_t* data, size_t size) {
    Ktx2ContainerInfo info;
    if (data == nullptr || size < 80 || std::memcmp(data, ktx2Magic, sizeof(ktx2Magic)) != 0) {
        return info;
    }
    info.valid = true;
    info.vkFormat = readU32(data, 12);
    info.width = readU32(data, 20);
    info.height = readU32(data, 24);
    info.levelCount = std::max(readU32(data, 40), 1u);
    info.supercompressionScheme = readU32(data, 44);
    const bool basisTranscodable = ktx2NeedsBasisTranscode(data, size);
    info.basisUniversalSupercompressed = info.supercompressionScheme == 1u || basisTranscodable;
    info.requiresTranscode = info.supercompressionScheme != 0u || info.vkFormat == VK_FORMAT_UNDEFINED;
    info.preserveNativePayload = info.supercompressionScheme == 0u && info.vkFormat != VK_FORMAT_UNDEFINED;
    info.supercompressionName = ktx2SupercompressionName(info.supercompressionScheme);
    if (info.preserveNativePayload) {
        info.policy = "preserve-native-ktx2-payload";
    } else if (info.basisUniversalSupercompressed) {
        info.policy = "transcode-basisu-supercompressed-ktx2";
    } else if (info.supercompressionScheme != 0u) {
        info.policy = "transcode-supercompressed-ktx2";
    } else {
        info.policy = "transcode-undefined-format-ktx2";
    }
    return info;
}

Ktx2ContainerInfo inspectKtx2Container(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> raw(size);
    file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(size));
    if (!file) {
        return {};
    }
    return inspectKtx2Container(raw.data(), raw.size());
}

[[nodiscard]] TextureData parseKtx2Data(const std::vector<uint8_t>& raw, std::string_view fallbackPath, ktx_transcode_fmt_e preferredTargetFormat = KTX_TTF_BC7_RGBA, bool allowLegacyBc3Fallback = true) {
    const size_t size = raw.size();
    if (size < 80 || std::memcmp(raw.data(), ktx2Magic, sizeof(ktx2Magic)) != 0) {
        throw std::runtime_error("Not a KTX2 byte buffer");
    }

    const uint32_t vkFormat      = readU32(raw.data(), 12);
    const uint32_t typeSize      = readU32(raw.data(), 16);
    const uint32_t pixelWidth    = readU32(raw.data(), 20);
    const uint32_t pixelHeight   = readU32(raw.data(), 24);
    const uint32_t pixelDepth    = readU32(raw.data(), 28);
    const uint32_t layerCount    = readU32(raw.data(), 32);
    const uint32_t faceCount     = readU32(raw.data(), 36);
    const uint32_t levelCountRaw = readU32(raw.data(), 40);
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

    const bool basisTranscodable = ktx2NeedsBasisTranscode(raw.data(), size);
    if (supercompression != 0 || basisTranscodable) {
        try {
            return transcodeBasisKtx2(raw, size, preferredTargetFormat, allowLegacyBc3Fallback);
        } catch (const std::runtime_error& e) {
            if (!fallbackPath.empty()) {
                fprintf(stderr, "%s. Falling back to stb_image.\n", e.what());
                return TextureLoader::loadRgba8(fallbackPath);
            }
            throw;
        }
    }

    constexpr size_t kHeaderSize = 80;
    constexpr size_t kLevelIndexEntrySize = 24;
    const uint32_t levelCount = std::max(levelCountRaw, 1u);
    if (kHeaderSize + static_cast<size_t>(levelCount) * kLevelIndexEntrySize > size) {
        throw std::runtime_error("KTX2: level index exceeds file bounds");
    }

    if (vkFormat != VK_FORMAT_UNDEFINED) {
        TextureData result;
        result.width  = static_cast<int>(pixelWidth);
        result.height = static_cast<int>(pixelHeight);
        result.depth  = static_cast<int>(pixelDepth);
        result.mipLevels = static_cast<int>(levelCount);
        result.isCompressed = isBcFormat(vkFormat);
        result.format = static_cast<VkFormat>(vkFormat);
        result.compressedFormat = result.isCompressed ? static_cast<VkFormat>(vkFormat) : VK_FORMAT_UNDEFINED;
        result.sourceContainerKind = "ktx2";
        result.nativePayloadSource = "ktx2-preserved-native-payload";
        result.sourceContainerPreserved = true;
        result.linearColorSpace = vkFormat == VK_FORMAT_R16G16B16A16_SFLOAT ||
                                  vkFormat == VK_FORMAT_R32G32B32A32_SFLOAT ||
                                  vkFormat == VK_FORMAT_R16G16B16_SFLOAT ||
                                  vkFormat == VK_FORMAT_R32G32B32_SFLOAT;

        for (uint32_t level = 0; level < levelCount; ++level) {
            const size_t entryOffset = kHeaderSize + static_cast<size_t>(level) * kLevelIndexEntrySize;
            const uint64_t mipOffset = readU64(raw.data(), entryOffset);
            const uint64_t mipLength = readU64(raw.data(), entryOffset + 8);
            appendTextureMip(
                result,
                raw.data(),
                mipOffset,
                mipLength,
                mipExtent(pixelWidth, level),
                mipExtent(pixelHeight, level),
                size);
        }
        result.mipLevels = std::max<int>(1, static_cast<int>(result.mipData.size()));
        return result;
    }

    if (!fallbackPath.empty()) {
        fprintf(stderr, "KTX2: format is undefined, falling back to stb_image\n");
        return TextureLoader::loadRgba8(fallbackPath);
    }
    throw std::runtime_error("KTX2: format is undefined");
}

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

    return parseKtx2Data(raw, path);
}

TextureData TextureLoader::loadKtx2(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        throw std::runtime_error("KTX2: empty byte buffer");
    }
    std::vector<uint8_t> raw(data, data + size);
    return parseKtx2Data(raw, {});
}

TextureData TextureLoader::loadKtx2(std::string_view path, const NativeTextureFormatSupport& formatSupport, NativeTextureRole role, NativeTextureColorSpace colorSpace) {
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

    const NativeTextureFormatSelection selection = selectNativeTextureFormat(role, colorSpace, formatSupport);
    return parseKtx2Data(raw, path, ktxTargetForNativeSelection(selection), false);
}

TextureData TextureLoader::loadKtx2(const uint8_t* data, size_t size, const NativeTextureFormatSupport& formatSupport, NativeTextureRole role, NativeTextureColorSpace colorSpace) {
    if (data == nullptr || size == 0) {
        throw std::runtime_error("KTX2: empty byte buffer");
    }
    std::vector<uint8_t> raw(data, data + size);
    const NativeTextureFormatSelection selection = selectNativeTextureFormat(role, colorSpace, formatSupport);
    return parseKtx2Data(raw, {}, ktxTargetForNativeSelection(selection), false);
}

TextureData loadDdsFile(std::string_view path, NativeTextureColorSpace colorSpace) {
    std::string filepath(path);
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open DDS file: " + filepath);
    }
    const size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> raw(size);
    file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(size));
    if (!file) {
        throw std::runtime_error("Failed to read DDS file: " + filepath);
    }
    return parseDdsData(raw, colorSpace);
}

TextureData loadDdsMemory(const uint8_t* data, size_t size, NativeTextureColorSpace colorSpace) {
    if (data == nullptr || size == 0) {
        throw std::runtime_error("DDS: empty byte buffer");
    }
    std::vector<uint8_t> raw(data, data + size);
    return parseDdsData(raw, colorSpace);
}

TextureData TextureLoader::load(std::string_view path) {
    if (isKtx2File(std::string(path))) {
        return loadKtx2(path);
    }
    if (isDdsFile(path)) {
        return loadDdsFile(path, NativeTextureColorSpace::SourceDefined);
    }
    return loadRgba8(path);
}

TextureData TextureLoader::load(const uint8_t* data, size_t size) {
    if (isKtx2Buffer(data, size)) {
        return loadKtx2(data, size);
    }
    if (isDdsBuffer(data, size)) {
        return loadDdsMemory(data, size, NativeTextureColorSpace::SourceDefined);
    }
    return loadRgba8(data, size);
}

TextureData TextureLoader::load(std::string_view path, const NativeTextureFormatSupport& formatSupport, NativeTextureRole role, NativeTextureColorSpace colorSpace) {
    if (isKtx2File(std::string(path))) {
        return loadKtx2(path, formatSupport, role, colorSpace);
    }
    if (isDdsFile(path)) {
        return loadDdsFile(path, colorSpace);
    }
    return loadRgba8(path);
}

TextureData TextureLoader::load(const uint8_t* data, size_t size, const NativeTextureFormatSupport& formatSupport, NativeTextureRole role, NativeTextureColorSpace colorSpace) {
    if (isKtx2Buffer(data, size)) {
        return loadKtx2(data, size, formatSupport, role, colorSpace);
    }
    if (isDdsBuffer(data, size)) {
        return loadDdsMemory(data, size, colorSpace);
    }
    return loadRgba8(data, size);
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

    const VkFormat format = texture.isCompressed ? texture.compressedFormat : texture.format;

    uint32_t mipLevels = texture.mipLevels > 1 ? static_cast<uint32_t>(texture.mipLevels) : 1u;
    if (!texture.mipData.empty()) {
        mipLevels = static_cast<uint32_t>(texture.mipData.size());
    }
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

    uploader.uploadToImage2D(image, texture.pixels.data(), texture.pixels.size(), texture.mipData, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return image;
}

} // namespace rtv
