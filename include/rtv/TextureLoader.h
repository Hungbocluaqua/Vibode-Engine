#pragma once

#include "rtv/Image.h"

#include <string_view>
#include <vector>

namespace rtv {

class BufferUploader;
class ResourceAllocator;

struct TextureData {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

class TextureLoader {
public:
    [[nodiscard]] static TextureData loadRgba8(std::string_view path);
    [[nodiscard]] static Image createTexture2D(
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const TextureData& texture,
        bool mipmapped,
        const char* debugName);
};

} // namespace rtv
