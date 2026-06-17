#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

enum class StreamingGpuWorkKind : uint8_t {
    BufferUpload,
    ImageMipUpload,
    BlasBuild,
    BlasCompaction,
    TlasPatch,
    DescriptorUpdate,
};

enum class StreamingGpuWorkState : uint8_t {
    Queued,
    Submitted,
    WaitingForTimeline,
    Complete,
    Cancelled,
    Failed,
};

struct StreamingGpuWorkDesc {
    StreamingGpuWorkKind kind = StreamingGpuWorkKind::BufferUpload;
    std::string label;
    std::string ownerGuid;
    uint64_t bytes = 0;
    double estimatedGpuMs = 0.0;
    uint32_t textureMipLevel = UINT32_MAX;
    uint32_t textureMipCount = 0;
    uint32_t blasBuilds = 0;
    uint32_t tlasPatches = 0;
    uint32_t descriptorUpdates = 0;
    bool requiresGraphicsQueue = true;
    bool payloadBacked = false;
};

struct StreamingGpuWorkBudget {
    uint64_t maxUploadBytes = 64ull * 1024ull * 1024ull;
    uint64_t maxStagingBytes = 128ull * 1024ull * 1024ull;
    double maxGpuMs = 2.0;
    uint32_t maxSubmissions = 8;
    uint32_t maxBlasBuilds = 2;
    uint32_t maxTlasPatches = 4;
    uint32_t maxDescriptorUpdates = 256;
};

struct StreamingGpuWorkFrameResult {
    uint32_t frameIndex = 0;
    uint32_t submittedTickets = 0;
    uint32_t completedTickets = 0;
    uint64_t submittedBytes = 0;
    double submittedGpuMs = 0.0;
    uint32_t submittedBlasBuilds = 0;
    uint32_t submittedTlasPatches = 0;
    uint32_t submittedDescriptorUpdates = 0;
    uint64_t highestSubmittedTimeline = 0;
    uint64_t submittedStagingBytes = 0;
    uint64_t retainedStagingBytes = 0;
    bool uploadBudgetExhausted = false;
    bool stagingBudgetExhausted = false;
    bool gpuBudgetExhausted = false;
    bool submissionBudgetExhausted = false;
    bool blasBudgetExhausted = false;
    bool tlasBudgetExhausted = false;
    bool descriptorBudgetExhausted = false;
};

struct StreamingGpuWorkSnapshot {
    uint64_t id = 0;
    StreamingGpuWorkKind kind = StreamingGpuWorkKind::BufferUpload;
    StreamingGpuWorkState state = StreamingGpuWorkState::Queued;
    std::string label;
    std::string ownerGuid;
    uint64_t bytes = 0;
    double estimatedGpuMs = 0.0;
    uint32_t textureMipLevel = UINT32_MAX;
    uint32_t textureMipCount = 0;
    uint32_t blasBuilds = 0;
    uint32_t tlasPatches = 0;
    uint32_t descriptorUpdates = 0;
    uint64_t submittedTimeline = 0;
    uint64_t retainedStagingBytes = 0;
    bool requiresGraphicsQueue = true;
    bool payloadBacked = false;
    bool canCancel = false;
    bool canRetire = false;
};

struct StreamingGpuWorkQueueStats {
    uint32_t queued = 0;
    uint32_t submitted = 0;
    uint32_t waitingForTimeline = 0;
    uint32_t complete = 0;
    uint32_t cancelled = 0;
    uint32_t failed = 0;
    uint64_t queuedBytes = 0;
    uint64_t submittedBytes = 0;
    uint64_t completedBytes = 0;
    uint64_t retainedStagingBytes = 0;
    uint64_t peakRetainedStagingBytes = 0;
};

struct StreamingGpuWorkPressureStats {
    uint32_t frames = 0;
    uint32_t idleFrames = 0;
    uint32_t stalledFrames = 0;
    uint32_t uploadBudgetFrames = 0;
    uint32_t stagingBudgetFrames = 0;
    uint32_t gpuBudgetFrames = 0;
    uint32_t submissionBudgetFrames = 0;
    uint32_t blasBudgetFrames = 0;
    uint32_t tlasBudgetFrames = 0;
    uint32_t descriptorBudgetFrames = 0;
    uint64_t peakRetainedStagingBytes = 0;
    StreamingGpuWorkFrameResult lastFrame{};
};

class StreamingGpuWorkQueue {
public:
    [[nodiscard]] uint64_t enqueue(StreamingGpuWorkDesc desc);
    [[nodiscard]] StreamingGpuWorkFrameResult submitFrame(const StreamingGpuWorkBudget& budget);
    [[nodiscard]] bool completeTimeline(uint64_t completedTimeline);
    [[nodiscard]] bool cancel(uint64_t id);
    [[nodiscard]] bool empty() const;
    [[nodiscard]] uint64_t nextTimelineValue() const { return nextTimelineValue_; }
    [[nodiscard]] std::vector<StreamingGpuWorkSnapshot> snapshots() const;
    [[nodiscard]] StreamingGpuWorkQueueStats stats() const;
    [[nodiscard]] StreamingGpuWorkPressureStats pressureStats() const;

private:
    struct Ticket {
        uint64_t id = 0;
        StreamingGpuWorkDesc desc{};
        StreamingGpuWorkState state = StreamingGpuWorkState::Queued;
        uint64_t submittedTimeline = 0;
        uint64_t retainedStagingBytes = 0;
    };

    [[nodiscard]] bool canSubmit(const Ticket& ticket, const StreamingGpuWorkBudget& budget, const StreamingGpuWorkFrameResult& frame, StreamingGpuWorkFrameResult& exhausted) const;
    [[nodiscard]] bool usesStaging(const Ticket& ticket) const;
    [[nodiscard]] uint64_t retainedStagingBytes() const;
    void recordPressure(const StreamingGpuWorkFrameResult& frame);
    [[nodiscard]] bool terminal(StreamingGpuWorkState state) const;

    uint64_t nextTicketId_ = 1;
    uint64_t nextTimelineValue_ = 1;
    uint32_t frameIndex_ = 0;
    uint64_t peakRetainedStagingBytes_ = 0;
    StreamingGpuWorkPressureStats pressure_{};
    std::vector<Ticket> tickets_;
};

[[nodiscard]] const char* streamingGpuWorkKindName(StreamingGpuWorkKind kind);
[[nodiscard]] const char* streamingGpuWorkStateName(StreamingGpuWorkState state);
[[nodiscard]] nlohmann::json streamingGpuWorkQueueStatsJson(const StreamingGpuWorkQueueStats& stats);
[[nodiscard]] nlohmann::json streamingGpuWorkFrameResultJson(const StreamingGpuWorkFrameResult& frame);
[[nodiscard]] nlohmann::json streamingGpuWorkPressureStatsJson(const StreamingGpuWorkPressureStats& stats);
[[nodiscard]] nlohmann::json streamingGpuWorkQueueSnapshotsJson(const std::vector<StreamingGpuWorkSnapshot>& snapshots);
[[nodiscard]] int simulateStreamingGpuWorkQueueCommand(
    uint32_t ticketCount,
    const StreamingGpuWorkBudget& budget,
    uint32_t completeLagFrames,
    const std::filesystem::path& jsonOut = {});

} // namespace rtv
