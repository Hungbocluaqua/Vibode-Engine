#pragma once

#include "rtv/AssetRegistry.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

enum class StreamingTaskKind : uint8_t {
    Metadata,
    IoRead,
    Decode,
    MainThreadApply,
    Upload,
    BlasBuild,
    TlasPatch,
    DescriptorUpdate,
};

enum class StreamingTaskState : uint8_t {
    Queued,
    Running,
    Complete,
    Cancelled,
    Failed,
    WaitingForMemory,
};

struct StreamingSchedulerBudget {
    uint32_t maxTasksPerFrame = 8;
    double maxCpuMs = 2.0;
    uint64_t maxIoBytes = 64ull * 1024ull * 1024ull;
    uint64_t maxUploadBytes = 64ull * 1024ull * 1024ull;
    uint64_t maxTransientMemoryBytes = 256ull * 1024ull * 1024ull;
};

struct StreamingTaskDesc {
    StreamingTaskKind kind = StreamingTaskKind::IoRead;
    AssetGuid guid;
    std::string label;
    int priority = 0;
    uint64_t ioBytes = 0;
    uint64_t uploadBytes = 0;
    uint64_t transientMemoryBytes = 0;
    double cpuCostMs = 0.0;
    bool selectedBoost = false;
    bool visibleBoost = false;
    // Dependency task ids. A task is not runnable until every dependency reaches Complete.
    // If any dependency is Cancelled or Failed, this task is cancelled/failed transitively.
    std::vector<uint64_t> dependencies;
};

struct StreamingTaskSnapshot {
    uint64_t id = 0;
    StreamingTaskKind kind = StreamingTaskKind::IoRead;
    StreamingTaskState state = StreamingTaskState::Queued;
    AssetGuid guid;
    std::string label;
    int basePriority = 0;
    int effectivePriority = 0;
    uint32_t queuedFrames = 0;
    uint64_t ioBytes = 0;
    uint64_t uploadBytes = 0;
    uint64_t transientMemoryBytes = 0;
    double cpuCostMs = 0.0;
    uint32_t dependencyCount = 0;
    uint32_t unmetDependencyCount = 0;
    uint32_t continuationCount = 0;       // Number of frames this task was split across.
    double remainingCpuMs = 0.0;          // CPU work still pending for in-progress continuations.
    uint64_t reservedTransientMemoryBytes = 0; // Reservation held across frames while running.
};

struct StreamingSchedulerFrameResult {
    uint32_t frameIndex = 0;
    uint32_t completedTasks = 0;
    uint32_t cancelledTasks = 0;
    uint32_t continuedTasks = 0;          // Tasks that ran but split a continuation into a later frame.
    uint32_t blockedByDependencyTasks = 0; // Runnable-but-for-unmet-dependencies this frame.
    uint64_t consumedIoBytes = 0;
    uint64_t consumedUploadBytes = 0;
    uint64_t peakTransientMemoryBytes = 0;
    uint64_t reservedTransientMemoryBytes = 0; // Live reservation at end of frame.
    double consumedCpuMs = 0.0;
    bool taskBudgetExhausted = false;
    bool cpuBudgetExhausted = false;
    bool ioBudgetExhausted = false;
    bool uploadBudgetExhausted = false;
    bool memoryBudgetExhausted = false;
};

class StreamingScheduler {
public:
    [[nodiscard]] uint64_t enqueue(StreamingTaskDesc desc);
    [[nodiscard]] bool cancel(uint64_t id);
    [[nodiscard]] uint32_t cancelGuid(const AssetGuid& guid);
    [[nodiscard]] StreamingSchedulerFrameResult stepFrame(const StreamingSchedulerBudget& budget);
    [[nodiscard]] std::vector<StreamingTaskSnapshot> snapshots(bool includeTerminal = true) const;
    [[nodiscard]] bool empty() const;

private:
    struct Task {
        uint64_t id = 0;
        StreamingTaskDesc desc{};
        StreamingTaskState state = StreamingTaskState::Queued;
        uint32_t queuedFrames = 0;
        double remainingCpuMs = 0.0;            // Decremented as continuations run; <=0 means done.
        uint32_t continuationCount = 0;         // Frames this task has been split across.
        bool started = false;                   // True once first continuation has run.
        bool memoryReserved = false;            // True while this task holds a transient reservation.
    };

    [[nodiscard]] Task* find(uint64_t id);
    [[nodiscard]] const Task* find(uint64_t id) const;
    [[nodiscard]] bool dependenciesComplete(const Task& task, bool& anyDependencyTerminalFailed) const;
    void releaseReservation(Task& task);

    uint64_t nextId_ = 1;
    uint32_t frameIndex_ = 0;
    uint64_t liveReservedMemoryBytes_ = 0;      // Memory held by in-progress (continued) tasks.
    std::vector<Task> tasks_;
};

[[nodiscard]] const char* streamingTaskKindName(StreamingTaskKind kind);
[[nodiscard]] const char* streamingTaskStateName(StreamingTaskState state);
[[nodiscard]] nlohmann::json streamingSchedulerSnapshotJson(const std::vector<StreamingTaskSnapshot>& snapshots);
[[nodiscard]] int simulateStreamingSchedulerCommand(
    uint32_t taskCount,
    const StreamingSchedulerBudget& budget,
    uint32_t cancelAfterFrame,
    const std::filesystem::path& jsonOut = {});

} // namespace rtv
