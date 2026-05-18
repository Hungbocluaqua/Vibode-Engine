#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

struct TextureAssetHandle {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

enum class TextureFilter : uint32_t {
    Nearest,
    Linear,
};

enum class TextureWrap : uint32_t {
    Repeat,
    ClampToEdge,
    MirroredRepeat,
};

struct TextureSamplerDesc {
    TextureFilter minFilter = TextureFilter::Linear;
    TextureFilter magFilter = TextureFilter::Linear;
    TextureWrap wrapS = TextureWrap::Repeat;
    TextureWrap wrapT = TextureWrap::Repeat;
};

struct TextureAsset {
    std::string name;
    std::filesystem::path sourcePath;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t channels = 4;
    bool srgb = true;
    bool resident = false;
    bool fallback = false;
    TextureSamplerDesc sampler{};
    std::vector<uint8_t> rgba8;
};

} // namespace rtv
