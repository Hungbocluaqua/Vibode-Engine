#include "rtv/GpuValidation.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <utility>

namespace rtv {

namespace {

constexpr size_t kManualBarrierRecentEventLimit = 4096;

struct ManualBarrierEscapeDiagnosticsState {
    std::mutex mutex;
    bool enabled = false;
    uint64_t dependencyCallCount = 0;
    uint64_t barrierCount = 0;
    uint64_t droppedRecentEventCount = 0;
    std::vector<ManualBarrierEscapeEvent> recentEvents;
    std::vector<ManualBarrierEscapeAggregate> aggregates;
};

ManualBarrierEscapeDiagnosticsState& manualBarrierState() {
    static ManualBarrierEscapeDiagnosticsState state;
    return state;
}

template <typename Handle>
uint64_t vulkanHandleValue(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<uint64_t>(handle);
    } else {
        return static_cast<uint64_t>(handle);
    }
}

bool matchesAggregate(const ManualBarrierEscapeAggregate& aggregate, const ManualBarrierEscapeEvent& event) {
    return aggregate.source == event.source &&
        aggregate.label == event.label &&
        aggregate.resourceKind == event.resourceKind &&
        aggregate.srcStage == event.srcStage &&
        aggregate.srcAccess == event.srcAccess &&
        aggregate.dstStage == event.dstStage &&
        aggregate.dstAccess == event.dstAccess &&
        aggregate.oldLayout == event.oldLayout &&
        aggregate.newLayout == event.newLayout;
}

void recordManualBarrierEscapeLocked(
    ManualBarrierEscapeDiagnosticsState& state,
    ManualBarrierEscapeEvent event) {
    event.sequence = ++state.barrierCount;
    if (state.recentEvents.size() < kManualBarrierRecentEventLimit) {
        state.recentEvents.push_back(event);
    } else {
        ++state.droppedRecentEventCount;
    }

    auto aggregateIt = std::find_if(
        state.aggregates.begin(),
        state.aggregates.end(),
        [&](const ManualBarrierEscapeAggregate& aggregate) {
            return matchesAggregate(aggregate, event);
        });
    if (aggregateIt != state.aggregates.end()) {
        ++aggregateIt->count;
        return;
    }

    state.aggregates.push_back(ManualBarrierEscapeAggregate{
        .source = event.source,
        .label = event.label,
        .resourceKind = event.resourceKind,
        .srcStage = event.srcStage,
        .srcAccess = event.srcAccess,
        .dstStage = event.dstStage,
        .dstAccess = event.dstAccess,
        .oldLayout = event.oldLayout,
        .newLayout = event.newLayout,
        .count = 1,
    });
}

} // namespace

void setManualBarrierEscapeDiagnosticsEnabled(bool enabled) {
    auto& state = manualBarrierState();
    std::lock_guard lock(state.mutex);
    state.enabled = enabled;
}

void resetManualBarrierEscapeDiagnostics() {
    auto& state = manualBarrierState();
    std::lock_guard lock(state.mutex);
    state.dependencyCallCount = 0;
    state.barrierCount = 0;
    state.droppedRecentEventCount = 0;
    state.recentEvents.clear();
    state.aggregates.clear();
}

void recordManualBarrierEscape(std::string source, std::string label, const VkDependencyInfo& dependency) {
    auto& state = manualBarrierState();
    std::lock_guard lock(state.mutex);
    if (!state.enabled) {
        return;
    }
    ++state.dependencyCallCount;

    for (uint32_t i = 0; i < dependency.memoryBarrierCount; ++i) {
        const VkMemoryBarrier2& barrier = dependency.pMemoryBarriers[i];
        recordManualBarrierEscapeLocked(state, ManualBarrierEscapeEvent{
            .source = source,
            .label = label,
            .resourceKind = "memory",
            .srcStage = barrier.srcStageMask,
            .srcAccess = barrier.srcAccessMask,
            .dstStage = barrier.dstStageMask,
            .dstAccess = barrier.dstAccessMask,
        });
    }
    for (uint32_t i = 0; i < dependency.bufferMemoryBarrierCount; ++i) {
        const VkBufferMemoryBarrier2& barrier = dependency.pBufferMemoryBarriers[i];
        recordManualBarrierEscapeLocked(state, ManualBarrierEscapeEvent{
            .source = source,
            .label = label,
            .resourceKind = "buffer",
            .resourceHandle = vulkanHandleValue(barrier.buffer),
            .srcStage = barrier.srcStageMask,
            .srcAccess = barrier.srcAccessMask,
            .dstStage = barrier.dstStageMask,
            .dstAccess = barrier.dstAccessMask,
        });
    }
    for (uint32_t i = 0; i < dependency.imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2& barrier = dependency.pImageMemoryBarriers[i];
        recordManualBarrierEscapeLocked(state, ManualBarrierEscapeEvent{
            .source = source,
            .label = label,
            .resourceKind = "image",
            .resourceHandle = vulkanHandleValue(barrier.image),
            .srcStage = barrier.srcStageMask,
            .srcAccess = barrier.srcAccessMask,
            .dstStage = barrier.dstStageMask,
            .dstAccess = barrier.dstAccessMask,
            .oldLayout = barrier.oldLayout,
            .newLayout = barrier.newLayout,
        });
    }
}

