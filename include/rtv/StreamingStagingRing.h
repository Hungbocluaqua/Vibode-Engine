#pragma once

#include "rtv/Buffer.h"
#include "rtv/NonCopyable.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <deque>
#include <optional>

namespace rtv {

class ResourceAllocator;

// Bounded ring allocator for streaming upload staging memory.
//
// The ring hands out byte sub-ranges of a single persistently-mapped upload
// buffer. Each allocation is tagged with the timeline value of the GPU
// submission that consumes it; the range is only reclaimed once that timeline
// value has completed on the device. This keeps streaming upload memory
// strictly bounded (allocations fail rather than grow past capacity) and makes
// staging reuse fence-safe.
//
// When constructed without a ResourceAllocator (CPU mode) the ring performs the
// same offset/recycle bookkeeping without owning a VkBuffer, so the allocation
// math can be exercised by offline simulation and unit tests.
struct StreamingStagingAllocation {
    uint64_t offset = 0;
    uint64_t size = 0;
    void* mapped = nullptr;       // null in CPU mode
    VkBuffer buffer = VK_NULL_HANDLE; // null in CPU mode
    bool valid = false;
};

struct StreamingStagingRingStats {
    uint64_t capacityBytes = 0;
    uint64_t inFlightBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t peakInFlightBytes = 0;
    uint64_t totalAllocatedBytes = 0;
    uint64_t totalReclaimedBytes = 0;
    uint32_t liveAllocationCount = 0;
    uint32_t allocationFailureCount = 0;
    uint64_t highestRetiredTimeline = 0;
};

class StreamingStagingRing final : private NonCopyable {
public:
    StreamingStagingRing() = default;
    // device-backed ring.
    StreamingStagingRing(ResourceAllocator& allocator, uint64_t capacityBytes, uint32_t alignment = 256u);
    // CPU-only ring (no VkBuffer); offset/recycle math only.
    explicit StreamingStagingRing(uint64_t capacityBytes, uint32_t alignment = 256u);
    ~StreamingStagingRing();

    StreamingStagingRing(StreamingStagingRing&& other) noexcept;
    StreamingStagingRing& operator=(StreamingStagingRing&& other) noexcept;

    // Reserve `bytes` for an upload that will be consumed by submission
    // `timelineValue`. Returns nullopt when the ring is full (bounded). The
    // returned range must be released via retire(completedTimeline) once that
    // timeline value has signaled.
    [[nodiscard]] std::optional<StreamingStagingAllocation> allocate(uint64_t bytes, uint64_t timelineValue);

    // Reclaim every allocation whose timeline value <= completedTimeline.
    // Returns the number of bytes reclaimed.
    uint64_t retire(uint64_t completedTimeline);

    [[nodiscard]] bool deviceBacked() const { return buffer_.handle() != VK_NULL_HANDLE; }
    [[nodiscard]] uint64_t capacityBytes() const { return capacityBytes_; }
    [[nodiscard]] StreamingStagingRingStats stats() const;

private:
    struct Pending {
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t timelineValue = 0;
    };

    [[nodiscard]] uint64_t alignUp(uint64_t value) const;
    [[nodiscard]] uint64_t inFlightBytes() const;

    Buffer buffer_;
    void* mapped_ = nullptr;
    uint64_t capacityBytes_ = 0;
    uint32_t alignment_ = 256u;
    // Ring head/tail in a monotonically increasing byte space; modulo capacity
    // gives the physical offset. Using a monotonic space avoids head/tail
    // wrap-around ambiguity.
    uint64_t head_ = 0; // next free byte (monotonic)
    uint64_t tail_ = 0; // oldest still-in-flight byte (monotonic)
    std::deque<Pending> pending_;
    uint64_t peakInFlightBytes_ = 0;
    uint64_t totalAllocatedBytes_ = 0;
    uint64_t totalReclaimedBytes_ = 0;
    uint32_t allocationFailureCount_ = 0;
    uint64_t highestRetiredTimeline_ = 0;
};

[[nodiscard]] nlohmann::json streamingStagingRingStatsJson(const StreamingStagingRingStats& stats);

} // namespace rtv
