#include "rtv/StreamingStagingRing.h"

#include "rtv/ResourceAllocator.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace rtv {

StreamingStagingRing::StreamingStagingRing(ResourceAllocator& allocator, uint64_t capacityBytes, uint32_t alignment)
    : capacityBytes_(capacityBytes), alignment_(alignment == 0u ? 1u : alignment) {
    capacityBytes_ = alignUp(capacityBytes_);
    if (capacityBytes_ == 0) {
        return;
    }
    buffer_.create(allocator, BufferDesc{
        .size = capacityBytes_,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "streaming staging ring",
    });
    mapped_ = buffer_.mappedData();
}

StreamingStagingRing::StreamingStagingRing(uint64_t capacityBytes, uint32_t alignment)
    : capacityBytes_(capacityBytes), alignment_(alignment == 0u ? 1u : alignment) {
    capacityBytes_ = alignUp(capacityBytes_);
}

StreamingStagingRing::~StreamingStagingRing() = default;

StreamingStagingRing::StreamingStagingRing(StreamingStagingRing&& other) noexcept {
    *this = std::move(other);
}

StreamingStagingRing& StreamingStagingRing::operator=(StreamingStagingRing&& other) noexcept {
    if (this != &other) {
        buffer_ = std::move(other.buffer_);
        mapped_ = other.mapped_;
        capacityBytes_ = other.capacityBytes_;
        alignment_ = other.alignment_;
        head_ = other.head_;
        tail_ = other.tail_;
        pending_ = std::move(other.pending_);
        peakInFlightBytes_ = other.peakInFlightBytes_;
        totalAllocatedBytes_ = other.totalAllocatedBytes_;
        totalReclaimedBytes_ = other.totalReclaimedBytes_;
        allocationFailureCount_ = other.allocationFailureCount_;
        highestRetiredTimeline_ = other.highestRetiredTimeline_;
        other.mapped_ = nullptr;
        other.capacityBytes_ = 0;
        other.head_ = 0;
        other.tail_ = 0;
    }
    return *this;
}

uint64_t StreamingStagingRing::alignUp(uint64_t value) const {
    const uint64_t a = alignment_ == 0u ? 1u : alignment_;
    return (value + a - 1u) / a * a;
}

uint64_t StreamingStagingRing::inFlightBytes() const {
    return head_ - tail_;
}

std::optional<StreamingStagingAllocation> StreamingStagingRing::allocate(uint64_t bytes, uint64_t timelineValue) {
    if (bytes == 0 || capacityBytes_ == 0) {
        ++allocationFailureCount_;
        return std::nullopt;
    }
    const uint64_t alignedBytes = alignUp(bytes);
    if (alignedBytes > capacityBytes_) {
        // Larger than the whole ring; cannot ever fit.
        ++allocationFailureCount_;
        return std::nullopt;
    }

    // Physical offset for the current monotonic head.
    uint64_t physOffset = head_ % capacityBytes_;
    uint64_t candidateHead = head_;
    // If the request would straddle the physical end of the ring, pad up to the
    // next wrap boundary so the returned range is physically contiguous. The
    // padding consumes monotonic space and must be covered by the free check.
    if (physOffset + alignedBytes > capacityBytes_) {
        const uint64_t padding = capacityBytes_ - physOffset;
        candidateHead = head_ + padding;
        physOffset = 0;
    }

    const uint64_t newInFlight = (candidateHead + alignedBytes) - tail_;
    if (newInFlight > capacityBytes_) {
        // Bounded: refuse rather than overrun memory still consumed by the GPU.
        ++allocationFailureCount_;
        return std::nullopt;
    }

    head_ = candidateHead + alignedBytes;
    pending_.push_back(Pending{.offset = physOffset, .size = alignedBytes, .timelineValue = timelineValue});
    totalAllocatedBytes_ += alignedBytes;
    peakInFlightBytes_ = std::max(peakInFlightBytes_, inFlightBytes());

    StreamingStagingAllocation out;
    out.offset = physOffset;
    out.size = alignedBytes;
    out.buffer = buffer_.handle();
    out.mapped = mapped_ != nullptr ? static_cast<void*>(static_cast<char*>(mapped_) + physOffset) : nullptr;
    out.valid = true;
    return out;
}

uint64_t StreamingStagingRing::retire(uint64_t completedTimeline) {
    uint64_t reclaimed = 0;
    while (!pending_.empty() && pending_.front().timelineValue <= completedTimeline) {
        const Pending& front = pending_.front();
        // Advance tail past any wrap padding plus this allocation. Because
        // allocations retire in submission order, the tail's physical position
        // is reconstructed from the recorded offset/size.
        const uint64_t tailPhys = tail_ % capacityBytes_;
        if (front.offset < tailPhys) {
            // This allocation wrapped; account for the padding skipped at the
            // end of the ring before it.
            tail_ += (capacityBytes_ - tailPhys) + front.size;
        } else {
            tail_ += (front.offset - tailPhys) + front.size;
        }
        reclaimed += front.size;
        pending_.pop_front();
    }
    totalReclaimedBytes_ += reclaimed;
    highestRetiredTimeline_ = std::max(highestRetiredTimeline_, completedTimeline);
    if (pending_.empty()) {
        // Fully drained: collapse monotonic counters to avoid unbounded growth.
        head_ = head_ % capacityBytes_;
        tail_ = head_;
    }
    return reclaimed;
}

StreamingStagingRingStats StreamingStagingRing::stats() const {
    StreamingStagingRingStats out;
    out.capacityBytes = capacityBytes_;
    out.inFlightBytes = inFlightBytes();
    out.freeBytes = capacityBytes_ - std::min(capacityBytes_, inFlightBytes());
    out.peakInFlightBytes = peakInFlightBytes_;
    out.totalAllocatedBytes = totalAllocatedBytes_;
    out.totalReclaimedBytes = totalReclaimedBytes_;
    out.liveAllocationCount = static_cast<uint32_t>(pending_.size());
    out.allocationFailureCount = allocationFailureCount_;
    out.highestRetiredTimeline = highestRetiredTimeline_;
    return out;
}

nlohmann::json streamingStagingRingStatsJson(const StreamingStagingRingStats& stats) {
    return {
        {"capacity_bytes", stats.capacityBytes},
        {"in_flight_bytes", stats.inFlightBytes},
        {"free_bytes", stats.freeBytes},
        {"peak_in_flight_bytes", stats.peakInFlightBytes},
        {"total_allocated_bytes", stats.totalAllocatedBytes},
        {"total_reclaimed_bytes", stats.totalReclaimedBytes},
        {"live_allocation_count", stats.liveAllocationCount},
        {"allocation_failure_count", stats.allocationFailureCount},
        {"highest_retired_timeline", stats.highestRetiredTimeline},
    };
}

} // namespace rtv
