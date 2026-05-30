#pragma once

#include "rtv/Image.h"
#include "rtv/TextureAsset.h"

#include <Volk/volk.h>

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
};

enum class CompressedTextureKind : uint8_t {
    Unknown,
    Ktx2,
};

[[nodiscard]] CompressedTextureKind detectCompressedTextureKind(std::string_view path);

class TextureLoader {
public:
    [[nodiscard]] static TextureData loadRgba8(std::string_view path);
    [[nodiscard]] static TextureData loadKtx2(std::string_view path);
    [[nodiscard]] static TextureData load(std::string_view path);
    [[nodiscard]] static Image createTexture2D(
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const TextureData& texture,
        bool mipmapped,
        const char* debugName);
    [[nodiscard]] static VkFormat compressedFormatFor(VkFormat baseFormat, bool srgb);
};

} // namespace rtv
