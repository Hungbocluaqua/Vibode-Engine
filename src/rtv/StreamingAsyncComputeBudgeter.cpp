#include "rtv/StreamingAsyncComputeBudgeter.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace rtv {

void StreamingAsyncComputeBudgeter::configure(uint64_t frameBudgetUs, uint64_t minStreamingHeadroomUs,
                                               bool asyncComputeAvailable, bool streamingAsyncEnabled) {
    frameBudgetUs_ = frameBudgetUs;
    minStreamingHeadroomUs_ = minStreamingHeadroomUs;
    asyncComputeAvailable_ = asyncComputeAvailable;
    streamingAsyncEnabled_ = streamingAsyncEnabled;
    rollingPathTracerUs_ = 0;
}

bool StreamingAsyncComputeBudgeter::beginFrame(uint64_t pathTracerGpuUs, uint64_t frameIndex) {
    state_.frameIndex = frameIndex;
    state_.pathTracerGpuUs = pathTracerGpuUs;
    state_.asyncComputeAvailable = asyncComputeAvailable_;
    state_.streamingAsyncEnabled = streamingAsyncEnabled_;

    updateRollingEstimate(pathTracerGpuUs);

    // p95 estimate: simple multiplier on the rolling average.
    state_.pathTracerGpuUsP95 = static_cast<uint64_t>(static_cast<double>(rollingPathTracerUs_) * 1.3);

    // Headroom = frame budget minus current path tracer rolling estimate.
    const int64_t headroom = static_cast<int64_t>(frameBudgetUs_) - static_cast<int64_t>(rollingPathTracerUs_);
    state_.asyncHeadroomUs = headroom > 0 ? static_cast<uint64_t>(headroom) : 0;

    const bool asyncAvailable = asyncComputeAvailable_ && streamingAsyncEnabled_ && !forceFallback_;
    if (!asyncAvailable || state_.asyncHeadroomUs < minStreamingHeadroomUs_) {
        state_.decision = Decision::FallbackSync;
        state_.streamingAsyncBudgetUs = 0;
        return false;
    }

    // If plenty of headroom, allow full async. Otherwise partial.
    const uint64_t generousHeadroom = minStreamingHeadroomUs_ * 3;
    if (state_.asyncHeadroomUs >= generousHeadroom) {
        state_.decision = Decision::AllowAsync;
        // Budget: up to half the headroom, capped at 2ms.
        state_.streamingAsyncBudgetUs = std::min<uint64_t>(state_.asyncHeadroomUs / 2, 2000);
    } else {
        state_.decision = Decision::PartialAsync;
        // Budget: up to a quarter of the headroom, capped at 1ms.
        state_.streamingAsyncBudgetUs = std::min<uint64_t>(state_.asyncHeadroomUs / 4, 1000);
    }

    return state_.streamingAsyncBudgetUs > 0;
}

void StreamingAsyncComputeBudgeter::endFrame(uint64_t streamingAsyncUsUsed) {
    (void)streamingAsyncUsUsed;
    // Future: track actual usage to refine estimates.
}

void StreamingAsyncComputeBudgeter::updateRollingEstimate(uint64_t newUs) {
    if (rollingPathTracerUs_ == 0) {
        rollingPathTracerUs_ = newUs;
    } else {
        rollingPathTracerUs_ = static_cast<uint64_t>(
            static_cast<double>(rollingPathTracerUs_) * (1.0 - kEmaAlpha) +
            static_cast<double>(newUs) * kEmaAlpha);
    }
    // Clamp to a reasonable range.
    rollingPathTracerUs_ = std::min<uint64_t>(rollingPathTracerUs_, frameBudgetUs_ * 2);
}

nlohmann::json streamingAsyncComputeBudgeterStateJson(
    const StreamingAsyncComputeBudgeter::FrameState& state) {
    const char* decisionStr = "fallback_sync";
    switch (state.decision) {
    case StreamingAsyncComputeBudgeter::Decision::AllowAsync:
        decisionStr = "allow_async";
        break;
    case StreamingAsyncComputeBudgeter::Decision::PartialAsync:
        decisionStr = "partial_async";
        break;
    case StreamingAsyncComputeBudgeter::Decision::FallbackSync:
        decisionStr = "fallback_sync";
        break;
    }

    return nlohmann::json{
        {"path_tracer_gpu_us", state.pathTracerGpuUs},
        {"path_tracer_gpu_us_p95", state.pathTracerGpuUsP95},
        {"async_headroom_us", state.asyncHeadroomUs},
        {"streaming_async_budget_us", state.streamingAsyncBudgetUs},
        {"decision", decisionStr},
        {"async_compute_available", state.asyncComputeAvailable},
        {"streaming_async_enabled", state.streamingAsyncEnabled},
        {"frame_index", state.frameIndex},
    };
}

} // namespace rtv
