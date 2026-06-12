#include "rtv/GpuUploadTicket.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace rtv {
namespace {

[[nodiscard]] uint64_t safeChunkSize(uint64_t value) {
    return value == 0 ? 16ull * 1024ull * 1024ull : value;
}

[[nodiscard]] bool terminal(GpuUploadTicketState state) {
    return state == GpuUploadTicketState::Cancelled || state == GpuUploadTicketState::Complete || state == GpuUploadTicketState::Failed;
}

} // namespace

const char* gpuUploadResourceKindName(GpuUploadResourceKind kind) {
    switch (kind) {
    case GpuUploadResourceKind::Buffer: return "buffer";
    case GpuUploadResourceKind::Image: return "image";
    }
    return "unknown";
}

const char* gpuUploadChunkStateName(GpuUploadChunkState state) {
    switch (state) {
    case GpuUploadChunkState::Pending: return "pending";
    case GpuUploadChunkState::Submitted: return "submitted";
    case GpuUploadChunkState::Complete: return "complete";
    case GpuUploadChunkState::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* gpuUploadTicketStateName(GpuUploadTicketState state) {
    switch (state) {
    case GpuUploadTicketState::Queued: return "queued";
    case GpuUploadTicketState::Submitting: return "submitting";
    case GpuUploadTicketState::WaitingForFence: return "waitingForFence";
    case GpuUploadTicketState::Cancelled: return "cancelled";
    case GpuUploadTicketState::Complete: return "complete";
    case GpuUploadTicketState::Failed: return "failed";
    }
    return "unknown";
}

GpuUploadTicket::GpuUploadTicket(uint64_t id, GpuUploadTicketDesc desc)
    : id_(id),
      kind_(desc.kind),
      label_(std::move(desc.label)),
      totalBytes_(desc.totalBytes) {
    const uint64_t chunkBytes = safeChunkSize(desc.chunkBytes);
    uint64_t offset = 0;
    while (offset < totalBytes_) {
        const uint64_t remaining = totalBytes_ - offset;
        const uint64_t bytes = std::min(chunkBytes, remaining);
        chunks_.push_back(Chunk{.offset = offset, .bytes = bytes});
        offset += bytes;
    }
    if (chunks_.empty()) {
        state_ = GpuUploadTicketState::Complete;
    }
}

uint64_t GpuUploadTicket::submittedBytes() const {
    uint64_t bytes = 0;
    for (const Chunk& chunk : chunks_) {
        if (chunk.state == GpuUploadChunkState::Submitted || chunk.state == GpuUploadChunkState::Complete) {
            bytes += chunk.bytes;
        }
    }
    return bytes;
}

uint64_t GpuUploadTicket::completedBytes() const {
    uint64_t bytes = 0;
    for (const Chunk& chunk : chunks_) {
        if (chunk.state == GpuUploadChunkState::Complete) {
            bytes += chunk.bytes;
        }
    }
    return bytes;
}

uint64_t GpuUploadTicket::retainedStagingBytes() const {
    uint64_t bytes = 0;
    for (const Chunk& chunk : chunks_) {
        if (chunk.stagingRetained) {
            bytes += chunk.bytes;
        }
    }
    return bytes;
}

bool GpuUploadTicket::canRetire() const {
    return terminal(state_) && retainedStagingBytes() == 0;
}

GpuUploadSubmitResult GpuUploadTicket::submitFrame(const GpuUploadFrameBudget& budget, uint64_t& nextTimelineValue) {
    GpuUploadSubmitResult result;
    if (terminal(state_) || cancellationRequested_) {
        result.ticketComplete = state_ == GpuUploadTicketState::Complete;
        return result;
    }

    uint64_t remainingBytes = budget.maxBytes;
    uint32_t remainingSubmissions = budget.maxSubmissions == 0 ? std::numeric_limits<uint32_t>::max() : budget.maxSubmissions;
    for (Chunk& chunk : chunks_) {
        if (chunk.state != GpuUploadChunkState::Pending) {
            continue;
        }
        if (remainingSubmissions == 0 || chunk.bytes > remainingBytes) {
            result.budgetExhausted = true;
            break;
        }
        chunk.state = GpuUploadChunkState::Submitted;
        chunk.timelineValue = nextTimelineValue++;
        chunk.stagingRetained = true;
        result.submittedBytes += chunk.bytes;
        ++result.submittedChunks;
        remainingBytes -= chunk.bytes;
        --remainingSubmissions;
    }

    refreshState();
    result.ticketComplete = state_ == GpuUploadTicketState::Complete;
    return result;
}

bool GpuUploadTicket::requestCancel(std::string reason) {
    if (terminal(state_)) {
        return false;
    }
    cancellationRequested_ = true;
    cancelReason_ = std::move(reason);
    for (Chunk& chunk : chunks_) {
        if (chunk.state == GpuUploadChunkState::Pending) {
            chunk.state = GpuUploadChunkState::Cancelled;
            chunk.stagingRetained = false;
        }
    }
    refreshState();
    return true;
}

bool GpuUploadTicket::completeTimeline(uint64_t completedTimelineValue) {
    bool changed = false;
    for (Chunk& chunk : chunks_) {
        if (chunk.state == GpuUploadChunkState::Submitted && chunk.timelineValue <= completedTimelineValue) {
            chunk.state = GpuUploadChunkState::Complete;
            chunk.stagingRetained = false;
            changed = true;
        }
    }
    refreshState();
    return changed;
}

GpuUploadTicketSnapshot GpuUploadTicket::snapshot(bool includeChunks) const {
    GpuUploadTicketSnapshot out;
    out.id = id_;
    out.kind = kind_;
    out.state = state_;
    out.label = label_;
    out.totalBytes = totalBytes_;
    out.submittedBytes = submittedBytes();
    out.completedBytes = completedBytes();
    out.retainedStagingBytes = retainedStagingBytes();
    out.chunkCount = chunks_.size();
    out.cancellationRequested = cancellationRequested_;
    out.canCancel = !terminal(state_);
    out.canRetire = canRetire();
    for (size_t i = 0; i < chunks_.size(); ++i) {
        const Chunk& chunk = chunks_[i];
        switch (chunk.state) {
        case GpuUploadChunkState::Pending: ++out.pendingChunks; break;
        case GpuUploadChunkState::Submitted: ++out.submittedChunks; break;
        case GpuUploadChunkState::Complete: ++out.completedChunks; break;
        case GpuUploadChunkState::Cancelled: break;
        }
        if (includeChunks) {
            out.chunks.push_back({
                .index = i,
                .offset = chunk.offset,
                .bytes = chunk.bytes,
                .timelineValue = chunk.timelineValue,
                .state = chunk.state,
                .stagingRetained = chunk.stagingRetained,
            });
        }
    }
    return out;
}

void GpuUploadTicket::refreshState() {
    const bool anyPending = std::any_of(chunks_.begin(), chunks_.end(), [](const Chunk& chunk) {
        return chunk.state == GpuUploadChunkState::Pending;
    });
    const bool anySubmitted = hasSubmittedChunks();
    const bool allCompleteOrCancelled = std::all_of(chunks_.begin(), chunks_.end(), [](const Chunk& chunk) {
        return chunk.state == GpuUploadChunkState::Complete || chunk.state == GpuUploadChunkState::Cancelled;
    });

    if (cancellationRequested_ && !anySubmitted) {
        state_ = GpuUploadTicketState::Cancelled;
    } else if (allCompleteOrCancelled && !cancellationRequested_) {
        state_ = GpuUploadTicketState::Complete;
    } else if (allCompleteOrCancelled && cancellationRequested_) {
        state_ = GpuUploadTicketState::Cancelled;
    } else if (anySubmitted) {
        state_ = GpuUploadTicketState::WaitingForFence;
    } else if (anyPending) {
        state_ = GpuUploadTicketState::Queued;
    }
}

bool GpuUploadTicket::hasSubmittedChunks() const {
    return std::any_of(chunks_.begin(), chunks_.end(), [](const Chunk& chunk) {
        return chunk.state == GpuUploadChunkState::Submitted;
    });
}

uint64_t GpuUploadTicketQueue::create(GpuUploadTicketDesc desc) {
    const uint64_t id = nextTicketId_++;
    tickets_.emplace_back(id, std::move(desc));
    return id;
}

GpuUploadSubmitResult GpuUploadTicketQueue::submitFrame(const GpuUploadFrameBudget& budget) {
    GpuUploadSubmitResult combined;
    uint64_t remainingBytes = budget.maxBytes;
    uint32_t remainingSubmissions = budget.maxSubmissions;
    for (GpuUploadTicket& ticket : tickets_) {
        GpuUploadFrameBudget perTicketBudget{
            .maxBytes = remainingBytes,
            .maxSubmissions = remainingSubmissions,
        };
        const GpuUploadSubmitResult result = ticket.submitFrame(perTicketBudget, nextTimelineValue_);
        combined.submittedBytes += result.submittedBytes;
        combined.submittedChunks += result.submittedChunks;
        combined.budgetExhausted = combined.budgetExhausted || result.budgetExhausted;
        if (remainingBytes >= result.submittedBytes) {
            remainingBytes -= result.submittedBytes;
        } else {
            remainingBytes = 0;
        }
        if (remainingSubmissions != 0) {
            remainingSubmissions = result.submittedChunks >= remainingSubmissions ? 0u : remainingSubmissions - static_cast<uint32_t>(result.submittedChunks);
        }
        if (remainingBytes == 0 || remainingSubmissions == 0) {
            combined.budgetExhausted = true;
            break;
        }
    }
    combined.ticketComplete = std::all_of(tickets_.begin(), tickets_.end(), [](const GpuUploadTicket& ticket) {
        return ticket.state() == GpuUploadTicketState::Complete || ticket.state() == GpuUploadTicketState::Cancelled;
    });
    return combined;
}

bool GpuUploadTicketQueue::requestCancel(uint64_t id, std::string reason) {
    for (GpuUploadTicket& ticket : tickets_) {
        if (ticket.id() == id) {
            return ticket.requestCancel(std::move(reason));
        }
    }
    return false;
}

bool GpuUploadTicketQueue::completeTimeline(uint64_t completedTimelineValue) {
    bool changed = false;
    for (GpuUploadTicket& ticket : tickets_) {
        changed = ticket.completeTimeline(completedTimelineValue) || changed;
    }
    return changed;
}

std::vector<GpuUploadTicketSnapshot> GpuUploadTicketQueue::snapshots(bool includeChunks) const {
    std::vector<GpuUploadTicketSnapshot> out;
    out.reserve(tickets_.size());
    for (const GpuUploadTicket& ticket : tickets_) {
        out.push_back(ticket.snapshot(includeChunks));
    }
    return out;
}

} // namespace rtv
