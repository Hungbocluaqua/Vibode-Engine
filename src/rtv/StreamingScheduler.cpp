#include "rtv/StreamingScheduler.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
        {"consumed_io_bytes", frame.consumedIoBytes},
        {"consumed_upload_bytes", frame.consumedUploadBytes},
        {"peak_transient_memory_bytes", frame.peakTransientMemoryBytes},
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
    task.desc = std::move(desc);
    tasks_.push_back(std::move(task));
    return tasks_.back().id;
}

bool StreamingScheduler::cancel(uint64_t id) {
    for (Task& task : tasks_) {
        if (task.id == id && !terminal(task.state)) {
            task.state = StreamingTaskState::Cancelled;
            return true;
        }
    }
    return false;
}

uint32_t StreamingScheduler::cancelGuid(const AssetGuid& guid) {
    uint32_t cancelled = 0;
    for (Task& task : tasks_) {
        if (task.desc.guid == guid && !terminal(task.state)) {
            task.state = StreamingTaskState::Cancelled;
            ++cancelled;
        }
    }
    return cancelled;
}

StreamingSchedulerFrameResult StreamingScheduler::stepFrame(const StreamingSchedulerBudget& budget) {
    StreamingSchedulerFrameResult result;
    result.frameIndex = frameIndex_++;

    for (Task& task : tasks_) {
        if (task.state == StreamingTaskState::Queued || task.state == StreamingTaskState::WaitingForMemory) {
            ++task.queuedFrames;
            task.state = StreamingTaskState::Queued;
        } else if (task.state == StreamingTaskState::Cancelled) {
            ++result.cancelledTasks;
        }
    }

    std::vector<Task*> runnable;
    for (Task& task : tasks_) {
        if (task.state == StreamingTaskState::Queued) {
            runnable.push_back(&task);
        }
    }
    std::sort(runnable.begin(), runnable.end(), [](const Task* a, const Task* b) {
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
        if (result.consumedCpuMs + task->desc.cpuCostMs > budget.maxCpuMs) {
            result.cpuBudgetExhausted = true;
            break;
        }
        if (result.consumedIoBytes + task->desc.ioBytes > budget.maxIoBytes) {
            result.ioBudgetExhausted = true;
            break;
        }
        if (result.consumedUploadBytes + task->desc.uploadBytes > budget.maxUploadBytes) {
            result.uploadBudgetExhausted = true;
            break;
        }
        if (task->desc.transientMemoryBytes > budget.maxTransientMemoryBytes) {
            task->state = StreamingTaskState::WaitingForMemory;
            result.memoryBudgetExhausted = true;
            continue;
        }

        task->state = StreamingTaskState::Running;
        result.consumedCpuMs += task->desc.cpuCostMs;
        result.consumedIoBytes += task->desc.ioBytes;
        result.consumedUploadBytes += task->desc.uploadBytes;
        result.peakTransientMemoryBytes = std::max(result.peakTransientMemoryBytes, task->desc.transientMemoryBytes);
        task->state = StreamingTaskState::Complete;
        ++result.completedTasks;
        ++tasksRun;
    }

    return result;
}

std::vector<StreamingTaskSnapshot> StreamingScheduler::snapshots(bool includeTerminal) const {
    std::vector<StreamingTaskSnapshot> out;
    for (const Task& task : tasks_) {
        if (!includeTerminal && terminal(task.state)) {
            continue;
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
    for (uint32_t i = 0; i < taskCount; ++i) {
        StreamingTaskDesc desc;
        desc.guid = "streaming-task-" + std::to_string(i % 5u);
        desc.kind = static_cast<StreamingTaskKind>(i % 8u);
        desc.label = std::string("scheduler task ") + std::to_string(i);
        desc.priority = static_cast<int>((i * 7u) % 31u);
        desc.ioBytes = (desc.kind == StreamingTaskKind::IoRead) ? (4ull * 1024ull * 1024ull) : 0ull;
        desc.uploadBytes = (desc.kind == StreamingTaskKind::Upload) ? (2ull * 1024ull * 1024ull) : 0ull;
        desc.transientMemoryBytes = (1ull + (i % 4u)) * 1024ull * 1024ull;
        desc.cpuCostMs = 0.1 + static_cast<double>(i % 5u) * 0.15;
        desc.selectedBoost = i == 0;
        desc.visibleBoost = (i % 3u) == 0;
        (void)scheduler.enqueue(std::move(desc));
    }

    nlohmann::json frames = nlohmann::json::array();
    constexpr uint32_t kMaxFrames = 512;
    for (uint32_t frame = 0; frame < kMaxFrames && !scheduler.empty(); ++frame) {
        if (cancelAfterFrame != 0 && frame == cancelAfterFrame) {
            (void)scheduler.cancelGuid("streaming-task-3");
        }
        StreamingSchedulerFrameResult result = scheduler.stepFrame(budget);
        frames.push_back(frameResultJson(result));
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
        {"frames", frames},
        {"final_tasks", streamingSchedulerSnapshotJson(scheduler.snapshots(true))},
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
