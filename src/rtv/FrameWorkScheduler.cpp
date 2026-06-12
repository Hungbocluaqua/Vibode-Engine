#include "rtv/FrameWorkScheduler.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rtv {
namespace {

constexpr size_t maxRecentJobs = 64;

[[nodiscard]] size_t queueIndex(FrameWorkQueue queue) {
    const auto value = static_cast<size_t>(queue);
    return value < static_cast<size_t>(FrameWorkQueue::Count) ? value : 0u;
}

[[nodiscard]] bool finished(FrameWorkJobStatus status) {
    return status == FrameWorkJobStatus::Cancelled || status == FrameWorkJobStatus::Failed || status == FrameWorkJobStatus::Complete;
}

} // namespace

const char* frameWorkQueueName(FrameWorkQueue queue) {
    switch (queue) {
    case FrameWorkQueue::MainThreadApply: return "MainThreadApply";
    case FrameWorkQueue::CpuCook: return "CpuCook";
    case FrameWorkQueue::GpuUploadPrepare: return "GpuUploadPrepare";
    case FrameWorkQueue::GpuUploadSubmit: return "GpuUploadSubmit";
    case FrameWorkQueue::GpuSceneBuild: return "GpuSceneBuild";
    case FrameWorkQueue::AccelerationStructureBuild: return "AccelerationStructureBuild";
    case FrameWorkQueue::RendererSwap: return "RendererSwap";
    case FrameWorkQueue::Count: break;
    }
    return "Unknown";
}

const char* frameWorkJobStatusName(FrameWorkJobStatus status) {
    switch (status) {
    case FrameWorkJobStatus::Queued: return "queued";
    case FrameWorkJobStatus::Running: return "running";
    case FrameWorkJobStatus::WaitingForFence: return "waitingForFence";
    case FrameWorkJobStatus::Cancelled: return "cancelled";
    case FrameWorkJobStatus::Failed: return "failed";
    case FrameWorkJobStatus::Complete: return "complete";
    }
    return "unknown";
}

uint64_t FrameWorkScheduler::enqueue(FrameWorkJobDesc desc) {
    JobState job;
    job.id = nextJobId_++;
    if (desc.title.empty()) {
        desc.title = frameWorkQueueName(desc.queue);
    }
    if (desc.status.empty()) {
        desc.status = "queued";
    }
    job.desc = std::move(desc);
    jobs_.push_back(std::move(job));
    return jobs_.back().id;
}

uint64_t FrameWorkScheduler::enqueueNoOp(FrameWorkQueue queue, std::string title) {
    FrameWorkJobDesc desc;
    desc.queue = queue;
    desc.title = title.empty() ? std::string(frameWorkQueueName(queue)) + " no-op" : std::move(title);
    desc.status = "queued no-op";
    desc.callback = [](FrameWorkJobContext&) {
        return FrameWorkJobStepResult{.complete = true, .progress = 1.0f};
    };
    return enqueue(std::move(desc));
}

void FrameWorkScheduler::tick() {
    const FrameWorkResolvedBudgets budgets = resolvedBudgets();
    double usedMainApplyMs = 0.0;
    double usedUploadSubmitMs = 0.0;
    uint64_t usedUploadBytes = 0;
    double usedAsBuildMs = 0.0;
    for (JobState& job : jobs_) {
        if (job.status != FrameWorkJobStatus::Queued && job.status != FrameWorkJobStatus::Running) {
            continue;
        }
        if (!canRun(job, budgets, usedMainApplyMs, usedUploadSubmitMs, usedUploadBytes, usedAsBuildMs)) {
            continue;
        }

        job.status = FrameWorkJobStatus::Running;
        job.desc.status = "running";
        FrameWorkJobStepResult result;
        if (job.desc.callback) {
            FrameWorkJobContext context{.id = job.id, .queue = job.desc.queue, .frameBudgets = budgets};
            result = job.desc.callback(context);
        } else {
            result.complete = true;
            result.progress = 1.0f;
        }

        charge(job, usedMainApplyMs, usedUploadSubmitMs, usedUploadBytes, usedAsBuildMs);
        job.progress = std::clamp(result.progress, 0.0f, 1.0f);
        job.warnings.insert(job.warnings.end(), result.warnings.begin(), result.warnings.end());
        if (!result.failure.empty()) {
            job.failure = std::move(result.failure);
            job.desc.status = job.failure;
            job.status = FrameWorkJobStatus::Failed;
            appendRecent(job);
        } else if (result.waitingForFence) {
            job.desc.status = "waiting for fence";
            job.status = FrameWorkJobStatus::WaitingForFence;
        } else if (result.complete) {
            job.progress = 1.0f;
            job.desc.status = "complete";
            job.status = FrameWorkJobStatus::Complete;
            appendRecent(job);
        } else {
            job.desc.status = "queued";
            job.status = FrameWorkJobStatus::Queued;
        }
    }
}

