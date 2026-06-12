#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

enum class GpuUploadResourceKind : uint8_t {
    Buffer,
    Image,
};

enum class GpuUploadChunkState : uint8_t {
    Pending,
    Submitted,
    Complete,
    Cancelled,
};

enum class GpuUploadTicketState : uint8_t {
    Queued,
    Submitting,
    WaitingForFence,
    Cancelled,
    Complete,
    Failed,
};

struct GpuUploadTicketDesc {
    GpuUploadResourceKind kind = GpuUploadResourceKind::Buffer;
    std::string label;
    uint64_t totalBytes = 0;
    uint64_t chunkBytes = 16ull * 1024ull * 1024ull;
};

struct GpuUploadFrameBudget {
    uint64_t maxBytes = 64ull * 1024ull * 1024ull;
    uint32_t maxSubmissions = 0;
};

struct GpuUploadChunkSnapshot {
    size_t index = 0;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    uint64_t timelineValue = 0;
    GpuUploadChunkState state = GpuUploadChunkState::Pending;
    bool stagingRetained = true;
};

struct GpuUploadTicketSnapshot {
    uint64_t id = 0;
    GpuUploadResourceKind kind = GpuUploadResourceKind::Buffer;
    GpuUploadTicketState state = GpuUploadTicketState::Queued;
    std::string label;
    uint64_t totalBytes = 0;
    uint64_t submittedBytes = 0;
    uint64_t completedBytes = 0;
    uint64_t retainedStagingBytes = 0;
    size_t chunkCount = 0;
    size_t pendingChunks = 0;
    size_t submittedChunks = 0;
    size_t completedChunks = 0;
    bool cancellationRequested = false;
    bool canCancel = false;
    bool canRetire = false;
    std::vector<GpuUploadChunkSnapshot> chunks;
};

struct GpuUploadSubmitResult {
    uint64_t submittedBytes = 0;
    size_t submittedChunks = 0;
    bool budgetExhausted = false;
    bool ticketComplete = false;
};

[[nodiscard]] const char* gpuUploadResourceKindName(GpuUploadResourceKind kind);
[[nodiscard]] const char* gpuUploadChunkStateName(GpuUploadChunkState state);
[[nodiscard]] const char* gpuUploadTicketStateName(GpuUploadTicketState state);

class GpuUploadTicket final {
public:
    GpuUploadTicket() = default;
    GpuUploadTicket(uint64_t id, GpuUploadTicketDesc desc);

    [[nodiscard]] uint64_t id() const { return id_; }
    [[nodiscard]] GpuUploadTicketState state() const { return state_; }
    [[nodiscard]] uint64_t totalBytes() const { return totalBytes_; }
    [[nodiscard]] uint64_t submittedBytes() const;
    [[nodiscard]] uint64_t completedBytes() const;
    [[nodiscard]] uint64_t retainedStagingBytes() const;
    [[nodiscard]] bool canRetire() const;
    [[nodiscard]] bool cancellationRequested() const { return cancellationRequested_; }

    [[nodiscard]] GpuUploadSubmitResult submitFrame(const GpuUploadFrameBudget& budget, uint64_t& nextTimelineValue);
    [[nodiscard]] bool requestCancel(std::string reason = {});
    [[nodiscard]] bool completeTimeline(uint64_t completedTimelineValue);
    [[nodiscard]] GpuUploadTicketSnapshot snapshot(bool includeChunks = true) const;

private:
    struct Chunk {
        uint64_t offset = 0;
        uint64_t bytes = 0;
        uint64_t timelineValue = 0;
        GpuUploadChunkState state = GpuUploadChunkState::Pending;
        bool stagingRetained = true;
    };

    void refreshState();
    [[nodiscard]] bool hasSubmittedChunks() const;

    uint64_t id_ = 0;
    GpuUploadResourceKind kind_ = GpuUploadResourceKind::Buffer;
    std::string label_;
    uint64_t totalBytes_ = 0;
    GpuUploadTicketState state_ = GpuUploadTicketState::Queued;
    bool cancellationRequested_ = false;
    std::string cancelReason_;
    std::vector<Chunk> chunks_;
};

class GpuUploadTicketQueue final {
public:
    [[nodiscard]] uint64_t create(GpuUploadTicketDesc desc);
    [[nodiscard]] GpuUploadSubmitResult submitFrame(const GpuUploadFrameBudget& budget);
    [[nodiscard]] bool requestCancel(uint64_t id, std::string reason = {});
    [[nodiscard]] bool completeTimeline(uint64_t completedTimelineValue);
    [[nodiscard]] std::vector<GpuUploadTicketSnapshot> snapshots(bool includeChunks = true) const;
    [[nodiscard]] uint64_t nextTimelineValue() const { return nextTimelineValue_; }

private:
    uint64_t nextTicketId_ = 1;
    uint64_t nextTimelineValue_ = 1;
    std::vector<GpuUploadTicket> tickets_;
};

} // namespace rtv
