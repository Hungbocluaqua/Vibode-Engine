#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "rtv/TextureLoader.h"

#include "rtv/BufferUploader.h"

#include <ktx.h>
#include <ktxvulkan.h>
#include <tinyexr.h>
#include <tiffio.h>

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

// OpenEXR files start with the magic number 0x76 0x2f 0x31 0x01.
constexpr uint8_t exrMagic[4] = {0x76, 0x2f, 0x31, 0x01};

[[nodiscard]] bool isExrBuffer(const uint8_t* data, size_t size) {
    return data != nullptr && size >= sizeof(exrMagic) && std::memcmp(data, exrMagic, sizeof(exrMagic)) == 0;
}

[[nodiscard]] bool isExrFile(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) {
        return false;
    }
    uint8_t header[4] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    return file.gcount() == sizeof(header) && std::memcmp(header, exrMagic, sizeof(exrMagic)) == 0;
}

[[nodiscard]] bool isTiffHeader(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 4u) {
        return false;
    }
    const bool littleTiff = data[0] == 'I' && data[1] == 'I' && data[2] == 42u && data[3] == 0u;
    const bool bigTiff = data[0] == 'M' && data[1] == 'M' && data[2] == 0u && data[3] == 42u;
    const bool littleBigTiff = data[0] == 'I' && data[1] == 'I' && data[2] == 43u && data[3] == 0u;
    const bool bigBigTiff = data[0] == 'M' && data[1] == 'M' && data[2] == 0u && data[3] == 43u;
    return littleTiff || bigTiff || littleBigTiff || bigBigTiff;
}

[[nodiscard]] bool isTiffBuffer(const uint8_t* data, size_t size) {
    return isTiffHeader(data, size);
}

[[nodiscard]] bool isTiffFile(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) {
        return false;
    }
    uint8_t header[4] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    return file.gcount() == sizeof(header) && isTiffHeader(header, sizeof(header));
}

// Decode an OpenEXR image (file or memory) into a 32-bit float RGBA TextureData.
// tinyexr returns tightly packed RGBA float scanlines in top-down order, which
// matches the orientation stb_image produces, so no vertical flip is applied here.
[[nodiscard]] TextureData decodeExrToFloatRgba(const float* rgba, int width, int height) {
    TextureData result;
    result.width = width;
    result.height = height;
    result.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    result.linearColorSpace = true;
    const size_t byteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * sizeof(float);
    result.pixels.resize(byteSize);
    std::memcpy(result.pixels.data(), rgba, byteSize);
    return result;
}

[[nodiscard]] TextureData loadExrFromFile(const std::string& filepath) {
    float* rgba = nullptr;
    int width = 0;
    int height = 0;
    const char* err = nullptr;
    const int rc = LoadEXR(&rgba, &width, &height, filepath.c_str(), &err);
    if (rc != TINYEXR_SUCCESS) {
        std::string message = std::string("LoadEXR failed for ") + filepath;
        if (err != nullptr) {
            message += std::string(": ") + err;
            FreeEXRErrorMessage(err);
        }
        throw std::runtime_error(message);
    }
    TextureData result = decodeExrToFloatRgba(rgba, width, height);
    free(rgba);
    return result;
}

[[nodiscard]] TextureData loadExrFromMemory(const uint8_t* data, size_t size) {
    float* rgba = nullptr;
    int width = 0;
    int height = 0;
    const char* err = nullptr;
    const int rc = LoadEXRFromMemory(&rgba, &width, &height, data, size, &err);
    if (rc != TINYEXR_SUCCESS) {
        std::string message = "LoadEXRFromMemory failed";
        if (err != nullptr) {
            message += std::string(": ") + err;
            FreeEXRErrorMessage(err);
        }
        throw std::runtime_error(message);
    }
    TextureData result = decodeExrToFloatRgba(rgba, width, height);
    free(rgba);
    return result;
}

