#pragma once

#include "rtv/Image.h"
#include "rtv/NativeTextureFormatPolicy.h"
#include "rtv/TextureAsset.h"

#include <Volk/volk.h>

#include <string>
#include <string_view>
#include <vector>

namespace rtv {

class BufferUploader;
class ResourceAllocator;

struct TextureData {
    int width = 0;
    int height = 0;
    int depth = 0;
    int mipLevels = 0;
    std::vector<unsigned char> pixels;
    std::vector<TextureMipLevel> mipData;
    bool isCompressed = false;
    bool linearColorSpace = false;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat compressedFormat = VK_FORMAT_UNDEFINED;
    std::string sourceContainerKind;
    std::string nativePayloadSource;
    bool sourceContainerPreserved = false;
    bool sourceContainerTranscoded = false;
};

enum class CompressedTextureKind : uint8_t {
    Unknown,
    Ktx2,
    BasisStandalone,
};

struct Ktx2ContainerInfo {
    bool valid = false;
    uint32_t vkFormat = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levelCount = 0;
    uint32_t supercompressionScheme = 0;
    bool basisUniversalSupercompressed = false;
    bool preserveNativePayload = false;
    bool requiresTranscode = false;
    std::string supercompressionName;
    std::string policy;
};

[[nodiscard]] CompressedTextureKind detectCompressedTextureKind(std::string_view path);
[[nodiscard]] Ktx2ContainerInfo inspectKtx2Container(const uint8_t* data, size_t size);
[[nodiscard]] Ktx2ContainerInfo inspectKtx2Container(std::string_view path);
[[nodiscard]] std::string_view compressedTextureKindName(CompressedTextureKind kind);

class TextureLoader {
public:
    [[nodiscard]] static TextureData loadRgba8(std::string_view path);
    [[nodiscard]] static TextureData loadRgba8(const uint8_t* data, size_t size);
    [[nodiscard]] static TextureData loadKtx2(std::string_view path);
    [[nodiscard]] static TextureData loadKtx2(const uint8_t* data, size_t size);
    [[nodiscard]] static TextureData loadKtx2(std::string_view path, const NativeTextureFormatSupport& formatSupport, NativeTextureRole role, NativeTextureColorSpace colorSpace);
    [[nodiscard]] static TextureData loadKtx2(const uint8_t* data, size_t size, const NativeTextureFormatSupport& formatSupport, NativeTextureRole role, NativeTextureColorSpace colorSpace);
    [[nodiscard]] static TextureData load(std::string_view path);
    [[nodiscard]] static TextureData load(const uint8_t* data, size_t size);
    [[nodiscard]] static TextureData load(std::string_view path, const NativeTextureFormatSupport& formatSupport, NativeTextureRole role, NativeTextureColorSpace colorSpace);
    [[nodiscard]] static TextureData load(const uint8_t* data, size_t size, const NativeTextureFormatSupport& formatSupport, NativeTextureRole role, NativeTextureColorSpace colorSpace);
    [[nodiscard]] static Image createTexture2D(
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const TextureData& texture,
        bool mipmapped,
        const char* debugName);
    [[nodiscard]] static VkFormat compressedFormatFor(VkFormat baseFormat, bool srgb);
};

} // namespace rtv
