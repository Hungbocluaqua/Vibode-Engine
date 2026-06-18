#include "rtv/TextureStreamingManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace rtv {

void TextureStreamingManager::registerTexture(AssetGuid guid, const std::string& name,
                                                uint32_t width, uint32_t height, uint32_t mipCount,
                                                uint32_t arrayLayers, uint64_t totalBytes,
                                                TextureStreamingRecord::Role role) {
    TextureStreamingRecord tex;
    tex.textureGuid = guid;
    tex.textureName = name;
    tex.width = width;
    tex.height = height;
    tex.mipCount = mipCount;
    tex.arrayLayers = arrayLayers;
    tex.totalBytes = totalBytes;
    tex.role = role;
    tex.residency = TextureMipResidency::None;
    tex.fallbackBound = true;
    textures_.push_back(std::move(tex));
}

void TextureStreamingManager::markMipResident(AssetGuid guid, uint32_t mipLevel, uint64_t mipBytes) {
    for (TextureStreamingRecord& tex : textures_) {
        if (tex.textureGuid == guid) {
            tex.residentMipsMask |= (1u << mipLevel);
            tex.residentBytes += mipBytes;

            if (mipLevel < tex.highestResidentMip || tex.highestResidentMip == UINT32_MAX) {
                tex.highestResidentMip = mipLevel;
            }
            if (mipLevel > tex.lowestResidentMip) {
                tex.lowestResidentMip = mipLevel;
            }

            // Update residency state.
            if (tex.residentMipsMask == (1u << tex.mipCount) - 1u) {
                tex.residency = TextureMipResidency::FullMipChainResident;
            } else if (tex.highestResidentMip >= tex.mipCount / 2u) {
                tex.residency = TextureMipResidency::StreamingHigherMips;
            } else if (tex.residentMipsMask != 0) {
                tex.residency = TextureMipResidency::LowestMipResident;
            }
            return;
        }
    }
}

void TextureStreamingManager::markFullyResident(AssetGuid guid) {
    for (TextureStreamingRecord& tex : textures_) {
        if (tex.textureGuid == guid) {
            tex.residency = TextureMipResidency::FullMipChainResident;
            tex.residentMipsMask = (1u << tex.mipCount) - 1u;
            tex.residentBytes = tex.totalBytes;
            tex.highestResidentMip = 0;
            tex.lowestResidentMip = tex.mipCount > 0 ? tex.mipCount - 1u : 0;
            return;
        }
    }
}

void TextureStreamingManager::updatePriority(AssetGuid guid, float screenSpaceSize) {
    for (TextureStreamingRecord& tex : textures_) {
        if (tex.textureGuid == guid) {
            tex.screenSpacePriority = screenSpaceSize;
            return;
        }
    }
}

void TextureStreamingManager::boostSelected(AssetGuid guid) {
    for (TextureStreamingRecord& tex : textures_) {
        if (tex.textureGuid == guid) {
            tex.selectedBoost = true;
            return;
        }
    }
}

void TextureStreamingManager::clearSelectedBoost(AssetGuid guid) {
    for (TextureStreamingRecord& tex : textures_) {
        if (tex.textureGuid == guid) {
            tex.selectedBoost = false;
            return;
        }
    }
}

void TextureStreamingManager::setStreamingBias(AssetGuid guid, float bias) {
    for (TextureStreamingRecord& tex : textures_) {
        if (tex.textureGuid == guid) {
            tex.streamingBias = std::clamp(bias, -1.0f, 1.0f);
            return;
        }
    }
}

std::vector<const TextureStreamingRecord*> TextureStreamingManager::prioritizedTextures() const {
    std::vector<const TextureStreamingRecord*> result;
    result.reserve(textures_.size());
    for (const TextureStreamingRecord& tex : textures_) {
        if (tex.residency < TextureMipResidency::FullMipChainResident) {
            result.push_back(&tex);
        }
    }
    std::sort(result.begin(), result.end(),
        [](const TextureStreamingRecord* a, const TextureStreamingRecord* b) {
            // Selected boost overrides everything.
            if (a->selectedBoost != b->selectedBoost) {
                return a->selectedBoost > b->selectedBoost;
            }
            // Role priority.
            const int roleA = rolePriority(a->role);
            const int roleB = rolePriority(b->role);
            if (roleA != roleB) {
                return roleA < roleB;  // Lower number = higher priority.
            }
            // Screen-space size.
            return a->screenSpacePriority > b->screenSpacePriority;
        });
    return result;
}