[[nodiscard]] TextureData decodeTiffToRgba8(TIFF* tiff, std::string_view sourceLabel) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) != 1 || TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) != 1) {
        throw std::runtime_error(std::string("TIFF missing image dimensions for ") + std::string(sourceLabel));
    }
    if (width == 0u || height == 0u) {
        throw std::runtime_error(std::string("TIFF has empty image dimensions for ") + std::string(sourceLabel));
    }
    if (width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("TIFF dimensions exceed TextureData limits for ") + std::string(sourceLabel));
    }
    if (width > std::numeric_limits<size_t>::max() / height) {
        throw std::runtime_error(std::string("TIFF dimensions overflow for ") + std::string(sourceLabel));
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixelCount > std::numeric_limits<size_t>::max() / 4u) {
        throw std::runtime_error(std::string("TIFF pixel buffer overflow for ") + std::string(sourceLabel));
    }

    std::vector<uint32_t> raster(pixelCount);
    if (TIFFReadRGBAImageOriented(tiff, width, height, raster.data(), ORIENTATION_TOPLEFT, 0) != 1) {
        throw std::runtime_error(std::string("TIFFReadRGBAImageOriented failed for ") + std::string(sourceLabel));
    }

    TextureData result;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.format = VK_FORMAT_R8G8B8A8_UNORM;
    result.pixels.resize(pixelCount * 4u);
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t pixel = raster[i];
        result.pixels[i * 4u + 0u] = static_cast<unsigned char>(TIFFGetR(pixel));
        result.pixels[i * 4u + 1u] = static_cast<unsigned char>(TIFFGetG(pixel));
        result.pixels[i * 4u + 2u] = static_cast<unsigned char>(TIFFGetB(pixel));
        result.pixels[i * 4u + 3u] = static_cast<unsigned char>(TIFFGetA(pixel));
    }
    return result;
}

[[nodiscard]] TextureData loadTiffFromFile(const std::string& filepath) {
    TIFF* tiff = TIFFOpen(filepath.c_str(), "r");
    if (tiff == nullptr) {
        throw std::runtime_error(std::string("TIFFOpen failed for ") + filepath);
    }
    try {
        TextureData result = decodeTiffToRgba8(tiff, filepath);
        TIFFClose(tiff);
        return result;
    } catch (...) {
        TIFFClose(tiff);
        throw;
    }
}

struct TiffMemoryStream {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t offset = 0;
};

tmsize_t tiffMemoryRead(thandle_t handle, void* buffer, tmsize_t byteCount) {
    if (handle == nullptr || buffer == nullptr || byteCount <= 0) {
        return 0;
    }
    auto* stream = static_cast<TiffMemoryStream*>(handle);
    if (stream->offset >= stream->size) {
        return 0;
    }
    const size_t available = stream->size - stream->offset;
    const size_t requested = static_cast<size_t>(byteCount);
    const size_t copied = std::min(available, requested);
    std::memcpy(buffer, stream->data + stream->offset, copied);
    stream->offset += copied;
    return static_cast<tmsize_t>(copied);
}

tmsize_t tiffMemoryWrite(thandle_t, void*, tmsize_t) {
    return 0;
}

toff_t tiffMemorySeek(thandle_t handle, toff_t offset, int whence) {
    if (handle == nullptr) {
        return static_cast<toff_t>(-1);
    }
    auto* stream = static_cast<TiffMemoryStream*>(handle);
    size_t base = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = stream->offset;
        break;
    case SEEK_END:
        base = stream->size;
        break;
    default:
        return static_cast<toff_t>(-1);
    }
    if (offset > static_cast<toff_t>(std::numeric_limits<size_t>::max())) {
        return static_cast<toff_t>(-1);
    }
    const size_t relative = static_cast<size_t>(offset);
    if (base > std::numeric_limits<size_t>::max() - relative) {
        return static_cast<toff_t>(-1);
    }
    const size_t next = base + relative;
    if (next > stream->size) {
        return static_cast<toff_t>(-1);
    }
    stream->offset = next;
    return static_cast<toff_t>(stream->offset);
}

int tiffMemoryClose(thandle_t) {
    return 0;
}

toff_t tiffMemorySize(thandle_t handle) {
    if (handle == nullptr) {
        return 0;
    }
    const auto* stream = static_cast<const TiffMemoryStream*>(handle);
    return static_cast<toff_t>(stream->size);
}

