#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AssetRegistry.h"
#include "rtv/EntityId.h"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

// Preview proxy types for progressive placement.
// Higher values = later in the progression chain, replacing lower values
// when real payloads become available.
enum class PreviewProxyKind : uint8_t {
    // No proxy; invisible until real geometry is resident.
    None = 0,
    // Axis-aligned bounding box shown as wireframe.
    BoundingBox = 1,
    // Convex hull derived from metadata bounds.
    ConvexHull = 2,
    // Low-poly mesh (decimated or simplified version).
    LowPolyMesh = 3,
    // Material-color-only solid fill for quick visual identification.
    MaterialColor = 4,
    // Billboard/impostor card facing camera (for vegetation/distant objects).
    Billboard = 5,
};

// Descriptor for a preview proxy generated from metadata before real payloads load.
struct PreviewProxyDesc {
    PreviewProxyKind kind = PreviewProxyKind::BoundingBox;

    // Bounding box from native metadata.
    std::array<float, 3> boundsMin = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> boundsMax = {1.0f, 1.0f, 1.0f};

    // Optional simplified mesh vertices (when kind == LowPolyMesh).
    std::vector<float> proxyVertices;  // interleaved xyz, n vertices * 3
    std::vector<uint32_t> proxyIndices;

    // Material tint color for MaterialColor proxy.
    std::array<float, 4> materialColor = {0.7f, 0.7f, 0.7f, 1.0f};

    // Billboard texture path (optional).
    std::string billboardTexturePath;

    // Entity hierarchy stability:
    // When the real asset replaces this proxy, the entity UUID, transform,
    // selection, and undo history are preserved.
    bool preserveEntityOnReplacement = true;

    // When the proxy should fade out as real geometry fades in.
    float fadeOutDurationSeconds = 0.3f;
    float fadeInDurationSeconds = 0.3f;
};

// State of a progressive placement entity.
enum class PreviewPlacementState : uint8_t {
    // Just placed; shell with metadata only.
    MetadataShell,
    // Bounds or proxy geometry visible.
    ProxyVisible,
    // Material constants loaded, fallback textures bound.
    MaterialConstantsResident,
    // Low mip textures ready.
    LowMipsResident,
    // Full mesh resident, BLAS building.
    MeshResident,
    // Fully renderable (BLAS + materials + all mips).
    FullyResident,
    // Replacement by real asset in progress.
    ReplacementInProgress,
    // Failed to load; showing error indicator.
    Failed,
};

// Per-entity proxy tracking for progressive asset placement.
struct PreviewPlacementRecord {
    EntityId entityId;
    AssetGuid assetGuid;
    PreviewProxyKind activeProxyKind = PreviewProxyKind::BoundingBox;
    PreviewPlacementState state = PreviewPlacementState::MetadataShell;
    uint64_t placedFrameSerial = 0;
    uint32_t placedFrameCount = 0;
    float proxyFadeFactor = 1.0f;   // 1.0 = fully proxy, 0.0 = fully real
    bool selected = false;
    bool hovered = false;
    std::string debugLabel;
};

// Manages progressive proxy generation, replacement, and fade transitions
// for streamed asset placement. Each placed asset starts as a metadata shell
// with a bounds/proxy and progressively refines as real payloads stream in.
//
// Key behaviors:
// - Entity identity (UUID, transform, selection, undo history) is NEVER changed
//   during proxy-to-real replacement.
// - Proxies render with low-cost debug geometry or simple solid-color draws.
// - Fade-out of proxy and fade-in of real geometry are smooth (optional).
// - Per-entity state is exposed for the inspector and viewport overlay.
class PreviewProxyManager final : private NonCopyable{
public:
    PreviewProxyManager() = default;

    // Register a new proxy placement for an entity. Called immediately after
    // drag/drop or import-and-place creates the metadata shell.
    void registerPlacement(EntityId entityId, AssetGuid assetGuid,
                           const PreviewProxyDesc& proxy,
                           uint64_t frameSerial);

    // Called each frame to advance fade transitions and check for
    // state changes (proxy → real replacement eligibility).
    void tick(uint64_t currentFrameSerial, float deltaSeconds);

    // Signal that a specific asset's real payload has reached a given
    // residency milestone. Updates the proxy state accordingly.
    void signalAssetProgress(AssetGuid guid, PreviewPlacementState newState);

    // Signal that the real geometry is ready to replace the proxy.
    // Triggers fade-out of proxy and fade-in of real geometry.
    void signalReplacementReady(EntityId entityId);

    // Remove a placement record (on entity delete / undo).
    void unregisterPlacement(EntityId entityId);

    // Query current proxy state for an entity.
    [[nodiscard]] const PreviewPlacementRecord* find(EntityId entityId) const;
    [[nodiscard]] const PreviewPlacementRecord* findByAssetGuid(AssetGuid guid) const;

    [[nodiscard]] const std::vector<PreviewPlacementRecord>& records() const { return records_; }

    // Generate a bounding-box proxy from metadata bounds.
    [[nodiscard]] static PreviewProxyDesc makeBoundingBoxProxy(
        const std::array<float, 3>& boundsMin,
        const std::array<float, 3>& boundsMax);

    // Settings.
    struct Settings {
        PreviewProxyKind defaultProxyKind = PreviewProxyKind::BoundingBox;
        bool fadeEnabled = true;
        float fadeDuration = 0.3f;
        bool showBoundsForPending = true;
        bool showBoundsForSelected = true;
        bool renderPendingAsProxy = true;
    };

    void setSettings(const Settings& settings) { settings_ = settings; }
    [[nodiscard]] const Settings& settings() const { return settings_; }

private:
    Settings settings_{};
    std::vector<PreviewPlacementRecord> records_;
};

[[nodiscard]] const char* previewProxyKindName(PreviewProxyKind kind);
[[nodiscard]] const char* previewPlacementStateName(PreviewPlacementState state);
[[nodiscard]] nlohmann::json previewPlacementRecordToJson(const PreviewPlacementRecord& record);

} // namespace rtv
