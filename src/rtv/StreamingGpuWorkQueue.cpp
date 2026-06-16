#include "rtv/StreamingGpuWorkQueue.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <utility>

namespace rtv {
namespace {

bool terminalState(StreamingGpuWorkState state) {
    return state == StreamingGpuWorkState::Complete ||
        state == StreamingGpuWorkState::Cancelled ||
        state == StreamingGpuWorkState::Failed;
}

StreamingGpuWorkDesc makeSimulationTicket(uint32_t index) {
    StreamingGpuWorkDesc desc;
    desc.kind = static_cast<StreamingGpuWorkKind>(index % 6u);
    desc.label = "streaming GPU work " + std::to_string(index);
    desc.ownerGuid = "streaming-gpu-owner-" + std::to_string(index % 7u);
    switch (desc.kind) {
    case StreamingGpuWorkKind::BufferUpload:
        desc.bytes = (4ull + (index % 4u) * 2ull) * 1024ull * 1024ull;
        desc.estimatedGpuMs = 0.10 + static_cast<double>(index % 3u) * 0.05;
        break;
    case StreamingGpuWorkKind::ImageMipUpload:
        desc.bytes = (2ull + (index % 6u)) * 1024ull * 1024ull;
        desc.estimatedGpuMs = 0.12 + static_cast<double>(index % 4u) * 0.04;
        desc.textureMipCount = 6;
        desc.textureMipLevel = 5u - (index % 6u);
        break;
    case StreamingGpuWorkKind::BlasBuild:
        desc.estimatedGpuMs = 0.45 + static_cast<double>(index % 3u) * 0.20;
        desc.blasBuilds = 1;
        break;
    case StreamingGpuWorkKind::BlasCompaction:
        desc.estimatedGpuMs = 0.20;
        desc.blasBuilds = 1;
        break;
    case StreamingGpuWorkKind::TlasPatch:
        desc.estimatedGpuMs = 0.08;
        desc.tlasPatches = 1;
        break;
    case StreamingGpuWorkKind::DescriptorUpdate:
        desc.estimatedGpuMs = 0.03;
        desc.descriptorUpdates = 32u + (index % 4u) * 16u;
        break;
    }
    return desc;
}

} // namespace

const char* streamingGpuWorkKindName(StreamingGpuWorkKind kind) {
    switch (kind) {
    case StreamingGpuWorkKind::BufferUpload: return "buffer_upload";
    case StreamingGpuWorkKind::ImageMipUpload: return "image_mip_upload";
    case StreamingGpuWorkKind::BlasBuild: return "blas_build";
    case StreamingGpuWorkKind::BlasCompaction: return "blas_compaction";
    case StreamingGpuWorkKind::TlasPatch: return "tlas_patch";
    case StreamingGpuWorkKind::DescriptorUpdate: return "descriptor_update";
    }
    return "buffer_upload";
}

const char* streamingGpuWorkStateName(StreamingGpuWorkState state) {
    switch (state) {
    case StreamingGpuWorkState::Queued: return "queued";
    case StreamingGpuWorkState::Submitted: return "submitted";
    case StreamingGpuWorkState::WaitingForTimeline: return "waiting_for_timeline";
    case StreamingGpuWorkState::Complete: return "complete";
    case StreamingGpuWorkState::Cancelled: return "cancelled";
    case StreamingGpuWorkState::Failed: return "failed";
    }
    return "queued";
}

uint64_t StreamingGpuWorkQueue::enqueue(StreamingGpuWorkDesc desc) {
    if (desc.label.empty()) {
        desc.label = streamingGpuWorkKindName(desc.kind);
    }
    Ticket ticket;
    ticket.id = nextTicketId_++;
    ticket.desc = std::move(desc);
    tickets_.push_back(std::move(ticket));
    return tickets_.back().id;
}

StreamingGpuWorkFrameResult StreamingGpuWorkQueue::submitFrame(const StreamingGpuWorkBudget& budget) {
    StreamingGpuWorkFrameResult result;
    result.frameIndex = frameIndex_++;
    for (Ticket& ticket : tickets_) {
        if (ticket.state != StreamingGpuWorkState::Queued) {
            continue;
        }
        StreamingGpuWorkFrameResult exhausted;
        if (!canSubmit(ticket, budget, result, exhausted)) {
            result.uploadBudgetExhausted = result.uploadBudgetExhausted || exhausted.uploadBudgetExhausted;
            result.stagingBudgetExhausted = result.stagingBudgetExhausted || exhausted.stagingBudgetExhausted;
            result.gpuBudgetExhausted = result.gpuBudgetExhausted || exhausted.gpuBudgetExhausted;
            result.submissionBudgetExhausted = result.submissionBudgetExhausted || exhausted.submissionBudgetExhausted;
            result.blasBudgetExhausted = result.blasBudgetExhausted || exhausted.blasBudgetExhausted;
            result.tlasBudgetExhausted = result.tlasBudgetExhausted || exhausted.tlasBudgetExhausted;
            result.descriptorBudgetExhausted = result.descriptorBudgetExhausted || exhausted.descriptorBudgetExhausted;
            break;
        }

        ticket.state = StreamingGpuWorkState::WaitingForTimeline;
        ticket.submittedTimeline = nextTimelineValue_++;
        if (usesStaging(ticket)) {
            ticket.retainedStagingBytes = ticket.desc.bytes;
            result.submittedStagingBytes += ticket.retainedStagingBytes;
        }
        ++result.submittedTickets;
        result.submittedBytes += ticket.desc.bytes;
        result.submittedGpuMs += ticket.desc.estimatedGpuMs;
        result.submittedBlasBuilds += ticket.desc.blasBuilds;
        result.submittedTlasPatches += ticket.desc.tlasPatches;
        result.submittedDescriptorUpdates += ticket.desc.descriptorUpdates;
        result.highestSubmittedTimeline = ticket.submittedTimeline;
        peakRetainedStagingBytes_ = std::max(peakRetainedStagingBytes_, retainedStagingBytes());
    }
    result.retainedStagingBytes = retainedStagingBytes();
    recordPressure(result);
    return result;
}

bool StreamingGpuWorkQueue::completeTimeline(uint64_t completedTimeline) {
    bool changed = false;
    for (Ticket& ticket : tickets_) {
        if (ticket.submittedTimeline != 0 &&
            ticket.submittedTimeline <= completedTimeline &&
            (ticket.state == StreamingGpuWorkState::WaitingForTimeline || ticket.retainedStagingBytes != 0)) {
            if (ticket.state == StreamingGpuWorkState::WaitingForTimeline) {
                ticket.state = StreamingGpuWorkState::Complete;
            }
            if (ticket.retainedStagingBytes != 0) {
                ticket.retainedStagingBytes = 0;
            }
            changed = true;
        }
    }
    return changed;
}

bool StreamingGpuWorkQueue::cancel(uint64_t id) {
    for (Ticket& ticket : tickets_) {
        if (ticket.id == id && !terminal(ticket.state)) {
            ticket.state = StreamingGpuWorkState::Cancelled;
            return true;
        }
    }
    return false;
}

bool StreamingGpuWorkQueue::empty() const {
    return std::all_of(tickets_.begin(), tickets_.end(), [&](const Ticket& ticket) {
        return terminal(ticket.state);
    });
}

std::vector<StreamingGpuWorkSnapshot> StreamingGpuWorkQueue::snapshots() const {
    std::vector<StreamingGpuWorkSnapshot> out;
    out.reserve(tickets_.size());
    for (const Ticket& ticket : tickets_) {
        out.push_back({
            .id = ticket.id,
            .kind = ticket.desc.kind,
            .state = ticket.state,
            .label = ticket.desc.label,
            .ownerGuid = ticket.desc.ownerGuid,
            .bytes = ticket.desc.bytes,
            .estimatedGpuMs = ticket.desc.estimatedGpuMs,
            .textureMipLevel = ticket.desc.textureMipLevel,
            .textureMipCount = ticket.desc.textureMipCount,
            .blasBuilds = ticket.desc.blasBuilds,
            .tlasPatches = ticket.desc.tlasPatches,
            .descriptorUpdates = ticket.desc.descriptorUpdates,
            .submittedTimeline = ticket.submittedTimeline,
            .retainedStagingBytes = ticket.retainedStagingBytes,
            .requiresGraphicsQueue = ticket.desc.requiresGraphicsQueue,
            .canCancel = !terminal(ticket.state),
            .canRetire = terminal(ticket.state) && ticket.retainedStagingBytes == 0,
        });
    }
    return out;
}

StreamingGpuWorkQueueStats StreamingGpuWorkQueue::stats() const {
    StreamingGpuWorkQueueStats out;
    for (const Ticket& ticket : tickets_) {
        switch (ticket.state) {
        case StreamingGpuWorkState::Queued:
            ++out.queued;
            out.queuedBytes += ticket.desc.bytes;
            break;
        case StreamingGpuWorkState::Submitted:
            ++out.submitted;
            out.submittedBytes += ticket.desc.bytes;
            out.retainedStagingBytes += ticket.retainedStagingBytes;
            break;
        case StreamingGpuWorkState::WaitingForTimeline:
            ++out.waitingForTimeline;
            out.submittedBytes += ticket.desc.bytes;
            out.retainedStagingBytes += ticket.retainedStagingBytes;
            break;
        case StreamingGpuWorkState::Complete:
            ++out.complete;
            out.completedBytes += ticket.desc.bytes;
            break;
        case StreamingGpuWorkState::Cancelled:
            ++out.cancelled;
            out.retainedStagingBytes += ticket.retainedStagingBytes;
            break;
        case StreamingGpuWorkState::Failed:
            ++out.failed;
            break;
        }
    }
    out.peakRetainedStagingBytes = peakRetainedStagingBytes_;
    return out;
}

StreamingGpuWorkPressureStats StreamingGpuWorkQueue::pressureStats() const {
    return pressure_;
}

bool StreamingGpuWorkQueue::canSubmit(
    const Ticket& ticket,
    const StreamingGpuWorkBudget& budget,
    const StreamingGpuWorkFrameResult& frame,
    StreamingGpuWorkFrameResult& exhausted) const {
    if (budget.maxSubmissions != 0 && frame.submittedTickets >= budget.maxSubmissions) {
        exhausted.submissionBudgetExhausted = true;
        return false;
    }
    if (frame.submittedBytes + ticket.desc.bytes > budget.maxUploadBytes) {
        exhausted.uploadBudgetExhausted = true;
        return false;
    }
    if (budget.maxStagingBytes != 0 && usesStaging(ticket)) {
        const uint64_t stagingBytes = retainedStagingBytes() + frame.submittedStagingBytes;
        if (stagingBytes + ticket.desc.bytes > budget.maxStagingBytes) {
            exhausted.stagingBudgetExhausted = true;
            return false;
        }
    }
    if (frame.submittedGpuMs + ticket.desc.estimatedGpuMs > budget.maxGpuMs) {
        exhausted.gpuBudgetExhausted = true;
        return false;
    }
    if (frame.submittedBlasBuilds + ticket.desc.blasBuilds > budget.maxBlasBuilds) {
        exhausted.blasBudgetExhausted = true;
        return false;
    }
    if (frame.submittedTlasPatches + ticket.desc.tlasPatches > budget.maxTlasPatches) {
        exhausted.tlasBudgetExhausted = true;
        return false;
    }
    if (frame.submittedDescriptorUpdates + ticket.desc.descriptorUpdates > budget.maxDescriptorUpdates) {
        exhausted.descriptorBudgetExhausted = true;
        return false;
    }
    return true;
}

bool StreamingGpuWorkQueue::usesStaging(const Ticket& ticket) const {
    return ticket.desc.kind == StreamingGpuWorkKind::BufferUpload ||
        ticket.desc.kind == StreamingGpuWorkKind::ImageMipUpload;
}

uint64_t StreamingGpuWorkQueue::retainedStagingBytes() const {
    uint64_t bytes = 0;
    for (const Ticket& ticket : tickets_) {
        bytes += ticket.retainedStagingBytes;
    }
    return bytes;
}

void StreamingGpuWorkQueue::recordPressure(const StreamingGpuWorkFrameResult& frame) {
    pressure_.lastFrame = frame;
    ++pressure_.frames;
    if (frame.submittedTickets == 0) {
        const bool pressured = frame.uploadBudgetExhausted ||
            frame.stagingBudgetExhausted ||
            frame.gpuBudgetExhausted ||
            frame.submissionBudgetExhausted ||
            frame.blasBudgetExhausted ||
            frame.tlasBudgetExhausted ||
            frame.descriptorBudgetExhausted;
        if (pressured) {
            ++pressure_.stalledFrames;
        } else {
            ++pressure_.idleFrames;
        }
    }
    if (frame.uploadBudgetExhausted) {
        ++pressure_.uploadBudgetFrames;
    }
    if (frame.stagingBudgetExhausted) {
        ++pressure_.stagingBudgetFrames;
    }
    if (frame.gpuBudgetExhausted) {
        ++pressure_.gpuBudgetFrames;
    }
    if (frame.submissionBudgetExhausted) {
        ++pressure_.submissionBudgetFrames;
    }
    if (frame.blasBudgetExhausted) {
        ++pressure_.blasBudgetFrames;
    }
    if (frame.tlasBudgetExhausted) {
        ++pressure_.tlasBudgetFrames;
    }
    if (frame.descriptorBudgetExhausted) {
        ++pressure_.descriptorBudgetFrames;
    }
    pressure_.peakRetainedStagingBytes = peakRetainedStagingBytes_;
}

bool StreamingGpuWorkQueue::terminal(StreamingGpuWorkState state) const {
    return terminalState(state);
}

nlohmann::json streamingGpuWorkQueueStatsJson(const StreamingGpuWorkQueueStats& stats) {
    return {
        {"queued", stats.queued},
        {"submitted", stats.submitted},
        {"waiting_for_timeline", stats.waitingForTimeline},
        {"complete", stats.complete},
        {"cancelled", stats.cancelled},
        {"failed", stats.failed},
        {"queued_bytes", stats.queuedBytes},
        {"submitted_bytes", stats.submittedBytes},
        {"completed_bytes", stats.completedBytes},
        {"retained_staging_bytes", stats.retainedStagingBytes},
        {"peak_retained_staging_bytes", stats.peakRetainedStagingBytes},
    };
}

nlohmann::json streamingGpuWorkFrameResultJson(const StreamingGpuWorkFrameResult& frame) {
    nlohmann::json hitchSources = nlohmann::json::array();
    if (frame.uploadBudgetExhausted) {
        hitchSources.push_back("Upload");
    }
    if (frame.stagingBudgetExhausted) {
        hitchSources.push_back("StagingMemory");
    }
    if (frame.gpuBudgetExhausted) {
        hitchSources.push_back("GpuWork");
    }
    if (frame.submissionBudgetExhausted) {
        hitchSources.push_back("Submission");
    }
    if (frame.blasBudgetExhausted) {
        hitchSources.push_back("BlasBuild");
    }
    if (frame.tlasBudgetExhausted) {
        hitchSources.push_back("TlasPatch");
    }
    if (frame.descriptorBudgetExhausted) {
        hitchSources.push_back("DescriptorUpdate");
    }

    return {
        {"frame_index", frame.frameIndex},
        {"submitted_tickets", frame.submittedTickets},
        {"completed_tickets", frame.completedTickets},
        {"submitted_bytes", frame.submittedBytes},
        {"submitted_gpu_ms", frame.submittedGpuMs},
        {"submitted_blas_builds", frame.submittedBlasBuilds},
        {"submitted_tlas_patches", frame.submittedTlasPatches},
        {"submitted_descriptor_updates", frame.submittedDescriptorUpdates},
        {"highest_submitted_timeline", frame.highestSubmittedTimeline},
        {"submitted_staging_bytes", frame.submittedStagingBytes},
        {"retained_staging_bytes", frame.retainedStagingBytes},
        {"upload_budget_exhausted", frame.uploadBudgetExhausted},
        {"staging_budget_exhausted", frame.stagingBudgetExhausted},
        {"gpu_budget_exhausted", frame.gpuBudgetExhausted},
        {"submission_budget_exhausted", frame.submissionBudgetExhausted},
        {"blas_budget_exhausted", frame.blasBudgetExhausted},
        {"tlas_budget_exhausted", frame.tlasBudgetExhausted},
        {"descriptor_budget_exhausted", frame.descriptorBudgetExhausted},
        {"hitch_sources", hitchSources},
    };
}

nlohmann::json streamingGpuWorkPressureStatsJson(const StreamingGpuWorkPressureStats& stats) {
    nlohmann::json activeHitchSources = nlohmann::json::array();
    if (stats.uploadBudgetFrames > 0) {
        activeHitchSources.push_back("Upload");
    }
    if (stats.stagingBudgetFrames > 0) {
        activeHitchSources.push_back("StagingMemory");
    }
    if (stats.gpuBudgetFrames > 0) {
        activeHitchSources.push_back("GpuWork");
    }
    if (stats.submissionBudgetFrames > 0) {
        activeHitchSources.push_back("Submission");
    }
    if (stats.blasBudgetFrames > 0) {
        activeHitchSources.push_back("BlasBuild");
    }
    if (stats.tlasBudgetFrames > 0) {
        activeHitchSources.push_back("TlasPatch");
    }
    if (stats.descriptorBudgetFrames > 0) {
        activeHitchSources.push_back("DescriptorUpdate");
    }

    return {
        {"frames", stats.frames},
        {"idle_frames", stats.idleFrames},
        {"stalled_frames", stats.stalledFrames},
        {"budget_pressure_frames", {
            {"upload", stats.uploadBudgetFrames},
            {"staging_memory", stats.stagingBudgetFrames},
            {"gpu_work", stats.gpuBudgetFrames},
            {"submission", stats.submissionBudgetFrames},
            {"blas_build", stats.blasBudgetFrames},
            {"tlas_patch", stats.tlasBudgetFrames},
            {"descriptor_update", stats.descriptorBudgetFrames},
        }},
        {"active_hitch_sources", activeHitchSources},
        {"peak_retained_staging_bytes", stats.peakRetainedStagingBytes},
        {"last_frame", streamingGpuWorkFrameResultJson(stats.lastFrame)},
    };
}

nlohmann::json streamingGpuWorkQueueSnapshotsJson(const std::vector<StreamingGpuWorkSnapshot>& snapshots) {
    nlohmann::json out = nlohmann::json::array();
    for (const StreamingGpuWorkSnapshot& snapshot : snapshots) {
        out.push_back({
            {"id", snapshot.id},
            {"kind", streamingGpuWorkKindName(snapshot.kind)},
            {"state", streamingGpuWorkStateName(snapshot.state)},
            {"label", snapshot.label},
            {"owner_guid", snapshot.ownerGuid},
            {"bytes", snapshot.bytes},
            {"estimated_gpu_ms", snapshot.estimatedGpuMs},
            {"texture_mip_level", snapshot.textureMipLevel == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(snapshot.textureMipLevel)},
            {"texture_mip_count", snapshot.textureMipCount},
            {"blas_builds", snapshot.blasBuilds},
            {"tlas_patches", snapshot.tlasPatches},
            {"descriptor_updates", snapshot.descriptorUpdates},
            {"submitted_timeline", snapshot.submittedTimeline},
            {"retained_staging_bytes", snapshot.retainedStagingBytes},
            {"requires_graphics_queue", snapshot.requiresGraphicsQueue},
            {"can_cancel", snapshot.canCancel},
            {"can_retire", snapshot.canRetire},
        });
    }
    return out;
}

int simulateStreamingGpuWorkQueueCommand(
    uint32_t ticketCount,
    const StreamingGpuWorkBudget& budget,
    uint32_t completeLagFrames,
    const std::filesystem::path& jsonOut) {
    StreamingGpuWorkQueue queue;
    for (uint32_t i = 0; i < ticketCount; ++i) {
        (void)queue.enqueue(makeSimulationTicket(i));
    }

    nlohmann::json frames = nlohmann::json::array();
    std::deque<uint64_t> submittedTimelineHistory;
    constexpr uint32_t kMaxFrames = 512;
    for (uint32_t frame = 0; frame < kMaxFrames && !queue.empty(); ++frame) {
        if (submittedTimelineHistory.size() > completeLagFrames) {
            const uint64_t completedTimeline = submittedTimelineHistory.front();
            submittedTimelineHistory.pop_front();
            (void)queue.completeTimeline(completedTimeline);
        }
        StreamingGpuWorkFrameResult result = queue.submitFrame(budget);
        if (result.highestSubmittedTimeline != 0) {
            submittedTimelineHistory.push_back(result.highestSubmittedTimeline);
        }
        frames.push_back(streamingGpuWorkFrameResultJson(result));
        if (result.submittedTickets == 0 && !submittedTimelineHistory.empty()) {
            const uint64_t completedTimeline = submittedTimelineHistory.front();
            submittedTimelineHistory.pop_front();
            (void)queue.completeTimeline(completedTimeline);
            continue;
        }
        if (result.submittedTickets == 0 && submittedTimelineHistory.empty()) {
            break;
        }
    }
    if (!submittedTimelineHistory.empty()) {
        (void)queue.completeTimeline(submittedTimelineHistory.back());
    }

    const nlohmann::json report = {
        {"schema", "StreamingGpuWorkQueueSimulationV1"},
        {"ok", queue.empty()},
        {"ticket_count", ticketCount},
        {"budget", {
            {"max_upload_bytes", budget.maxUploadBytes},
            {"max_staging_bytes", budget.maxStagingBytes},
            {"max_gpu_ms", budget.maxGpuMs},
            {"max_submissions", budget.maxSubmissions},
            {"max_blas_builds", budget.maxBlasBuilds},
            {"max_tlas_patches", budget.maxTlasPatches},
            {"max_descriptor_updates", budget.maxDescriptorUpdates},
        }},
        {"complete_lag_frames", completeLagFrames},
        {"frames", frames},
        {"stats", streamingGpuWorkQueueStatsJson(queue.stats())},
        {"hitch_summary", streamingGpuWorkPressureStatsJson(queue.pressureStats())},
        {"tickets", streamingGpuWorkQueueSnapshotsJson(queue.snapshots())},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Could not create streaming GPU work report directory: " << parent.string() << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Could not write streaming GPU work report: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return report.value("ok", false) ? 0 : 1;
}

} // namespace rtv