bool FrameWorkScheduler::cancel(uint64_t id, std::string reason) {
    for (JobState& job : jobs_) {
        if (job.id != id || finished(job.status)) {
            continue;
        }
        job.status = FrameWorkJobStatus::Cancelled;
        job.desc.status = reason.empty() ? "cancelled" : reason;
        appendRecent(job);
        return true;
    }
    return false;
}

bool FrameWorkScheduler::completeFence(uint64_t id) {
    for (JobState& job : jobs_) {
        if (job.id != id || job.status != FrameWorkJobStatus::WaitingForFence) {
            continue;
        }
        job.status = FrameWorkJobStatus::Complete;
        job.progress = 1.0f;
        job.desc.status = "complete";
        appendRecent(job);
        return true;
    }
    return false;
}

void FrameWorkScheduler::clearFinished() {
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const JobState& job) {
        return finished(job.status);
    }), jobs_.end());
}

bool FrameWorkScheduler::empty() const {
    return jobs_.empty();
}

bool FrameWorkScheduler::hasActiveJobs() const {
    return std::any_of(jobs_.begin(), jobs_.end(), [](const JobState& job) {
        return !finished(job.status);
    });
}

size_t FrameWorkScheduler::liveJobCount() const {
    return static_cast<size_t>(std::count_if(jobs_.begin(), jobs_.end(), [](const JobState& job) {
        return !finished(job.status);
    }));
}

FrameWorkSchedulerSnapshot FrameWorkScheduler::snapshot() const {
    FrameWorkSchedulerSnapshot out;
    out.budgets = resolvedBudgets();
    for (size_t i = 0; i < out.queues.size(); ++i) {
        const auto queue = static_cast<FrameWorkQueue>(i);
        out.queues[i].queue = queue;
        out.queues[i].name = frameWorkQueueName(queue);
    }
    for (const JobState& job : jobs_) {
        FrameWorkQueueSnapshot& queue = out.queues[queueIndex(job.desc.queue)];
        switch (job.status) {
        case FrameWorkJobStatus::Queued: ++queue.queued; break;
        case FrameWorkJobStatus::Running: ++queue.running; break;
        case FrameWorkJobStatus::WaitingForFence: ++queue.waitingForFence; break;
        case FrameWorkJobStatus::Cancelled: ++queue.cancelled; break;
        case FrameWorkJobStatus::Failed: ++queue.failed; break;
        case FrameWorkJobStatus::Complete: ++queue.complete; break;
        }
        if (!finished(job.status)) {
            queue.activeJobs.push_back(makeSnapshot(job));
        }
    }
    out.recentJobs = recentJobs_;
    return out;
}

void FrameWorkScheduler::setBudgetConfig(const FrameWorkBudgetConfig& budgets) {
    budgets_ = budgets;
}

void FrameWorkScheduler::setPreviousAccelerationStructureGpuMs(double milliseconds) {
    budgets_.previousAccelerationStructureGpuMs = std::max(0.0, milliseconds);
}