int tiffMemoryMap(thandle_t handle, void** base, toff_t* size) {
    if (handle == nullptr || base == nullptr || size == nullptr) {
        return 0;
    }
    const auto* stream = static_cast<const TiffMemoryStream*>(handle);
    *base = const_cast<uint8_t*>(stream->data);
    *size = static_cast<toff_t>(stream->size);
    return 1;
}

void tiffMemoryUnmap(thandle_t, void*, toff_t) {
}

[[nodiscard]] TextureData loadTiffFromMemory(const uint8_t* data, size_t size) {
    if (size > static_cast<size_t>(std::numeric_limits<toff_t>::max())) {
        throw std::runtime_error("TIFF memory load failed: byte buffer is too large");
    }
    TiffMemoryStream stream{data, size, 0u};
    TIFF* tiff = TIFFClientOpen(
        "memory.tiff",
        "r",
        static_cast<thandle_t>(&stream),
        tiffMemoryRead,
        tiffMemoryWrite,
        tiffMemorySeek,
        tiffMemoryClose,
        tiffMemorySize,
        tiffMemoryMap,
        tiffMemoryUnmap);
    if (tiff == nullptr) {
        throw std::runtime_error("TIFFClientOpen failed for memory buffer");
    }
    try {
        TextureData result = decodeTiffToRgba8(tiff, "memory buffer");
        TIFFClose(tiff);
        return result;
    } catch (...) {
        TIFFClose(tiff);
        throw;
    }
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

    if (isExrFile(filepath)) {
        return loadExrFromFile(filepath);
    }

    if (isTiffFile(filepath)) {
        return loadTiffFromFile(filepath);
    }

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

    if (isExrBuffer(data, size)) {
        return loadExrFromMemory(data, size);
    }

    if (isTiffBuffer(data, size)) {
        return loadTiffFromMemory(data, size);
    }

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

[[nodiscard]] uint32_t popcount32(uint32_t value) {
    uint32_t count = 0;
    while (value != 0u) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

[[nodiscard]] uint32_t legacyDdsCubemapFaceCount(uint32_t caps2) {
    constexpr uint32_t ddsCaps2CubemapPositiveX = 0x00000400u;
    constexpr uint32_t ddsCaps2CubemapNegativeX = 0x00000800u;
    constexpr uint32_t ddsCaps2CubemapPositiveY = 0x00001000u;
    constexpr uint32_t ddsCaps2CubemapNegativeY = 0x00002000u;
    constexpr uint32_t ddsCaps2CubemapPositiveZ = 0x00004000u;
    constexpr uint32_t ddsCaps2CubemapNegativeZ = 0x00008000u;
    const uint32_t faceMask = caps2 & (
        ddsCaps2CubemapPositiveX | ddsCaps2CubemapNegativeX |
        ddsCaps2CubemapPositiveY | ddsCaps2CubemapNegativeY |
        ddsCaps2CubemapPositiveZ | ddsCaps2CubemapNegativeZ);
    const uint32_t faceCount = popcount32(faceMask);
    return faceCount > 0u ? faceCount : 6u;
}

[[nodiscard]] uint32_t trailingZeroCount32(uint32_t value) {
    if (value == 0u) {
        return 32u;
    }
    uint32_t count = 0;
    while ((value & 1u) == 0u) {
        value >>= 1u;
        ++count;
    }
    return count;
}

[[nodiscard]] uint8_t unpackMaskedChannel(uint32_t pixel, uint32_t mask, uint8_t fallback) {
    if (mask == 0u) {
        return fallback;
    }
    const uint32_t shift = trailingZeroCount32(mask);
    const uint32_t bits = popcount32(mask);
    if (bits == 0u) {
        return fallback;
    }
    const uint32_t value = (pixel & mask) >> shift;
    const uint32_t maxValue = (1u << bits) - 1u;
    return static_cast<uint8_t>((value * 255u + maxValue / 2u) / maxValue);
}

struct DdsUncompressedLayout {
    enum class Kind {
        MaskedUnorm,
        Rgba16Float,
        Rgba32Float,
    };

    Kind kind = Kind::MaskedUnorm;
    uint32_t bitsPerPixel = 0u;
    uint32_t rMask = 0u;
    uint32_t gMask = 0u;
    uint32_t bMask = 0u;
    uint32_t aMask = 0u;
};

void appendUncompressedDdsMip(
    TextureData& tex,
    const std::vector<uint8_t>& raw,
    size_t srcOffset,
    uint32_t width,
    uint32_t height,
    uint32_t bitsPerPixel,
    uint32_t rMask,
    uint32_t gMask,
    uint32_t bMask,
    uint32_t aMask) {
    if (width == 0u || height == 0u || bitsPerPixel == 0u || (bitsPerPixel % 8u) != 0u || bitsPerPixel > 32u) {
        throw std::runtime_error("DDS: unsupported uncompressed pixel layout");
    }
    const uint32_t bytesPerPixel = bitsPerPixel / 8u;
    const uint64_t srcBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * bytesPerPixel;
    if (srcOffset > raw.size() || srcBytes > raw.size() - srcOffset) {
        throw std::runtime_error("DDS: uncompressed mip payload exceeds file bounds");
    }

    const uint64_t dstOffset = static_cast<uint64_t>(tex.pixels.size());
    const uint64_t dstBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
    tex.pixels.resize(tex.pixels.size() + static_cast<size_t>(dstBytes));
    uint8_t* dst = tex.pixels.data() + dstOffset;
    const uint8_t* src = raw.data() + srcOffset;
    for (uint64_t pixelIndex = 0; pixelIndex < static_cast<uint64_t>(width) * static_cast<uint64_t>(height); ++pixelIndex) {
        uint32_t pixel = 0;
        const uint8_t* pixelBytes = src + pixelIndex * bytesPerPixel;
        for (uint32_t byte = 0; byte < bytesPerPixel; ++byte) {
            pixel |= static_cast<uint32_t>(pixelBytes[byte]) << (8u * byte);
        }
        dst[pixelIndex * 4ull + 0ull] = unpackMaskedChannel(pixel, rMask, 0u);
        dst[pixelIndex * 4ull + 1ull] = unpackMaskedChannel(pixel, gMask, 0u);
        dst[pixelIndex * 4ull + 2ull] = unpackMaskedChannel(pixel, bMask, 0u);
        dst[pixelIndex * 4ull + 3ull] = unpackMaskedChannel(pixel, aMask, 255u);
    }
    tex.mipData.push_back(TextureMipLevel{
        .offset = dstOffset,
        .size = dstBytes,
        .width = std::max(width, 1u),
        .height = std::max(height, 1u),
    });
}

[[nodiscard]] float halfToFloat(uint16_t value) {
    const uint32_t sign = (static_cast<uint32_t>(value & 0x8000u)) << 16u;
    const uint32_t exponent = (value >> 10u) & 0x1fu;
    uint32_t mantissa = value & 0x03ffu;
    uint32_t bits = sign;
    if (exponent == 0u) {
        if (mantissa != 0u) {
            int32_t normalizedExponent = -14;
            while ((mantissa & 0x0400u) == 0u) {
                mantissa <<= 1u;
                --normalizedExponent;
            }
            mantissa &= 0x03ffu;
            bits |= (static_cast<uint32_t>(normalizedExponent + 127) << 23u) | (mantissa << 13u);
        }
    } else if (exponent == 31u) {
        bits |= 0x7f800000u | (mantissa << 13u);
    } else {
        bits |= ((exponent + 112u) << 23u) | (mantissa << 13u);
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

void appendFloatDdsMip(
    TextureData& tex,
    const std::vector<uint8_t>& raw,
    size_t srcOffset,
    uint32_t width,
    uint32_t height,
    DdsUncompressedLayout::Kind kind) {
    const uint32_t bytesPerPixel = kind == DdsUncompressedLayout::Kind::Rgba16Float ? 8u : 16u;
    const uint64_t srcBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * bytesPerPixel;
    if (srcOffset > raw.size() || srcBytes > raw.size() - srcOffset) {
        throw std::runtime_error("DDS: float mip payload exceeds file bounds");
    }

    const uint64_t dstOffset = static_cast<uint64_t>(tex.pixels.size());
    const uint64_t dstBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull * sizeof(float);
    tex.pixels.resize(tex.pixels.size() + static_cast<size_t>(dstBytes));
    auto* dst = reinterpret_cast<float*>(tex.pixels.data() + dstOffset);
    const uint8_t* src = raw.data() + srcOffset;
    for (uint64_t pixelIndex = 0; pixelIndex < static_cast<uint64_t>(width) * static_cast<uint64_t>(height); ++pixelIndex) {
        if (kind == DdsUncompressedLayout::Kind::Rgba16Float) {
            const uint8_t* pixel = src + pixelIndex * 8ull;
            for (uint32_t channel = 0; channel < 4u; ++channel) {
                const uint16_t half = static_cast<uint16_t>(pixel[channel * 2u]) |
                                      (static_cast<uint16_t>(pixel[channel * 2u + 1u]) << 8u);
                dst[pixelIndex * 4ull + channel] = halfToFloat(half);
            }
        } else {
            std::memcpy(dst + pixelIndex * 4ull, src + pixelIndex * 16ull, 4u * sizeof(float));
        }
    }
    tex.mipData.push_back(TextureMipLevel{
        .offset = dstOffset,
        .size = dstBytes,
        .width = std::max(width, 1u),
        .height = std::max(height, 1u),
    });
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
    case VK_FORMAT_BC2_UNORM_BLOCK:      case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:      case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:      case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:      case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
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
    const uint32_t headerDepth = std::max(readU32(raw.data(), 24), 1u);
    const uint32_t mipCount = std::max(readU32(raw.data(), 28), 1u);
    const uint32_t pfFlags = readU32(raw.data(), 80);
    const uint32_t ddsFourCc = readU32(raw.data(), 84);
    const uint32_t caps2 = readU32(raw.data(), 112);

    uint32_t dataOffset = 128u;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t blockBytes = 0;
    DdsUncompressedLayout uncompressed{};
    bool hasUncompressedLayout = false;
    uint32_t sourceDepth = ((caps2 & 0x00200000u) != 0u) ? headerDepth : 1u;
    bool sourceIsCubemap = (caps2 & 0x00000200u) != 0u;
    uint32_t sourceFaceCount = sourceIsCubemap ? legacyDdsCubemapFaceCount(caps2) : 1u;
    uint32_t sourceArrayLayers = sourceFaceCount;
    if ((pfFlags & 0x4u) != 0u) {
        switch (ddsFourCc) {
        case fourCc('D', 'X', 'T', '1'):
            format = colorSpace == NativeTextureColorSpace::Srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            blockBytes = 8u;
            break;
        case fourCc('D', 'X', 'T', '3'):
            format = colorSpace == NativeTextureColorSpace::Srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
            blockBytes = 16u;
            break;
        case fourCc('D', 'X', 'T', '5'):
            format = colorSpace == NativeTextureColorSpace::Srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
            blockBytes = 16u;
            break;
        case fourCc('A', 'T', 'I', '1'):
        case fourCc('B', 'C', '4', 'U'):
            format = VK_FORMAT_BC4_UNORM_BLOCK;
            blockBytes = 8u;
            break;
        case fourCc('B', 'C', '4', 'S'):
            format = VK_FORMAT_BC4_SNORM_BLOCK;
            blockBytes = 8u;
            break;
        case fourCc('A', 'T', 'I', '2'):
        case fourCc('B', 'C', '5', 'U'):
            format = VK_FORMAT_BC5_UNORM_BLOCK;
            blockBytes = 16u;
            break;
        case fourCc('B', 'C', '5', 'S'):
            format = VK_FORMAT_BC5_SNORM_BLOCK;
            blockBytes = 16u;
            break;
        case fourCc('D', 'X', '1', '0'): {
            if (raw.size() < 148u) {
                throw std::runtime_error("DDS: truncated DX10 header");
            }
            const uint32_t dxgiFormat = readU32(raw.data(), 128);
            const uint32_t resourceDimension = readU32(raw.data(), 132);
            const uint32_t miscFlag = readU32(raw.data(), 136);
            const uint32_t dx10ArraySize = std::max(readU32(raw.data(), 140), 1u);
            dataOffset = 148u;
            sourceIsCubemap = (miscFlag & 0x4u) != 0u;
            sourceFaceCount = sourceIsCubemap ? 6u : 1u;
            sourceArrayLayers = dx10ArraySize * sourceFaceCount;
            sourceDepth = resourceDimension == 4u ? headerDepth : 1u;
            switch (dxgiFormat) {
            case 2u:
                format = VK_FORMAT_R32G32B32A32_SFLOAT;
                uncompressed = DdsUncompressedLayout{DdsUncompressedLayout::Kind::Rgba32Float, 128u, 0u, 0u, 0u, 0u};
                hasUncompressedLayout = true;
                break;
            case 10u:
                format = VK_FORMAT_R32G32B32A32_SFLOAT;
                uncompressed = DdsUncompressedLayout{DdsUncompressedLayout::Kind::Rgba16Float, 64u, 0u, 0u, 0u, 0u};
                hasUncompressedLayout = true;
                break;
            case 28u:
                format = VK_FORMAT_R8G8B8A8_UNORM;
                uncompressed = DdsUncompressedLayout{DdsUncompressedLayout::Kind::MaskedUnorm, 32u, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u};
                hasUncompressedLayout = true;
                break;
            case 29u:
                format = VK_FORMAT_R8G8B8A8_SRGB;
                uncompressed = DdsUncompressedLayout{DdsUncompressedLayout::Kind::MaskedUnorm, 32u, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u};
                hasUncompressedLayout = true;
                break;
            case 49u:
                format = colorSpace == NativeTextureColorSpace::Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                uncompressed = DdsUncompressedLayout{DdsUncompressedLayout::Kind::MaskedUnorm, 16u, 0x000000ffu, 0x0000ff00u, 0u, 0u};
                hasUncompressedLayout = true;
                break;
            case 61u:
                format = colorSpace == NativeTextureColorSpace::Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                uncompressed = DdsUncompressedLayout{DdsUncompressedLayout::Kind::MaskedUnorm, 8u, 0x000000ffu, 0u, 0u, 0u};
                hasUncompressedLayout = true;
                break;
            case 87u:
                format = VK_FORMAT_R8G8B8A8_UNORM;
                uncompressed = DdsUncompressedLayout{DdsUncompressedLayout::Kind::MaskedUnorm, 32u, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u};
                hasUncompressedLayout = true;
                break;
            case 91u:
                format = VK_FORMAT_R8G8B8A8_SRGB;
                uncompressed = DdsUncompressedLayout{DdsUncompressedLayout::Kind::MaskedUnorm, 32u, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u};
                hasUncompressedLayout = true;
                break;
            case 71u: format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; blockBytes = 8u; break;
            case 72u: format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; blockBytes = 8u; break;
            case 74u: format = VK_FORMAT_BC2_UNORM_BLOCK; blockBytes = 16u; break;
            case 75u: format = VK_FORMAT_BC2_SRGB_BLOCK; blockBytes = 16u; break;
            case 77u: format = VK_FORMAT_BC3_UNORM_BLOCK; blockBytes = 16u; break;
            case 78u: format = VK_FORMAT_BC3_SRGB_BLOCK; blockBytes = 16u; break;
            case 80u: format = VK_FORMAT_BC4_UNORM_BLOCK; blockBytes = 8u; break;
            case 81u: format = VK_FORMAT_BC4_SNORM_BLOCK; blockBytes = 8u; break;
            case 83u: format = VK_FORMAT_BC5_UNORM_BLOCK; blockBytes = 16u; break;
            case 84u: format = VK_FORMAT_BC5_SNORM_BLOCK; blockBytes = 16u; break;
            case 95u: format = VK_FORMAT_BC6H_UFLOAT_BLOCK; blockBytes = 16u; break;
            case 96u: format = VK_FORMAT_BC6H_SFLOAT_BLOCK; blockBytes = 16u; break;
            case 98u: format = VK_FORMAT_BC7_UNORM_BLOCK; blockBytes = 16u; break;
            case 99u: format = VK_FORMAT_BC7_SRGB_BLOCK; blockBytes = 16u; break;
            default: break;
            }
            break;
        }
        default:
            break;
        }
        if (format == VK_FORMAT_UNDEFINED || (blockBytes == 0u && !hasUncompressedLayout)) {
            throw std::runtime_error("DDS: unsupported FourCC block-compressed format");
        }
    } else if ((pfFlags & 0x40u) != 0u) {
        format = colorSpace == NativeTextureColorSpace::Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        uncompressed = DdsUncompressedLayout{
            DdsUncompressedLayout::Kind::MaskedUnorm,
            readU32(raw.data(), 88),
            readU32(raw.data(), 92),
            readU32(raw.data(), 96),
            readU32(raw.data(), 100),
            readU32(raw.data(), 104),
        };
        hasUncompressedLayout = true;
    } else {
        throw std::runtime_error("DDS: unsupported pixel format flags");
    }

    TextureData result;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.depth = static_cast<int>(sourceDepth);
    result.mipLevels = static_cast<int>(mipCount);
    result.sourceArrayLayers = sourceArrayLayers;
    result.sourceDepth = sourceDepth;
    result.sourceFaceCount = sourceFaceCount;
    result.sourceIsCubemap = sourceIsCubemap;
    result.isCompressed = blockBytes != 0u;
    result.format = format;
    result.compressedFormat = blockBytes != 0u ? format : VK_FORMAT_UNDEFINED;
    result.linearColorSpace = format == VK_FORMAT_R32G32B32A32_SFLOAT;
    result.sourceContainerKind = "dds";
    result.nativePayloadSource = blockBytes != 0u ? "dds-preserved-native-payload" :
        (result.linearColorSpace ? "dds-float-decoded-rgba32f" : "dds-uncompressed-decoded-rgba8");
    result.sourceContainerPreserved = blockBytes != 0u;
    result.sourceContainerTranscoded = blockBytes == 0u;

    const uint64_t subresourceCount = static_cast<uint64_t>(std::max(sourceArrayLayers, 1u));
    size_t srcOffset = dataOffset;
    for (uint64_t subresource = 0; subresource < subresourceCount; ++subresource) {
        const bool uploadableFirst2DSubresource = subresource == 0u;
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            const uint32_t mipWidth = mipExtent(width, mip);
            const uint32_t mipHeight = mipExtent(height, mip);
            const uint32_t mipDepth = mipExtent(sourceDepth, mip);
            if (blockBytes != 0u) {
                const uint32_t firstSliceBytes = blockCompressedMipBytes(mipWidth, mipHeight, blockBytes);
                const uint64_t mipBytes = static_cast<uint64_t>(firstSliceBytes) * static_cast<uint64_t>(std::max(mipDepth, 1u));
                if (srcOffset > raw.size() || mipBytes > raw.size() - srcOffset) {
                    throw std::runtime_error("DDS: mip payload exceeds file bounds");
                }
                if (uploadableFirst2DSubresource) {
                    appendTextureMip(result, raw.data(), srcOffset, firstSliceBytes, mipWidth, mipHeight, raw.size());
                }
                srcOffset += mipBytes;
            } else {
                const size_t sliceBytes = static_cast<size_t>(mipWidth) * static_cast<size_t>(mipHeight) * static_cast<size_t>(uncompressed.bitsPerPixel / 8u);
                const size_t mipBytes = sliceBytes * static_cast<size_t>(std::max(mipDepth, 1u));
                if (srcOffset > raw.size() || mipBytes > raw.size() - srcOffset) {
                    throw std::runtime_error("DDS: mip payload exceeds file bounds");
                }
                if (uploadableFirst2DSubresource) {
                    if (uncompressed.kind == DdsUncompressedLayout::Kind::MaskedUnorm) {
                        appendUncompressedDdsMip(
                            result,
                            raw,
                            srcOffset,
                            mipWidth,
                            mipHeight,
                            uncompressed.bitsPerPixel,
                            uncompressed.rMask,
                            uncompressed.gMask,
                            uncompressed.bMask,
                            uncompressed.aMask);
                    } else {
                        appendFloatDdsMip(result, raw, srcOffset, mipWidth, mipHeight, uncompressed.kind);
                    }
                }
                srcOffset += mipBytes;
            }
        }
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
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK:
        return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return baseFormat;
    case VK_FORMAT_BC7_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        return baseFormat;
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
