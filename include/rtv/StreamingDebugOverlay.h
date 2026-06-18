#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/StreamingRuntime.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

// Editor streaming debug overlay state.
// Provides real-time viewport visibility into:
// - Active streaming roots and their progress.
// - Per-asset residency state (mesh, BLAS, texture mips, materials).
// - Memory pressure and budget utilization.
// - Failed assets with actionable reasons.
// - Queue snapshots (I/O, decode, upload, BLAS, TLAS).
//
// This data is consumed by the EditorLayer viewport overlay and
// exposed through --dump-streaming JSON for headless diagnostics.
struct StreamingDebugOverlayState {
    // Active roots being streamed.
    struct RootEntry {
        uint64_t serial = 0;
        std::string label;
        std::string rootPath;
        float progress = 0.0f;               // 0..1
        uint32_t residentAssetCount = 0;
        uint32_t totalAssetCount = 0;
        uint32_t failedAssetCount = 0;
        std::string currentBottleneck;       // "io", "decode", "upload", "blas", "memory"
        bool paused = false;
        bool complete = false;
    };
    std::vector<RootEntry> roots;

    // Quick-glance summary.
    struct SummaryStats {
        uint32_t totalAssets = 0;
        uint32_t residentAssets = 0;
        uint32_t pendingAssets = 0;
        uint32_t failedAssets = 0;
        uint64_t uploadedBytes = 0;
        uint64_t pendingBytes = 0;
        uint32_t activeBlasBuilds = 0;
        uint32_t activeTlasPatches = 0;
        bool memoryPressure = false;
        bool ioPressure = false;
        bool uploadPressure = false;
        bool budgetViolated = false;
    } summary;

    // Per-queue snapshot for the Job Center / debug panel.
    std::vector<StreamingQueueSnapshot> queues;

    // Memory bar visualization data.
    struct MemoryBar {
        std::string label;
        uint64_t usedBytes = 0;
        uint64_t budgetBytes = 0;
        float fraction = 0.0f;
    };
    std::vector<MemoryBar> memoryBars;

    // Failed assets with actionable reasons.
    struct FailedAsset {
        std::string guid;
        std::string label;
        std::string reason;
        bool retryable = false;
    };
    std::vector<FailedAsset> failedAssets;

    // Editor settings for the overlay.
    struct OverlaySettings {
        bool enabled = true;
        bool showInViewport = false;         // Overlay in 3D viewport.
        bool showInJobCenter = true;         // Panel in Job Center.
        bool showMemoryBars = true;
        bool showQueueSnapshots = true;
        bool showFailedAssets = true;
        bool autoHideWhenIdle = true;        // Hide overlay when nothing is streaming.
        float idleTimeoutSeconds = 3.0f;
    } settings;

    // Whether the overlay is currently visible (idle timeout may hide it).
    bool visible = false;

    // Last update frame serial.
    uint64_t lastFrameSerial = 0;
};

// Manages the editor streaming debug overlay state.
//
// Updated each frame from live streaming state (NativeGpuAssetCache,
// GpuSceneStreamingState, StreamingGpuWorkQueue, StreamingScheduler,
// StreamingRuntimeState) and consumed by the editor viewport overlay
// and Job Center panel.
class StreamingDebugOverlay final : private NonCopyable {
public:
    StreamingDebugOverlay() = default;

    // Refresh the overlay state from live streaming components.
    // Called each frame before the editor overlay renders.
    void refresh(const StreamingRuntimeState& runtimeState,
                 const std::vector<StreamingRootSnapshot>& roots,
                 const StreamingDebugOverlayState::OverlaySettings& settings,
                 uint64_t frameSerial);

    [[nodiscard]] const StreamingDebugOverlayState& state() const { return state_; }

private:
    void buildMemoryBars();
    StreamingDebugOverlayState state_;
};

[[nodiscard]] nlohmann::json streamingDebugOverlayToJson(const StreamingDebugOverlayState& state);

} // namespace rtv
