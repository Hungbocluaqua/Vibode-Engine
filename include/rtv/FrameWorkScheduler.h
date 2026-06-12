#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rtv {

enum class FrameWorkQueue : uint8_t {
    MainThreadApply,
    CpuCook,
    GpuUploadPrepare,
    GpuUploadSubmit,
    GpuSceneBuild,
    AccelerationStructureBuild,
    RendererSwap,
    Count,
};

enum class FrameWorkJobStatus : uint8_t {
    Queued,
    Running,
    WaitingForFence,
    Cancelled,
    Failed,
    Complete,
};

struct FrameWorkBudgetConfig {
    double mainThreadApplyMs = 2.0;
    double uploadSubmitMs = 4.0;
    uint64_t uploadBytes = 64ull * 1024ull * 1024ull;
    double minAccelerationStructureBuildMs = 1.0;
    double maxAccelerationStructureBuildMs = 8.0;
    double previousAccelerationStructureGpuMs = 2.0;
};

struct FrameWorkResolvedBudgets {
    double mainThreadApplyMs = 0.0;
    double uploadSubmitMs = 0.0;
    uint64_t uploadBytes = 0;
    double accelerationStructureBuildMs = 0.0;
};

struct FrameWorkJobStepResult {
    bool complete = true;
    bool waitingForFence = false;
    float progress = 1.0f;
    std::string failure;
    std::vector<std::string> warnings;
};

struct FrameWorkJobContext {
    uint64_t id = 0;
    FrameWorkQueue queue = FrameWorkQueue::MainThreadApply;
    FrameWorkResolvedBudgets frameBudgets{};
};

using FrameWorkJobCallback = std::function<FrameWorkJobStepResult(FrameWorkJobContext&)>;

struct FrameWorkJobDesc {
    FrameWorkQueue queue = FrameWorkQueue::MainThreadApply;
    std::string title;
    std::string status;
    double estimatedCostMs = 0.0;
    uint64_t estimatedUploadBytes = 0;
    FrameWorkJobCallback callback;
};

struct FrameWorkJobSnapshot {
    uint64_t id = 0;
    FrameWorkQueue queue = FrameWorkQueue::MainThreadApply;
    FrameWorkJobStatus status = FrameWorkJobStatus::Queued;
    std::string queueName;
    std::string title;
    std::string statusText;
    float progress = 0.0f;
    std::vector<std::string> warnings;
    std::string failure;
};

struct FrameWorkQueueSnapshot {
    FrameWorkQueue queue = FrameWorkQueue::MainThreadApply;
    std::string name;
    size_t queued = 0;
    size_t running = 0;
    size_t waitingForFence = 0;
    size_t cancelled = 0;
    size_t failed = 0;
    size_t complete = 0;
    std::vector<FrameWorkJobSnapshot> activeJobs;
};

struct FrameWorkSchedulerSnapshot {
    FrameWorkResolvedBudgets budgets{};
    std::array<FrameWorkQueueSnapshot, static_cast<size_t>(FrameWorkQueue::Count)> queues{};
    std::vector<FrameWorkJobSnapshot> recentJobs;
};

[[nodiscard]] const char* frameWorkQueueName(FrameWorkQueue queue);
[[nodiscard]] const char* frameWorkJobStatusName(FrameWorkJobStatus status);

class FrameWorkScheduler final {
public:
    [[nodiscard]] uint64_t enqueue(FrameWorkJobDesc desc);
    [[nodiscard]] uint64_t enqueueNoOp(FrameWorkQueue queue, std::string title = {});

    void tick();
    [[nodiscard]] bool cancel(uint64_t id, std::string reason = {});
    [[nodiscard]] bool completeFence(uint64_t id);
    void clearFinished();

    [[nodiscard]] bool empty() const;
    [[nodiscard]] bool hasActiveJobs() const;
    [[nodiscard]] size_t liveJobCount() const;
    [[nodiscard]] FrameWorkSchedulerSnapshot snapshot() const;

    [[nodiscard]] const FrameWorkBudgetConfig& budgetConfig() const { return budgets_; }
    void setBudgetConfig(const FrameWorkBudgetConfig& budgets);
    void setPreviousAccelerationStructureGpuMs(double milliseconds);

private:
    struct JobState {
        uint64_t id = 0;
        FrameWorkJobDesc desc{};
        FrameWorkJobStatus status = FrameWorkJobStatus::Queued;
        float progress = 0.0f;
        std::vector<std::string> warnings;
        std::string failure;
    };

    [[nodiscard]] FrameWorkResolvedBudgets resolvedBudgets() const;
    [[nodiscard]] bool canRun(const JobState& job, const FrameWorkResolvedBudgets& budgets, double& usedMainApplyMs, double& usedUploadSubmitMs, uint64_t& usedUploadBytes, double& usedAsBuildMs) const;
    void charge(const JobState& job, double& usedMainApplyMs, double& usedUploadSubmitMs, uint64_t& usedUploadBytes, double& usedAsBuildMs) const;
    void appendRecent(const JobState& job);
    [[nodiscard]] FrameWorkJobSnapshot makeSnapshot(const JobState& job) const;

    FrameWorkBudgetConfig budgets_{};
    uint64_t nextJobId_ = 1;
    std::vector<JobState> jobs_;
    std::vector<FrameWorkJobSnapshot> recentJobs_;
};

} // namespace rtv
