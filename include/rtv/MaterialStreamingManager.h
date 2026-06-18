#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AssetRegistry.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

// Material streaming state.
enum class MaterialStreamingState : uint8_t {
    // No data loaded.
    None,
    // Material constants loaded (small, immediate).
    ConstantsResident,
    // Texture slots have fallback descriptors bound.
    TextureSlotsFallbackBound,
    // Some texture slots have real textures (partial residency).
    TextureSlotsPartiallyResident,
    // All material slots fully resident with real textures.
    FullyResident,
    // Failed to load.
    Failed,
};

// Per-material streaming record.
struct MaterialStreamingRecord {
    AssetGuid materialGuid;
    std::string materialName;
    MaterialStreamingState state = MaterialStreamingState::None;

    // Material feature flags (needed before shader binding decisions).
    bool alphaTest = false;
    bool alphaBlend = false;
    float opacityThreshold = 0.5f;
    bool emissive = false;
    bool transmission = false;
    bool clearcoat = false;

    // Texture slot count and residency.
    uint32_t totalTextureSlots = 0;
    uint32_t residentTextureSlots = 0;
    std::vector<AssetGuid> textureSlotGuids;

    // Priority for streaming.
    float priority = 0.0f;
    bool selectedBoost = false;

    // Estimated bytes.
    uint64_t constantsBytes = 0;
    uint64_t totalTextureBytes = 0;
};

// Manages progressive material streaming.
//
// Policies:
// - Material constants load and upload immediately (very small).
// - Alpha mode and opacity threshold available before BLAS/TLAS classification.
// - Emissive metadata available before light candidate updates.
// - Material feature flags available before shader binding.
// - Texture slots stream independently with fallback descriptors.
class MaterialStreamingManager final : private NonCopyable {
public:
    MaterialStreamingManager() = default;

    // Register a material for progressive streaming.
    void registerMaterial(AssetGuid guid, const std::string& name,
                          bool alphaTest, bool alphaBlend, float opacityThreshold,
                          bool emissive, bool transmission, bool clearcoat,
                          uint64_t constantsBytes);

    // Set the texture slot GUIDs for a material.
    void setTextureSlots(AssetGuid materialGuid, const std::vector<AssetGuid>& slots);

    // Signal that material constants are now GPU-resident.
    void signalConstantsResident(AssetGuid guid);

    // Signal that a specific texture slot now has a real texture.
    void signalTextureSlotResident(AssetGuid materialGuid, AssetGuid textureGuid);

    // Mark the material as fully resident.
    void markFullyResident(AssetGuid guid);

    // Mark as failed.
    void markFailed(AssetGuid guid, const std::string& reason);

    // Priority controls.
    void boostSelected(AssetGuid guid);
    void clearSelectedBoost(AssetGuid guid);
    void setPriority(AssetGuid guid, float priority);

    // Query.
    [[nodiscard]] const MaterialStreamingRecord* find(AssetGuid guid) const;
    [[nodiscard]] bool isRenderable(AssetGuid guid) const;
    [[nodiscard]] const std::vector<MaterialStreamingRecord>& materials() const { return materials_; }

private:
    std::vector<MaterialStreamingRecord> materials_;
};

[[nodiscard]] const char* materialStreamingStateName(MaterialStreamingState state);
[[nodiscard]] nlohmann::json materialStreamingToJson(const std::vector<MaterialStreamingRecord>& materials);

} // namespace rtv
