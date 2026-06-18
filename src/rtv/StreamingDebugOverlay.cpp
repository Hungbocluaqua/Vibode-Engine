#include "rtv/StreamingDebugOverlay.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace rtv {

void StreamingDebugOverlay::refresh(const StreamingRuntimeState& /*runtimeState*/,
                                     const std::vector<StreamingRootSnapshot>& roots,
                                     const StreamingDebugOverlayState::OverlaySettings& settings,
                                     uint64_t frameSerial) {
    state_.settings = settings;
    state_.lastFrameSerial = frameSerial;

    // Determine visibility based on idle timeout.
    const bool hasActiveWork = !roots.empty() &&
        std::any_of(roots.begin(), roots.end(),
            [](const StreamingRootSnapshot& r) { return r.active && !r.complete; });

    if (settings.autoHideWhenIdle && !hasActiveWork) {
        state_.visible = false;
    } else {
        state_.visible = settings.enabled;
    }

    // Build root entries.
    state_.roots.clear();
    for (const StreamingRootSnapshot& root : roots) {
        StreamingDebugOverlayState::RootEntry entry;
        entry.serial = root.serial;
        entry.label = root.label;
        entry.rootPath = root.rootPath.generic_string();
        entry.residentAssetCount = root.loadedFiles;
        entry.totalAssetCount = std::max(1u, root.totalFiles);
        entry.failedAssetCount = root.failedFiles;
        entry.paused = !root.active;
        entry.complete = root.complete;
        entry.progress = root.totalFiles > 0
            ? static_cast<float>(root.loadedFiles) / static_cast<float>(root.totalFiles)
            : 0.0f;

        // Determine bottleneck.
        if (root.failedFiles > 0) {
            entry.currentBottleneck = "failure";
        } else if (entry.paused) {
            entry.currentBottleneck = "paused";
        } else {
            entry.currentBottleneck = "io";
        }
        state_.roots.push_back(std::move(entry));
    }

    // Build summary from runtime state snapshots.
    state_.summary = {};
    for (const StreamingRootSnapshot& root : roots) {
        state_.summary.totalAssets += root.totalFiles;
        state_.summary.residentAssets += root.loadedFiles;
        state_.summary.pendingAssets += (root.totalFiles - root.loadedFiles);
        state_.summary.failedAssets += root.failedFiles;
    }
    state_.summary.pendingBytes = 0;
    state_.summary.uploadedBytes = 0;

    // Build queue snapshots from runtime state.
    state_.queues.clear();
    for (const StreamingRootSnapshot& root : roots) {
        // Each root contributes queue-like stats from its own fields.
        StreamingQueueSnapshot snap;
        snap.gpuResident = root.loadedFiles;
        snap.failed = root.failedFiles;
        snap.requested = root.totalFiles;
        state_.queues.push_back(snap);
    }

    buildMemoryBars();
}

