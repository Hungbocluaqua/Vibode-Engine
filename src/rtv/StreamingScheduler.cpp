#include "rtv/StreamingScheduler.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>

namespace rtv {
namespace {

bool terminal(StreamingTaskState state) {
    return state == StreamingTaskState::Complete ||
        state == StreamingTaskState::Cancelled ||
        state == StreamingTaskState::Failed;
}

int effectivePriority(const StreamingTaskDesc& desc, uint32_t queuedFrames) {
    int priority = desc.priority + static_cast<int>(queuedFrames / 2u);
    if (desc.selectedBoost) {
        priority += 100;
    }
    if (desc.visibleBoost) {
        priority += 25;
    }
    return priority;
}

nlohmann::json frameResultJson(const StreamingSchedulerFrameResult& frame) {
    return {
        {"frame_index", frame.frameIndex},
        {"completed_tasks", frame.completedTasks},
        {"cancelled_tasks", frame.cancelledTasks},
        {"continued_tasks", frame.continuedTasks},
        {"blocked_by_dependency_tasks", frame.blockedByDependencyTasks},
        {"consumed_io_bytes", frame.consumedIoBytes},
        {"consumed_upload_bytes", frame.consumedUploadBytes},
        {"peak_transient_memory_bytes", frame.peakTransientMemoryBytes},
        {"reserved_transient_memory_bytes", frame.reservedTransientMemoryBytes},
        {"consumed_cpu_ms", frame.consumedCpuMs},
        {"task_budget_exhausted", frame.taskBudgetExhausted},
        {"cpu_budget_exhausted", frame.cpuBudgetExhausted},
        {"io_budget_exhausted", frame.ioBudgetExhausted},
        {"upload_budget_exhausted", frame.uploadBudgetExhausted},
        {"memory_budget_exhausted", frame.memoryBudgetExhausted},
    };
}

} // namespace

const char* streamingTaskKindName(StreamingTaskKind kind) {
    switch (kind) {
    case StreamingTaskKind::Metadata: return "metadata";
    case StreamingTaskKind::IoRead: return "io_read";
    case StreamingTaskKind::Decode: return "decode";
    case StreamingTaskKind::MainThreadApply: return "main_thread_apply";
    case StreamingTaskKind::Upload: return "upload";
    case StreamingTaskKind::BlasBuild: return "blas_build";
    case StreamingTaskKind::TlasPatch: return "tlas_patch";
    case StreamingTaskKind::DescriptorUpdate: return "descriptor_update";
    }
    return "io_read";
}

const char* streamingTaskStateName(StreamingTaskState state) {
    switch (state) {
    case StreamingTaskState::Queued: return "queued";
    case StreamingTaskState::Running: return "running";
    case StreamingTaskState::Complete: return "complete";
    case StreamingTaskState::Cancelled: return "cancelled";
    case StreamingTaskState::Failed: return "failed";
    case StreamingTaskState::WaitingForMemory: return "waiting_for_memory";
    }
    return "queued";
}

uint64_t StreamingScheduler::enqueue(StreamingTaskDesc desc) {
    if (desc.label.empty()) {
        desc.label = streamingTaskKindName(desc.kind);
    }
    Task task;
    task.id = nextId_++;
    task.remainingCpuMs = std::max(0.0, desc.cpuCostMs);
    task.desc = std::move(desc);
    tasks_.push_back(std::move(task));
    return tasks_.back().id;
}

StreamingScheduler::Task* StreamingScheduler::find(uint64_t id) {
    for (Task& task : tasks_) {
        if (task.id == id) {
            return &task;
        }
    }
    return nullptr;
}

const StreamingScheduler::Task* StreamingScheduler::find(uint64_t id) const {
    for (const Task& task : tasks_) {
        if (task.id == id) {
            return &task;
        }
    }
    return nullptr;
}

bool StreamingScheduler::dependenciesComplete(const Task& task, bool& anyDependencyTerminalFailed) const {
    anyDependencyTerminalFailed = false;
    for (uint64_t depId : task.desc.dependencies) {
        const Task* dep = find(depId);
        if (dep == nullptr) {
            // Unknown dependency id: treat as already satisfied so stale ids never deadlock the queue.
            continue;
        }
        if (dep->state == StreamingTaskState::Cancelled || dep->state == StreamingTaskState::Failed) {
            anyDependencyTerminalFailed = true;
            return false;
        }
        if (dep->state != StreamingTaskState::Complete) {
            return false;
        }
    }
    return true;
}

void StreamingScheduler::releaseReservation(Task& task) {
    if (task.memoryReserved) {
        liveReservedMemoryBytes_ -= std::min(liveReservedMemoryBytes_, task.desc.transientMemoryBytes);
        task.memoryReserved = false;
    }
}

bool StreamingScheduler::cancel(uint64_t id) {
    Task* task = find(id);
    if (task != nullptr && !terminal(task->state)) {
        releaseReservation(*task);
        task->state = StreamingTaskState::Cancelled;
        return true;
    }
    return false;
}

uint32_t StreamingScheduler::cancelGuid(const AssetGuid& guid) {
    uint32_t cancelled = 0;
    for (Task& task : tasks_) {
        if (task.desc.guid == guid && !terminal(task.state)) {
            releaseReservation(task);
            task.state = StreamingTaskState::Cancelled;
            ++cancelled;
        }
    }
    return cancelled;
}

StreamingSchedulerFrameResult StreamingScheduler::stepFrame(const StreamingSchedulerBudget& budget) {
    StreamingSchedulerFrameResult result;
    result.frameIndex = frameIndex_++;

    // Transitively cancel tasks whose dependencies failed, and age the waiting queue.
    for (Task& task : tasks_) {
        if (task.state == StreamingTaskState::Queued || task.state == StreamingTaskState::WaitingForMemory) {
            bool depFailed = false;
            if (!dependenciesComplete(task, depFailed) && depFailed) {
                task.state = StreamingTaskState::Cancelled;
                ++result.cancelledTasks;
                continue;
            }
            ++task.queuedFrames;
            task.state = StreamingTaskState::Queued;
        }
    }

    // Continued (in-progress) tasks keep their memory reservation and are prioritized to finish first
    // so a long decoder cannot be starved by newly arriving work. Then queued, dependency-ready tasks.
    std::vector<Task*> runnable;
    for (Task& task : tasks_) {
        if (task.state == StreamingTaskState::Running) {
            runnable.push_back(&task);
            continue;
        }
        if (task.state == StreamingTaskState::Queued) {
            bool depFailed = false;
            if (dependenciesComplete(task, depFailed)) {
                runnable.push_back(&task);
            } else {
                ++result.blockedByDependencyTasks;
            }
        }
    }
    std::sort(runnable.begin(), runnable.end(), [](const Task* a, const Task* b) {
        if (a->started != b->started) {
            return a->started; // finish in-progress continuations first
        }
        const int pa = effectivePriority(a->desc, a->queuedFrames);
        const int pb = effectivePriority(b->desc, b->queuedFrames);
        if (pa != pb) {
            return pa > pb;
        }
        return a->id < b->id;
    });

    uint32_t tasksRun = 0;
    for (Task* task : runnable) {
        if (budget.maxTasksPerFrame != 0 && tasksRun >= budget.maxTasksPerFrame) {
            result.taskBudgetExhausted = true;
            break;
        }

        const double cpuRemainingInFrame = budget.maxCpuMs - result.consumedCpuMs;
        if (cpuRemainingInFrame <= 0.0 && task->remainingCpuMs > 0.0) {
            result.cpuBudgetExhausted = true;
            break;
        }

        // I/O and upload byte budgets apply once, when the task first runs.
        if (!task->started) {
            if (result.consumedIoBytes + task->desc.ioBytes > budget.maxIoBytes) {
                result.ioBudgetExhausted = true;
                break;
            }
            if (result.consumedUploadBytes + task->desc.uploadBytes > budget.maxUploadBytes) {
                result.uploadBudgetExhausted = true;
                break;
            }
            // Reserve transient memory for the lifetime of the task (held across continuations).
            if (task->desc.transientMemoryBytes > 0) {
                if (task->desc.transientMemoryBytes > budget.maxTransientMemoryBytes) {
                    task->state = StreamingTaskState::WaitingForMemory;
                    result.memoryBudgetExhausted = true;
                    continue;
                }
                if (liveReservedMemoryBytes_ + task->desc.transientMemoryBytes > budget.maxTransientMemoryBytes) {
                    task->state = StreamingTaskState::WaitingForMemory;
                    result.memoryBudgetExhausted = true;
                    continue;
                }
                liveReservedMemoryBytes_ += task->desc.transientMemoryBytes;
                task->memoryReserved = true;
            }
            task->started = true;
            result.consumedIoBytes += task->desc.ioBytes;
            result.consumedUploadBytes += task->desc.uploadBytes;
        }

        // Run as much CPU work as the frame budget allows; split the rest into a continuation.
        const double work = std::min(task->remainingCpuMs, cpuRemainingInFrame);
        task->remainingCpuMs -= work;
        result.consumedCpuMs += work;
        result.peakTransientMemoryBytes = std::max(result.peakTransientMemoryBytes, task->desc.transientMemoryBytes);
        ++tasksRun;

        if (task->remainingCpuMs > 1e-9) {
            // Long-running task: yield and continue next frame, keeping its memory reservation.
            ++task->continuationCount;
            task->state = StreamingTaskState::Running;
            ++result.continuedTasks;
            result.cpuBudgetExhausted = true;
            break;
        }

        releaseReservation(*task);
        task->state = StreamingTaskState::Complete;
        ++result.completedTasks;
    }

    result.reservedTransientMemoryBytes = liveReservedMemoryBytes_;
    return result;
}

std::vector<StreamingTaskSnapshot> StreamingScheduler::snapshots(bool includeTerminal) const {
    std::vector<StreamingTaskSnapshot> out;
    for (const Task& task : tasks_) {
        if (!includeTerminal && terminal(task.state)) {
            continue;
        }
        uint32_t unmet = 0;
        for (uint64_t depId : task.desc.dependencies) {
            const Task* dep = find(depId);
            if (dep != nullptr && dep->state != StreamingTaskState::Complete) {
                ++unmet;
            }
        }
        out.push_back(StreamingTaskSnapshot{
            .id = task.id,
            .kind = task.desc.kind,
            .state = task.state,
            .guid = task.desc.guid,
            .label = task.desc.label,
            .basePriority = task.desc.priority,
            .effectivePriority = effectivePriority(task.desc, task.queuedFrames),
            .queuedFrames = task.queuedFrames,
            .ioBytes = task.desc.ioBytes,
            .uploadBytes = task.desc.uploadBytes,
            .transientMemoryBytes = task.desc.transientMemoryBytes,
            .cpuCostMs = task.desc.cpuCostMs,
            .dependencyCount = static_cast<uint32_t>(task.desc.dependencies.size()),
            .unmetDependencyCount = unmet,
            .continuationCount = task.continuationCount,
            .remainingCpuMs = task.remainingCpuMs,
            .reservedTransientMemoryBytes = task.memoryReserved ? task.desc.transientMemoryBytes : 0ull,
        });
    }
    return out;
}

bool StreamingScheduler::empty() const {
    return std::all_of(tasks_.begin(), tasks_.end(), [](const Task& task) {
        return terminal(task.state);
    });
}

nlohmann::json streamingSchedulerSnapshotJson(const std::vector<StreamingTaskSnapshot>& snapshots) {
    nlohmann::json out = nlohmann::json::array();
    for (const StreamingTaskSnapshot& snapshot : snapshots) {
        out.push_back({
            {"id", snapshot.id},
            {"kind", streamingTaskKindName(snapshot.kind)},
            {"state", streamingTaskStateName(snapshot.state)},
            {"guid", snapshot.guid},
            {"label", snapshot.label},
            {"base_priority", snapshot.basePriority},
            {"effective_priority", snapshot.effectivePriority},
            {"queued_frames", snapshot.queuedFrames},
            {"io_bytes", snapshot.ioBytes},
            {"upload_bytes", snapshot.uploadBytes},
            {"transient_memory_bytes", snapshot.transientMemoryBytes},
            {"cpu_cost_ms", snapshot.cpuCostMs},
            {"dependency_count", snapshot.dependencyCount},
            {"unmet_dependency_count", snapshot.unmetDependencyCount},
            {"continuation_count", snapshot.continuationCount},
            {"remaining_cpu_ms", snapshot.remainingCpuMs},
            {"reserved_transient_memory_bytes", snapshot.reservedTransientMemoryBytes},
        });
    }
    return out;
}

int simulateStreamingSchedulerCommand(
    uint32_t taskCount,
    const StreamingSchedulerBudget& budget,
    uint32_t cancelAfterFrame,
    const std::filesystem::path& jsonOut) {
    StreamingScheduler scheduler;
    // Per-GUID streaming pipeline dependency chain: metadata -> io_read -> decode -> upload -> blas/descriptor.
    // This models the real streaming flow so the simulation exercises dependency edges, and the per-stage
    // ordering proves a decode task never runs before its I/O read completes.
    std::vector<uint64_t> lastIdForGuid(5u, 0u);
    for (uint32_t i = 0; i < taskCount; ++i) {
        StreamingTaskDesc desc;
        const uint32_t guidSlot = i % 5u;
        desc.guid = "streaming-task-" + std::to_string(guidSlot);
        desc.kind = static_cast<StreamingTaskKind>(i % 8u);
        desc.label = std::string("scheduler task ") + std::to_string(i);
        desc.priority = static_cast<int>((i * 7u) % 31u);
        desc.ioBytes = (desc.kind == StreamingTaskKind::IoRead) ? (4ull * 1024ull * 1024ull) : 0ull;
        desc.uploadBytes = (desc.kind == StreamingTaskKind::Upload) ? (2ull * 1024ull * 1024ull) : 0ull;
        desc.transientMemoryBytes = (1ull + (i % 4u)) * 1024ull * 1024ull;
        // Decode tasks are deliberately expensive so they exceed a single frame's CPU budget and split
        // into continuation work across frames while holding their transient-memory reservation.
        desc.cpuCostMs = (desc.kind == StreamingTaskKind::Decode)
            ? (budget.maxCpuMs * 2.5 + 0.5)
            : 0.1 + static_cast<double>(i % 5u) * 0.15;
        desc.selectedBoost = i == 0;
        desc.visibleBoost = (i % 3u) == 0;
        // Chain each task after the previous task for the same synthetic GUID.
        if (lastIdForGuid[guidSlot] != 0u) {
            desc.dependencies.push_back(lastIdForGuid[guidSlot]);
        }
        const uint64_t id = scheduler.enqueue(std::move(desc));
        lastIdForGuid[guidSlot] = id;
    }

    nlohmann::json frames = nlohmann::json::array();
    uint32_t totalContinued = 0;
    uint32_t totalBlockedByDependency = 0;
    uint64_t peakReservedMemoryBytes = 0;
    constexpr uint32_t kMaxFrames = 512;
    for (uint32_t frame = 0; frame < kMaxFrames && !scheduler.empty(); ++frame) {
        if (cancelAfterFrame != 0 && frame == cancelAfterFrame) {
            (void)scheduler.cancelGuid("streaming-task-3");
        }
        StreamingSchedulerFrameResult result = scheduler.stepFrame(budget);
        totalContinued += result.continuedTasks;
        totalBlockedByDependency += result.blockedByDependencyTasks;
        peakReservedMemoryBytes = std::max(peakReservedMemoryBytes, result.reservedTransientMemoryBytes);
        frames.push_back(frameResultJson(result));
    }

    // Per-category utilization (completed/cancelled tallies per task kind) for the Job Center.
    const std::vector<StreamingTaskSnapshot> finalSnapshots = scheduler.snapshots(true);
    std::array<uint32_t, 8> completedByKind{};
    std::array<uint32_t, 8> cancelledByKind{};
    uint32_t maxContinuationCount = 0;
    for (const StreamingTaskSnapshot& snapshot : finalSnapshots) {
        const size_t kindIndex = static_cast<size_t>(snapshot.kind);
        if (snapshot.state == StreamingTaskState::Complete && kindIndex < completedByKind.size()) {
            ++completedByKind[kindIndex];
        } else if (snapshot.state == StreamingTaskState::Cancelled && kindIndex < cancelledByKind.size()) {
            ++cancelledByKind[kindIndex];
        }
        maxContinuationCount = std::max(maxContinuationCount, snapshot.continuationCount);
    }
    nlohmann::json perCategory = nlohmann::json::array();
    for (uint32_t k = 0; k < 8u; ++k) {
        perCategory.push_back({
            {"kind", streamingTaskKindName(static_cast<StreamingTaskKind>(k))},
            {"completed", completedByKind[k]},
            {"cancelled", cancelledByKind[k]},
        });
    }

    const nlohmann::json report = {
        {"schema", "StreamingSchedulerSimulationV1"},
        {"ok", scheduler.empty()},
        {"task_count", taskCount},
        {"budget", {
            {"max_tasks_per_frame", budget.maxTasksPerFrame},
            {"max_cpu_ms", budget.maxCpuMs},
            {"max_io_bytes", budget.maxIoBytes},
            {"max_upload_bytes", budget.maxUploadBytes},
            {"max_transient_memory_bytes", budget.maxTransientMemoryBytes},
        }},
        {"totals", {
            {"frames_simulated", frames.size()},
            {"continued_task_runs", totalContinued},
            {"blocked_by_dependency_runs", totalBlockedByDependency},
            {"max_task_continuation_count", maxContinuationCount},
            {"peak_reserved_transient_memory_bytes", peakReservedMemoryBytes},
        }},
        {"per_category_utilization", perCategory},
        {"frames", frames},
        {"final_tasks", streamingSchedulerSnapshotJson(finalSnapshots)},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Could not create streaming scheduler report directory: " << parent.string() << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Could not write streaming scheduler report: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return report.value("ok", false) ? 0 : 1;
}

} // namespace rtv