FrameWorkResolvedBudgets FrameWorkScheduler::resolvedBudgets() const {
    FrameWorkResolvedBudgets out;
    out.mainThreadApplyMs = std::max(0.0, budgets_.mainThreadApplyMs);
    out.uploadSubmitMs = std::max(0.0, budgets_.uploadSubmitMs);
    out.uploadBytes = budgets_.uploadBytes;
    const double adaptive = budgets_.previousAccelerationStructureGpuMs * 0.75;
    out.accelerationStructureBuildMs = std::clamp(
        std::isfinite(adaptive) ? adaptive : budgets_.minAccelerationStructureBuildMs,
        std::max(0.0, budgets_.minAccelerationStructureBuildMs),
        std::max(budgets_.minAccelerationStructureBuildMs, budgets_.maxAccelerationStructureBuildMs));
    return out;
}

bool FrameWorkScheduler::canRun(const JobState& job, const FrameWorkResolvedBudgets& budgets, double& usedMainApplyMs, double& usedUploadSubmitMs, uint64_t& usedUploadBytes, double& usedAsBuildMs) const {
    const double costMs = std::max(0.0, job.desc.estimatedCostMs);
    switch (job.desc.queue) {
    case FrameWorkQueue::MainThreadApply:
        return costMs == 0.0 || usedMainApplyMs + costMs <= budgets.mainThreadApplyMs;
    case FrameWorkQueue::GpuUploadSubmit:
        return (costMs == 0.0 || usedUploadSubmitMs + costMs <= budgets.uploadSubmitMs) &&
            (job.desc.estimatedUploadBytes == 0 || usedUploadBytes + job.desc.estimatedUploadBytes <= budgets.uploadBytes);
    case FrameWorkQueue::AccelerationStructureBuild:
        return costMs == 0.0 || usedAsBuildMs + costMs <= budgets.accelerationStructureBuildMs;
    case FrameWorkQueue::CpuCook:
    case FrameWorkQueue::GpuUploadPrepare:
    case FrameWorkQueue::GpuSceneBuild:
    case FrameWorkQueue::RendererSwap:
        return true;
    case FrameWorkQueue::Count:
        break;
    }
    return false;
}

void FrameWorkScheduler::charge(const JobState& job, double& usedMainApplyMs, double& usedUploadSubmitMs, uint64_t& usedUploadBytes, double& usedAsBuildMs) const {
    const double costMs = std::max(0.0, job.desc.estimatedCostMs);
    switch (job.desc.queue) {
    case FrameWorkQueue::MainThreadApply:
        usedMainApplyMs += costMs;
        break;
    case FrameWorkQueue::GpuUploadSubmit:
        usedUploadSubmitMs += costMs;
        usedUploadBytes += job.desc.estimatedUploadBytes;
        break;
    case FrameWorkQueue::AccelerationStructureBuild:
        usedAsBuildMs += costMs;
        break;
    case FrameWorkQueue::CpuCook:
    case FrameWorkQueue::GpuUploadPrepare:
    case FrameWorkQueue::GpuSceneBuild:
    case FrameWorkQueue::RendererSwap:
    case FrameWorkQueue::Count:
        break;
    }
}

void FrameWorkScheduler::appendRecent(const JobState& job) {
    recentJobs_.push_back(makeSnapshot(job));
    while (recentJobs_.size() > maxRecentJobs) {
        recentJobs_.erase(recentJobs_.begin());
    }
}

FrameWorkJobSnapshot FrameWorkScheduler::makeSnapshot(const JobState& job) const {
    return {
        .id = job.id,
        .queue = job.desc.queue,
        .status = job.status,
        .queueName = frameWorkQueueName(job.desc.queue),
        .title = job.desc.title,
        .statusText = job.desc.status.empty() ? frameWorkJobStatusName(job.status) : job.desc.status,
        .progress = job.progress,
        .warnings = job.warnings,
        .failure = job.failure,
    };
}

} // namespace rtv