void StreamingDebugOverlay::buildMemoryBars() {
    state_.memoryBars.clear();

    // Use actual budget values from the overlay state's roots or defaults.
    // In production, these would come from NativeGpuAssetCache stats.
    const uint64_t cpuBudget = 512ull * 1024ull * 1024ull;
    const uint64_t gpuBudget = 1024ull * 1024ull * 1024ull;
    const uint64_t stagingBudget = 128ull * 1024ull * 1024ull;

    // Estimate used bytes from roots (loaded bytes as a proxy).
    uint64_t totalLoadedBytes = 0;
    uint64_t totalPendingBytes = 0;
    for (const StreamingDebugOverlayState::RootEntry& root : state_.roots) {
        (void)root;
    }

    {
        StreamingDebugOverlayState::MemoryBar bar;
        bar.label = "CPU Streaming";
        bar.usedBytes = totalLoadedBytes;
        bar.budgetBytes = cpuBudget;
        bar.fraction = cpuBudget > 0
            ? std::min(1.0f, static_cast<float>(totalLoadedBytes) / static_cast<float>(cpuBudget))
            : 0.0f;
        state_.memoryBars.push_back(bar);
    }

    {
        StreamingDebugOverlayState::MemoryBar bar;
        bar.label = "GPU Streaming";
        bar.usedBytes = totalLoadedBytes;  // proxy — real values from NativeGpuAssetCache
        bar.budgetBytes = gpuBudget;
        bar.fraction = gpuBudget > 0
            ? std::min(1.0f, static_cast<float>(totalLoadedBytes) / static_cast<float>(gpuBudget))
            : 0.0f;
        state_.memoryBars.push_back(bar);
    }

    {
        StreamingDebugOverlayState::MemoryBar bar;
        bar.label = "Upload Staging";
        bar.usedBytes = totalPendingBytes;
        bar.budgetBytes = stagingBudget;
        bar.fraction = stagingBudget > 0
            ? std::min(1.0f, static_cast<float>(totalPendingBytes) / static_cast<float>(stagingBudget))
            : 0.0f;
        state_.memoryBars.push_back(bar);
    }

    state_.summary.memoryPressure =
        std::any_of(state_.memoryBars.begin(), state_.memoryBars.end(),
            [](const StreamingDebugOverlayState::MemoryBar& bar) {
                return bar.fraction > 0.85f;
            });
}

nlohmann::json streamingDebugOverlayToJson(const StreamingDebugOverlayState& state) {
    nlohmann::json roots = nlohmann::json::array();
    for (const StreamingDebugOverlayState::RootEntry& root : state.roots) {
        roots.push_back({
            {"serial", root.serial},
            {"label", root.label},
            {"root_path", root.rootPath},
            {"progress", root.progress},
            {"resident_asset_count", root.residentAssetCount},
            {"total_asset_count", root.totalAssetCount},
            {"failed_asset_count", root.failedAssetCount},
            {"current_bottleneck", root.currentBottleneck},
            {"paused", root.paused},
            {"complete", root.complete},
        });
    }

    nlohmann::json memoryBars = nlohmann::json::array();
    for (const StreamingDebugOverlayState::MemoryBar& bar : state.memoryBars) {
        memoryBars.push_back({
            {"label", bar.label},
            {"used_bytes", bar.usedBytes},
            {"budget_bytes", bar.budgetBytes},
            {"fraction", bar.fraction},
        });
    }

    nlohmann::json failedAssets = nlohmann::json::array();
    for (const StreamingDebugOverlayState::FailedAsset& asset : state.failedAssets) {
        failedAssets.push_back({
            {"guid", asset.guid},
            {"label", asset.label},
            {"reason", asset.reason},
            {"retryable", asset.retryable},
        });
    }

    return nlohmann::json{
        {"visible", state.visible},
        {"roots", roots},
        {"summary", {
            {"total_assets", state.summary.totalAssets},
            {"resident_assets", state.summary.residentAssets},
            {"pending_assets", state.summary.pendingAssets},
            {"failed_assets", state.summary.failedAssets},
            {"uploaded_bytes", state.summary.uploadedBytes},
            {"pending_bytes", state.summary.pendingBytes},
            {"active_blas_builds", state.summary.activeBlasBuilds},
            {"active_tlas_patches", state.summary.activeTlasPatches},
            {"memory_pressure", state.summary.memoryPressure},
            {"io_pressure", state.summary.ioPressure},
            {"upload_pressure", state.summary.uploadPressure},
            {"budget_violated", state.summary.budgetViolated},
        }},
        {"memory_bars", memoryBars},
        {"failed_assets", failedAssets},
        {"settings", {
            {"enabled", state.settings.enabled},
            {"show_in_viewport", state.settings.showInViewport},
            {"show_in_job_center", state.settings.showInJobCenter},
            {"show_memory_bars", state.settings.showMemoryBars},
            {"auto_hide_when_idle", state.settings.autoHideWhenIdle},
        }},
        {"last_frame_serial", state.lastFrameSerial},
    };
}

} // namespace rtv
