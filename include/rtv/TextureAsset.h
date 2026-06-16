#pragma once

#include <Volk/volk.h>

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

struct TextureMipLevel {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t width = 1;
    uint32_t height = 1;
};

struct TextureAsset {
    std::string name;
    std::filesystem::path sourcePath;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t channels = 4;
    uint32_t sourceArrayLayers = 1;
    uint32_t sourceDepth = 1;
    uint32_t sourceFaceCount = 1;
    bool sourceIsCubemap = false;
    int mipLevels = 1;
    bool srgb = true;
    bool resident = false;
    bool fallback = false;
    bool isCompressed = false;
    bool linearColorSpace = false;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat compressedFormat = VK_FORMAT_UNDEFINED;
    TextureSamplerDesc sampler{};
    std::string nativeGuid;
    std::string nativeSource;
    std::filesystem::path nativePath;
    std::string sourceContainerKind;
    std::string nativePayloadSource;
    // Role authored by the source material binding (e.g. glTF material slot/extension),
    // when known. Empty means "infer from filename/format". This lets the importer
    // prefer the authored material role over filename heuristics during cooking.
    std::string authoredRole;
    bool sourceContainerPreserved = false;
    bool sourceContainerTranscoded = false;
    std::vector<uint8_t> rgba8;
    std::vector<TextureMipLevel> mipData;
};

} // namespace rtv
