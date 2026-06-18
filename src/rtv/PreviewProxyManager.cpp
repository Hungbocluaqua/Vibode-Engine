#include "rtv/PreviewProxyManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace rtv {

void PreviewProxyManager::registerPlacement(EntityId entityId, AssetGuid assetGuid,
                                             const PreviewProxyDesc& proxy,
                                             uint64_t frameSerial) {
    // Remove any existing record for this entity (re-registration).
    records_.erase(std::remove_if(records_.begin(), records_.end(),
        [entityId](const PreviewPlacementRecord& r) { return r.entityId == entityId; }),
        records_.end());

    PreviewPlacementRecord record;
    record.entityId = entityId;
    record.assetGuid = assetGuid;
    record.activeProxyKind = proxy.kind;
    record.state = PreviewPlacementState::MetadataShell;
    record.placedFrameSerial = frameSerial;
    record.placedFrameCount = 0;
    record.proxyFadeFactor = 1.0f;
    record.debugLabel = std::string(previewProxyKindName(proxy.kind)) + " proxy";
    records_.push_back(record);
}

void PreviewProxyManager::tick(uint64_t currentFrameSerial, float deltaSeconds) {
    for (PreviewPlacementRecord& record : records_) {
        record.placedFrameCount = static_cast<uint32_t>(
            (currentFrameSerial > record.placedFrameSerial)
                ? (currentFrameSerial - record.placedFrameSerial)
                : 0);

        // Advance fade transitions.
        if (record.state == PreviewPlacementState::ReplacementInProgress) {
            if (settings_.fadeEnabled && settings_.fadeDuration > 0.0f) {
                const float fadeStep = deltaSeconds / settings_.fadeDuration;
                record.proxyFadeFactor = std::max(0.0f, record.proxyFadeFactor - fadeStep);
            } else {
                record.proxyFadeFactor = 0.0f;
            }

            // Once fade is complete, mark as fully resident.
            if (record.proxyFadeFactor <= 0.0f) {
                record.state = PreviewPlacementState::FullyResident;
                record.activeProxyKind = PreviewProxyKind::None;
            }
        }
    }
}

void PreviewProxyManager::signalAssetProgress(AssetGuid guid, PreviewPlacementState newState) {
    for (PreviewPlacementRecord& record : records_) {
        if (record.assetGuid == guid && static_cast<uint8_t>(newState) > static_cast<uint8_t>(record.state)) {
            record.state = newState;
        }
    }
}

void PreviewProxyManager::signalReplacementReady(EntityId entityId) {
    for (PreviewPlacementRecord& record : records_) {
        if (record.entityId == entityId &&
            record.state != PreviewPlacementState::ReplacementInProgress &&
            record.state != PreviewPlacementState::FullyResident) {
            record.state = PreviewPlacementState::ReplacementInProgress;
            break;
        }
    }
}

void PreviewProxyManager::unregisterPlacement(EntityId entityId) {
    records_.erase(std::remove_if(records_.begin(), records_.end(),
        [entityId](const PreviewPlacementRecord& r) { return r.entityId == entityId; }),
        records_.end());
}

const PreviewPlacementRecord* PreviewProxyManager::find(EntityId entityId) const {
    for (const PreviewPlacementRecord& record : records_) {
        if (record.entityId == entityId) {
            return &record;
        }
    }
    return nullptr;
}

const PreviewPlacementRecord* PreviewProxyManager::findByAssetGuid(AssetGuid guid) const {
    for (const PreviewPlacementRecord& record : records_) {
        if (record.assetGuid == guid) {
            return &record;
        }
    }
    return nullptr;
}

PreviewProxyDesc PreviewProxyManager::makeBoundingBoxProxy(
    const std::array<float, 3>& boundsMin,
    const std::array<float, 3>& boundsMax) {
    PreviewProxyDesc proxy;
    proxy.kind = PreviewProxyKind::BoundingBox;
    proxy.boundsMin = boundsMin;
    proxy.boundsMax = boundsMax;
    proxy.preserveEntityOnReplacement = true;
    return proxy;
}

const char* previewProxyKindName(PreviewProxyKind kind) {
    switch (kind) {
    case PreviewProxyKind::None: return "none";
    case PreviewProxyKind::BoundingBox: return "bounding_box";
    case PreviewProxyKind::ConvexHull: return "convex_hull";
    case PreviewProxyKind::LowPolyMesh: return "low_poly_mesh";
    case PreviewProxyKind::MaterialColor: return "material_color";
    case PreviewProxyKind::Billboard: return "billboard";
    }
    return "unknown";
}

const char* previewPlacementStateName(PreviewPlacementState state) {
    switch (state) {
    case PreviewPlacementState::MetadataShell: return "metadata_shell";
    case PreviewPlacementState::ProxyVisible: return "proxy_visible";
    case PreviewPlacementState::MaterialConstantsResident: return "material_constants_resident";
    case PreviewPlacementState::LowMipsResident: return "low_mips_resident";
    case PreviewPlacementState::MeshResident: return "mesh_resident";
    case PreviewPlacementState::FullyResident: return "fully_resident";
    case PreviewPlacementState::ReplacementInProgress: return "replacement_in_progress";
    case PreviewPlacementState::Failed: return "failed";
    }
    return "unknown";
}

nlohmann::json previewPlacementRecordToJson(const PreviewPlacementRecord& record) {
    return nlohmann::json{
        {"entity_id", record.entityId.index},
        {"asset_guid", record.assetGuid},
        {"active_proxy_kind", previewProxyKindName(record.activeProxyKind)},
        {"state", previewPlacementStateName(record.state)},
        {"placed_frame_serial", record.placedFrameSerial},
        {"placed_frame_count", record.placedFrameCount},
        {"proxy_fade_factor", record.proxyFadeFactor},
        {"selected", record.selected},
        {"hovered", record.hovered},
        {"debug_label", record.debugLabel},
    };
}

} // namespace rtv
