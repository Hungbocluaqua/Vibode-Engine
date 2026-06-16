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
};

struct StreamingSchedulerFrameResult {
    uint32_t frameIndex = 0;
    uint32_t completedTasks = 0;
    uint32_t cancelledTasks = 0;
    uint64_t consumedIoBytes = 0;
    uint64_t consumedUploadBytes = 0;
    uint64_t peakTransientMemoryBytes = 0;
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
    };

    uint64_t nextId_ = 1;
    uint32_t frameIndex_ = 0;
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
