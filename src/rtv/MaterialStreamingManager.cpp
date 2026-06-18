#include "rtv/MaterialStreamingManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace rtv {

void MaterialStreamingManager::registerMaterial(AssetGuid guid, const std::string& name,
                                                  bool alphaTest, bool alphaBlend, float opacityThreshold,
                                                  bool emissive, bool transmission, bool clearcoat,
                                                  uint64_t constantsBytes) {
    MaterialStreamingRecord mat;
    mat.materialGuid = guid;
    mat.materialName = name;
    mat.alphaTest = alphaTest;
    mat.alphaBlend = alphaBlend;
    mat.opacityThreshold = opacityThreshold;
    mat.emissive = emissive;
    mat.transmission = transmission;
    mat.clearcoat = clearcoat;
    mat.constantsBytes = constantsBytes;
    mat.state = MaterialStreamingState::None;
    materials_.push_back(std::move(mat));
}

void MaterialStreamingManager::setTextureSlots(AssetGuid materialGuid,
                                                 const std::vector<AssetGuid>& slots) {
    for (MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == materialGuid) {
            mat.textureSlotGuids = slots;
            mat.totalTextureSlots = static_cast<uint32_t>(slots.size());
            return;
        }
    }
}

void MaterialStreamingManager::signalConstantsResident(AssetGuid guid) {
    for (MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == guid) {
            if (mat.state < MaterialStreamingState::TextureSlotsFallbackBound) {
                mat.state = MaterialStreamingState::TextureSlotsFallbackBound;
            }
            return;
        }
    }
}

void MaterialStreamingManager::signalTextureSlotResident(AssetGuid materialGuid, AssetGuid textureGuid) {
    for (MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == materialGuid) {
            mat.residentTextureSlots++;
            // Track which specific textures are resident for diagnostics.
            mat.textureSlotGuids.push_back(textureGuid);
            if (mat.residentTextureSlots >= mat.totalTextureSlots && mat.totalTextureSlots > 0) {
                mat.state = MaterialStreamingState::FullyResident;
            } else if (mat.residentTextureSlots > 0) {
                mat.state = MaterialStreamingState::TextureSlotsPartiallyResident;
            }
            return;
        }
    }
}

void MaterialStreamingManager::markFullyResident(AssetGuid guid) {
    for (MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == guid) {
            mat.state = MaterialStreamingState::FullyResident;
            mat.residentTextureSlots = mat.totalTextureSlots;
            return;
        }
    }
}

void MaterialStreamingManager::markFailed(AssetGuid guid, const std::string& reason) {
    for (MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == guid) {
            mat.state = MaterialStreamingState::Failed;
            mat.materialName = reason;  // Store reason in material name for debugging.
        }
    }
}

void MaterialStreamingManager::boostSelected(AssetGuid guid) {
    for (MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == guid) {
            mat.selectedBoost = true;
            mat.priority = std::max(mat.priority, 100.0f);
            return;
        }
    }
}

void MaterialStreamingManager::clearSelectedBoost(AssetGuid guid) {
    for (MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == guid) {
            mat.selectedBoost = false;
            return;
        }
    }
}

void MaterialStreamingManager::setPriority(AssetGuid guid, float priority) {
    for (MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == guid) {
            mat.priority = priority;
            return;
        }
    }
}

const MaterialStreamingRecord* MaterialStreamingManager::find(AssetGuid guid) const {
    for (const MaterialStreamingRecord& mat : materials_) {
        if (mat.materialGuid == guid) return &mat;
    }
    return nullptr;
}

bool MaterialStreamingManager::isRenderable(AssetGuid guid) const {
    const MaterialStreamingRecord* mat = find(guid);
    return mat != nullptr &&
           mat->state >= MaterialStreamingState::ConstantsResident &&
           mat->state != MaterialStreamingState::Failed;
}

const char* materialStreamingStateName(MaterialStreamingState state) {
    switch (state) {
    case MaterialStreamingState::None: return "none";
    case MaterialStreamingState::ConstantsResident: return "constants_resident";
    case MaterialStreamingState::TextureSlotsFallbackBound: return "texture_slots_fallback_bound";
    case MaterialStreamingState::TextureSlotsPartiallyResident: return "texture_slots_partially_resident";
    case MaterialStreamingState::FullyResident: return "fully_resident";
    case MaterialStreamingState::Failed: return "failed";
    }
    return "unknown";
}

nlohmann::json materialStreamingToJson(const std::vector<MaterialStreamingRecord>& materials) {
    nlohmann::json result = nlohmann::json::array();
    for (const MaterialStreamingRecord& mat : materials) {
        nlohmann::json slots = nlohmann::json::array();
        for (const AssetGuid& slot : mat.textureSlotGuids) {
            slots.push_back(slot);
        }
        result.push_back({
            {"material_guid", mat.materialGuid},
            {"material_name", mat.materialName},
            {"state", materialStreamingStateName(mat.state)},
            {"alpha_test", mat.alphaTest},
            {"alpha_blend", mat.alphaBlend},
            {"opacity_threshold", mat.opacityThreshold},
            {"emissive", mat.emissive},
            {"transmission", mat.transmission},
            {"clearcoat", mat.clearcoat},
            {"total_texture_slots", mat.totalTextureSlots},
            {"resident_texture_slots", mat.residentTextureSlots},
            {"texture_slot_guids", slots},
            {"priority", mat.priority},
            {"selected_boost", mat.selectedBoost},
            {"renderable", mat.state >= MaterialStreamingState::ConstantsResident && mat.state != MaterialStreamingState::Failed},
        });
    }
    return result;
}

} // namespace rtv
