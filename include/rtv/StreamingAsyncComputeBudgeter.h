#pragma once

#include "rtv/NonCopyable.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>

namespace rtv {

// Per-frame decision maker for whether streaming GPU work (decompression,
// transcode, compute-based preprocessing) should be submitted to the async
// compute queue instead of the transfer or graphics queue.
//
// The budgeter keeps a rolling estimate of path tracing GPU time and the
// available GPU frame budget, then decides per frame:
//   1. How many microseconds streaming compute may consume on async.
//   2. Whether to allow full async, partial async, or fall back to sync.
//   3. Whether streaming should throttle because the path tracer is near budget.
//
// All decisions are exposed as JSON-serializable decisions for diagnostics.
class StreamingAsyncComputeBudgeter final : private NonCopyable {
public:
    enum class Decision : uint8_t {
        // Use async compute freely — path tracer has plenty of headroom.
        AllowAsync,
        // Use async compute sparingly — cap at half the available headroom.
        PartialAsync,
        // Do not use async compute — either not available or path tracer
        // is already near/over budget.
        FallbackSync,
    };

    struct FrameState {
        // Rolling estimates (microseconds).
        uint64_t pathTracerGpuUs = 0;      // p50 of recent path trace times
        uint64_t pathTracerGpuUsP95 = 0;   // p95 tail
        uint64_t asyncHeadroomUs = 0;      // frame budget minus path trace p50
        uint64_t streamingAsyncBudgetUs = 0; // allowed streaming async us this frame

        Decision decision = Decision::FallbackSync;
        bool asyncComputeAvailable = false;
        bool streamingAsyncEnabled = false;
        uint64_t frameIndex = 0;
    };

    StreamingAsyncComputeBudgeter() = default;

    // Configure the budgeter. Called once at init or when settings change.
    void configure(uint64_t frameBudgetUs, uint64_t minStreamingHeadroomUs,
                   bool asyncComputeAvailable, bool streamingAsyncEnabled);

    // Called every frame before recording. pathTracerGpuUs is the path tracer's
    // GPU duration this frame (from profiler). Returns true if streaming compute
    // may record this frame.
    [[nodiscard]] bool beginFrame(uint64_t pathTracerGpuUs, uint64_t frameIndex);

    // Called after frame recording to record actual usage.
    void endFrame(uint64_t streamingAsyncUsUsed);

    [[nodiscard]] const FrameState& state() const { return state_; }
    [[nodiscard]] FrameState snapshot() const { return state_; }

    // Force sync fallback for testing/validation.
    void forceFallbackSync() { forceFallback_ = true; }
    void clearForceFallback() { forceFallback_ = false; }
    [[nodiscard]] bool forceFallback() const { return forceFallback_; }

private:
    void updateRollingEstimate(uint64_t newUs);

    uint64_t frameBudgetUs_ = 16600;       // default ~16.6ms
    uint64_t minStreamingHeadroomUs_ = 2000; // minimum 2ms headroom for streaming async
    bool asyncComputeAvailable_ = false;
    bool streamingAsyncEnabled_ = true;
    bool forceFallback_ = false;

    // Rolling average for path tracer GPU time (exponential moving average).
    uint64_t rollingPathTracerUs_ = 0;
    static constexpr double kEmaAlpha = 0.2;

    FrameState state_{};
};

[[nodiscard]] nlohmann::json streamingAsyncComputeBudgeterStateJson(
    const StreamingAsyncComputeBudgeter::FrameState& state);

} // namespace rtv