const TextureStreamingRecord* TextureStreamingManager::find(AssetGuid guid) const {
    for (const TextureStreamingRecord& tex : textures_) {
        if (tex.textureGuid == guid) return &tex;
    }
    return nullptr;
}

uint32_t TextureStreamingManager::nextMipToStream(const TextureStreamingRecord& record,
                                                     float minScreenSize) {
    if (record.residency >= TextureMipResidency::FullMipChainResident) {
        return UINT32_MAX;  // Nothing left to stream.
    }

    // Apply streaming bias to screen-size threshold (negative bias = sooner mip upgrade).
    const float effectiveMinSize = std::max(0.001f, minScreenSize * (1.0f + record.streamingBias));

    // Start with the lowest mip if nothing resident.
    if (record.residency == TextureMipResidency::None) {
        return record.mipCount > 0 ? record.mipCount - 1u : 0;  // lowest (simplest) mip first.
    }

    // If screen-space size is below the effective threshold, don't request a
    // higher-resolution mip yet.
    if (record.screenSpacePriority < effectiveMinSize) {
        return UINT32_MAX;
    }

    // Stream next higher mip (lower index), scanning from highest resident toward mip 0.
    for (uint32_t mip = record.highestResidentMip; mip > 0; --mip) {
        if ((record.residentMipsMask & (1u << mip)) == 0 &&
            mip < record.highestResidentMip) {
            // Only request the next-lower-index (higher-res) mip.
            return mip - 1u;
        }
    }

    return UINT32_MAX;
}

int TextureStreamingManager::rolePriority(TextureStreamingRecord::Role role) {
    switch (role) {
    case TextureStreamingRecord::Role::BaseColor: return 0;
    case TextureStreamingRecord::Role::Normal: return 1;
    case TextureStreamingRecord::Role::Roughness: return 2;
    case TextureStreamingRecord::Role::Opacity: return 3;
    case TextureStreamingRecord::Role::Metallic: return 4;
    case TextureStreamingRecord::Role::Emissive: return 5;
    case TextureStreamingRecord::Role::Occlusion: return 6;
    case TextureStreamingRecord::Role::Displacement: return 7;
    case TextureStreamingRecord::Role::Unknown: return 8;
    }
    return 8;
}

const char* textureMipResidencyName(TextureMipResidency residency) {
    switch (residency) {
    case TextureMipResidency::None: return "none";
    case TextureMipResidency::LowestMipResident: return "lowest_mip_resident";
    case TextureMipResidency::StreamingHigherMips: return "streaming_higher_mips";
    case TextureMipResidency::FullMipChainResident: return "full_mip_chain_resident";
    }
    return "unknown";
}

const char* textureRoleName(TextureStreamingRecord::Role role) {
    switch (role) {
    case TextureStreamingRecord::Role::BaseColor: return "base_color";
    case TextureStreamingRecord::Role::Normal: return "normal";
    case TextureStreamingRecord::Role::Roughness: return "roughness";
    case TextureStreamingRecord::Role::Metallic: return "metallic";
    case TextureStreamingRecord::Role::Emissive: return "emissive";
    case TextureStreamingRecord::Role::Opacity: return "opacity";
    case TextureStreamingRecord::Role::Occlusion: return "occlusion";
    case TextureStreamingRecord::Role::Displacement: return "displacement";
    case TextureStreamingRecord::Role::Unknown: return "unknown";
    }
    return "unknown";
}

nlohmann::json textureStreamingToJson(const std::vector<TextureStreamingRecord>& textures) {
    nlohmann::json result = nlohmann::json::array();
    for (const TextureStreamingRecord& tex : textures) {
        result.push_back({
            {"texture_guid", tex.textureGuid},
            {"texture_name", tex.textureName},
            {"residency", textureMipResidencyName(tex.residency)},
            {"width", tex.width},
            {"height", tex.height},
            {"mip_count", tex.mipCount},
            {"resident_mips_mask", tex.residentMipsMask},
            {"highest_resident_mip", tex.highestResidentMip},
            {"lowest_resident_mip", tex.lowestResidentMip},
            {"total_bytes", tex.totalBytes},
            {"resident_bytes", tex.residentBytes},
            {"role", textureRoleName(tex.role)},
            {"screen_space_priority", tex.screenSpacePriority},
            {"selected_boost", tex.selectedBoost},
            {"fallback_bound", tex.fallbackBound},
            {"streaming_bias", tex.streamingBias},
        });
    }
    return result;
}

} // namespace rtv