ManualBarrierEscapeDiagnosticsSnapshot manualBarrierEscapeDiagnosticsSnapshot() {
    auto& state = manualBarrierState();
    std::lock_guard lock(state.mutex);
    ManualBarrierEscapeDiagnosticsSnapshot snapshot;
    snapshot.enabled = state.enabled;
    snapshot.dependencyCallCount = state.dependencyCallCount;
    snapshot.barrierCount = state.barrierCount;
    snapshot.droppedRecentEventCount = state.droppedRecentEventCount;
    snapshot.recentEvents = state.recentEvents;
    snapshot.aggregates = state.aggregates;
    return snapshot;
}

void RendererValidationLog::recordBarrier(std::string label, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    std::ostringstream stream;
    stream << label << ": stage " << srcStage << "/" << dstStage << " access " << srcAccess << "/" << dstAccess;
    barrierEvents_.push_back(stream.str());
}

void RendererValidationLog::recordResourceState(ResourceStateEvent event) {
    resourceStateEvents_.push_back(std::move(event));
}

void RendererValidationLog::recordAccumulationInvalidation(std::string reason, uint64_t frame) {
    invalidations_.push_back({std::move(reason), frame});
    if (invalidations_.size() > 64) {
        invalidations_.erase(invalidations_.begin(), invalidations_.begin() + static_cast<std::ptrdiff_t>(invalidations_.size() - 64));
    }
}

void RendererValidationLog::recordSceneUpdateRoute(std::string kind, std::string action, double cpuMs) {
    for (SceneUpdateRouteEvent& route : sceneUpdateRoutes_) {
        if (route.kind == kind && route.action == action) {
            ++route.count;
            route.totalCpuMs += cpuMs;
            route.lastCpuMs = cpuMs;
            if (route.minCpuMs <= 0.0 || cpuMs < route.minCpuMs) {
                route.minCpuMs = cpuMs;
            }
            if (cpuMs > route.maxCpuMs) {
                route.maxCpuMs = cpuMs;
            }
            return;
        }
    }
    sceneUpdateRoutes_.push_back(SceneUpdateRouteEvent{
        .kind = std::move(kind),
        .action = std::move(action),
        .count = 1,
        .totalCpuMs = cpuMs,
        .lastCpuMs = cpuMs,
        .minCpuMs = cpuMs,
        .maxCpuMs = cpuMs,
    });
}

void RendererValidationLog::recordSchedulerQueueEvent(std::string queue, std::string job, std::string status, uint64_t generation, double cpuMs) {
    const bool frameBudgetViolation = status.find("budget_exhausted") != std::string::npos;
    for (SchedulerQueueEvent& event : schedulerQueueEvents_) {
        if (event.queue == queue && event.job == job && event.status == status && event.generation == generation) {
            ++event.count;
            event.totalCpuMs += cpuMs;
            event.lastCpuMs = cpuMs;
            event.frameBudgetViolationCount += frameBudgetViolation ? 1ull : 0ull;
            if (event.minCpuMs <= 0.0 || cpuMs < event.minCpuMs) {
                event.minCpuMs = cpuMs;
            }
            if (cpuMs > event.maxCpuMs) {
                event.maxCpuMs = cpuMs;
            }
            return;
        }
    }
    schedulerQueueEvents_.push_back(SchedulerQueueEvent{
        .queue = std::move(queue),
        .job = std::move(job),
        .status = std::move(status),
        .generation = generation,
        .count = 1,
        .totalCpuMs = cpuMs,
        .lastCpuMs = cpuMs,
        .minCpuMs = cpuMs,
        .maxCpuMs = cpuMs,
        .frameBudgetViolationCount = frameBudgetViolation ? 1ull : 0ull,
    });
}

void RendererValidationLog::beginFrame(uint64_t frame) {
    currentFrame_ = frame;
    passEvents_.clear();
    barrierEvents_.clear();
    resourceStateEvents_.clear();
}

void RendererValidationLog::recordPass(std::string label) {
    std::ostringstream stream;
    stream << "frame " << currentFrame_ << ": " << label;
    passEvents_.push_back(stream.str());
}

} // namespace rtv
