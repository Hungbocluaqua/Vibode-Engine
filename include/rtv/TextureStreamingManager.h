#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AssetRegistry.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

// Mip residency state for a streamed texture.
enum class TextureMipResidency : uint8_t {
    // No mips resident — fallback descriptor bound.
    None,
    // Lowest useful mip resident (typically mip 4-6 for a 4K texture).
    LowestMipResident,
    // Additional low mips streaming.
    StreamingHigherMips,
    // Full mip chain resident.
    FullMipChainResident,
};

// Per-texture streaming record.
struct TextureStreamingRecord {
    AssetGuid textureGuid;
    std::string textureName;
    TextureMipResidency residency = TextureMipResidency::None;

    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t mipCount = 1;
    uint32_t arrayLayers = 1;
    uint64_t totalBytes = 0;
    uint64_t residentBytes = 0;

    // Which mip levels are GPU-resident (bitmask).
    uint32_t residentMipsMask = 0;

    // Highest mip level currently resident (lower numbers = higher resolution).
    uint32_t highestResidentMip = UINT32_MAX;

    // Lowest mip level currently resident (higher numbers = lower resolution).
    uint32_t lowestResidentMip = 0;

    // Role for priority decisions.
    enum class Role : uint8_t {
        Unknown,
        BaseColor,
        Normal,
        Roughness,
        Metallic,
        Emissive,
        Opacity,
        Occlusion,
        Displacement,
    } role = Role::Unknown;

    // Screen-space priority for mip streaming.
    float screenSpacePriority = 0.0f;

    // Whether this texture is bound to a selected/pinned asset.
    bool selectedBoost = false;

    // Fallback descriptor already bound.
    bool fallbackBound = true;

    // Streaming bias control (-1.0 to +1.0, 0 = default).
    float streamingBias = 0.0f;
};

// Manages progressive texture mip streaming.
//
// Policies:
// - Always bind fallback descriptors first.
// - Stream lowest useful mip first, then higher mips by screen-space size.
// - Support partial resident mip chains.
// - Selected-object textures get priority boost.
// - Texture role affects priority (BaseColor > Normal > Roughness, etc.).
class TextureStreamingManager final : private NonCopyable {
public:
    TextureStreamingManager() = default;

    // Register a texture for mip streaming.
    void registerTexture(AssetGuid guid, const std::string& name,
                         uint32_t width, uint32_t height, uint32_t mipCount,
                         uint32_t arrayLayers, uint64_t totalBytes,
                         TextureStreamingRecord::Role role);

    // Signal that a mip level is now GPU-resident.
    void markMipResident(AssetGuid guid, uint32_t mipLevel, uint64_t mipBytes);

    // Mark the full mip chain resident.
    void markFullyResident(AssetGuid guid);

    // Update screen-space priority based on camera and object transforms.
    void updatePriority(AssetGuid guid, float screenSpaceSize);

    // Boost priority for a selected texture.
    void boostSelected(AssetGuid guid);
    void clearSelectedBoost(AssetGuid guid);

    // Set per-texture streaming bias.
    void setStreamingBias(AssetGuid guid, float bias);

    // Get textures sorted by streaming priority (highest first).
    [[nodiscard]] std::vector<const TextureStreamingRecord*> prioritizedTextures() const;

    // Query.
    [[nodiscard]] const TextureStreamingRecord* find(AssetGuid guid) const;
    [[nodiscard]] const std::vector<TextureStreamingRecord>& textures() const { return textures_; }

    // Determine which mip level to stream next for a texture.
    [[nodiscard]] static uint32_t nextMipToStream(const TextureStreamingRecord& record,
                                                    float minScreenSize = 0.01f);

private:
    static int rolePriority(TextureStreamingRecord::Role role);

    std::vector<TextureStreamingRecord> textures_;
};

[[nodiscard]] const char* textureMipResidencyName(TextureMipResidency residency);
[[nodiscard]] const char* textureRoleName(TextureStreamingRecord::Role role);
[[nodiscard]] nlohmann::json textureStreamingToJson(const std::vector<TextureStreamingRecord>& textures);

} // namespace rtv
