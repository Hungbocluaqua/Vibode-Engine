#include "rtv/TopologyRebuildTicket.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace rtv {
namespace {

[[nodiscard]] bool terminal(TopologyRebuildTicketState state) {
    return state == TopologyRebuildTicketState::Cancelled || state == TopologyRebuildTicketState::Complete || state == TopologyRebuildTicketState::Failed;
}

[[nodiscard]] bool buildStage(TopologyRebuildStage stage) {
    return stage != TopologyRebuildStage::RetireOldRenderer;
}

} // namespace

const char* topologyRebuildStageName(TopologyRebuildStage stage) {
    switch (stage) {
    case TopologyRebuildStage::CpuSceneExtraction: return "cpuSceneExtraction";
    case TopologyRebuildStage::GpuSceneBufferBuild: return "gpuSceneBufferBuild";
    case TopologyRebuildStage::BufferUploads: return "bufferUploads";
    case TopologyRebuildStage::TextureUploads: return "textureUploads";
    case TopologyRebuildStage::BlasBuildBatch: return "blasBuildBatch";
    case TopologyRebuildStage::TlasBuildOrRefit: return "tlasBuildOrRefit";
    case TopologyRebuildStage::RendererDescriptorUpdate: return "rendererDescriptorUpdate";
    case TopologyRebuildStage::FinalRendererSwap: return "finalRendererSwap";
    case TopologyRebuildStage::RetireOldRenderer: return "retireOldRenderer";
    }
    return "unknown";
}

const char* topologyRebuildStageStateName(TopologyRebuildStageState state) {
    switch (state) {
    case TopologyRebuildStageState::Pending: return "pending";
    case TopologyRebuildStageState::Running: return "running";
    case TopologyRebuildStageState::Complete: return "complete";
    case TopologyRebuildStageState::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* topologyRebuildTicketStateName(TopologyRebuildTicketState state) {
    switch (state) {
    case TopologyRebuildTicketState::Queued: return "queued";
    case TopologyRebuildTicketState::Building: return "building";
    case TopologyRebuildTicketState::WaitingForRetirementFence: return "waitingForRetirementFence";
    case TopologyRebuildTicketState::Cancelled: return "cancelled";
    case TopologyRebuildTicketState::Complete: return "complete";
    case TopologyRebuildTicketState::Failed: return "failed";
    }
    return "unknown";
}

TopologyRebuildTicket::TopologyRebuildTicket(uint64_t id, uint64_t generation, std::string label, std::vector<TopologyRebuildStageDesc> stages)
    : id_(id), generation_(generation), label_(std::move(label)) {
    stages_.reserve(stages.size());
    for (TopologyRebuildStageDesc& stage : stages) {
        stages_.push_back(StageState{.desc = std::move(stage)});
    }
    if (stages_.empty()) {
        state_ = TopologyRebuildTicketState::Complete;
        previousRendererVisible_ = false;
        finalRendererSwapped_ = true;
        oldRendererRetired_ = true;
    }
}

bool TopologyRebuildTicket::active() const {
    return !terminal(state_);
}

TopologyRebuildStepResult TopologyRebuildTicket::stepFrame(const TopologyRebuildFrameBudget& budget, uint64_t& nextTimelineValue) {
    TopologyRebuildStepResult result;
    if (terminal(state_)) {
        result.ticketComplete = state_ == TopologyRebuildTicketState::Complete;
        return result;
    }
    if (state_ == TopologyRebuildTicketState::WaitingForRetirementFence) {
        return result;
    }

    state_ = TopologyRebuildTicketState::Building;
    double remainingMs = std::max(0.0, budget.maxCpuMs);
    uint32_t remainingStages = budget.maxStages == 0 ? std::numeric_limits<uint32_t>::max() : budget.maxStages;
    for (StageState& stage : stages_) {
        if (stage.state != TopologyRebuildStageState::Pending) {
            continue;
        }
        const double cost = std::max(0.0, stage.desc.estimatedCostMs);
        if (remainingStages == 0 || (cost > 0.0 && cost > remainingMs)) {
            result.budgetExhausted = true;
            break;
        }
        stage.state = TopologyRebuildStageState::Running;
        stage.state = TopologyRebuildStageState::Complete;
        result.consumedMs += cost;
        ++result.completedStages;
        remainingMs = cost >= remainingMs ? 0.0 : remainingMs - cost;
        --remainingStages;
        if (stage.desc.stage == TopologyRebuildStage::FinalRendererSwap) {
            finalRendererSwapped_ = true;
            previousRendererVisible_ = false;
            oldRendererRetained_ = true;
            retirementTimelineValue_ = nextTimelineValue++;
        } else if (stage.desc.stage == TopologyRebuildStage::RetireOldRenderer) {
            if (oldRendererRetired_) {
                stage.state = TopologyRebuildStageState::Complete;
            } else {
                stage.state = TopologyRebuildStageState::Pending;
                result.budgetExhausted = true;
                break;
            }
        }
    }

    refreshState();
    result.ticketComplete = state_ == TopologyRebuildTicketState::Complete;
    return result;
}

bool TopologyRebuildTicket::cancelAsStale() {
    if (terminal(state_) || finalRendererSwapped_) {
        return false;
    }
    cancellationRequested_ = true;
    staleGeneration_ = true;
    for (StageState& stage : stages_) {
        if (stage.state == TopologyRebuildStageState::Pending || stage.state == TopologyRebuildStageState::Running) {
            stage.state = TopologyRebuildStageState::Cancelled;
        }
    }
    state_ = TopologyRebuildTicketState::Cancelled;
    previousRendererVisible_ = true;
    oldRendererRetained_ = false;
    oldRendererRetired_ = false;
    retirementTimelineValue_ = 0;
    return true;
}

bool TopologyRebuildTicket::completeRetirementFence(uint64_t completedTimelineValue) {
    if (!oldRendererRetained_ || oldRendererRetired_ || retirementTimelineValue_ == 0 || completedTimelineValue < retirementTimelineValue_) {
        return false;
    }
    oldRendererRetained_ = false;
    oldRendererRetired_ = true;
    for (StageState& stage : stages_) {
        if (stage.desc.stage == TopologyRebuildStage::RetireOldRenderer && stage.state == TopologyRebuildStageState::Pending) {
            stage.state = TopologyRebuildStageState::Complete;
        }
    }
    refreshState();
    return true;
}

TopologyRebuildTicketSnapshot TopologyRebuildTicket::snapshot(bool includeStages) const {
    TopologyRebuildTicketSnapshot out;
    out.id = id_;
    out.generation = generation_;
    out.state = state_;
    out.label = label_;
    out.stageCount = stages_.size();
    out.completedStages = completedStageCount();
    out.pendingStages = pendingStageCount();
    out.cancelledStages = 0;
    for (const StageState& stage : stages_) {
        if (stage.state == TopologyRebuildStageState::Cancelled) {
            ++out.cancelledStages;
        }
    }
    out.progress = stages_.empty() ? 1.0f : static_cast<float>(out.completedStages) / static_cast<float>(stages_.size());
    out.previousRendererVisible = previousRendererVisible_;
    out.finalRendererSwapped = finalRendererSwapped_;
    out.oldRendererRetained = oldRendererRetained_;
    out.oldRendererRetired = oldRendererRetired_;
    out.retirementTimelineValue = retirementTimelineValue_;
    out.cancellationRequested = cancellationRequested_;
    out.staleGeneration = staleGeneration_;
    if (includeStages) {
        out.stages.reserve(stages_.size());
        for (size_t i = 0; i < stages_.size(); ++i) {
            const StageState& stage = stages_[i];
            out.stages.push_back({
                .index = i,
                .stage = stage.desc.stage,
                .state = stage.state,
                .estimatedCostMs = stage.desc.estimatedCostMs,
                .label = stage.desc.label,
            });
        }
    }
    return out;
}

void TopologyRebuildTicket::refreshState() {
    if (terminal(state_)) {
        return;
    }
    if (allBuildStagesComplete() && oldRendererRetained_ && !oldRendererRetired_) {
        state_ = TopologyRebuildTicketState::WaitingForRetirementFence;
        return;
    }
    if (allBuildStagesComplete() && (!oldRendererRetained_ || oldRendererRetired_)) {
        state_ = TopologyRebuildTicketState::Complete;
        return;
    }
    if (completedStageCount() > 0u) {
        state_ = TopologyRebuildTicketState::Building;
    } else {
        state_ = TopologyRebuildTicketState::Queued;
    }
}

size_t TopologyRebuildTicket::completedStageCount() const {
    return static_cast<size_t>(std::count_if(stages_.begin(), stages_.end(), [](const StageState& stage) {
        return stage.state == TopologyRebuildStageState::Complete;
    }));
}

size_t TopologyRebuildTicket::pendingStageCount() const {
    return static_cast<size_t>(std::count_if(stages_.begin(), stages_.end(), [](const StageState& stage) {
        return stage.state == TopologyRebuildStageState::Pending;
    }));
}

bool TopologyRebuildTicket::allBuildStagesComplete() const {
    return std::all_of(stages_.begin(), stages_.end(), [](const StageState& stage) {
        return !buildStage(stage.desc.stage) || stage.state == TopologyRebuildStageState::Complete;
    });
}

uint64_t TopologyRebuildTicketQueue::create(std::string label, std::vector<TopologyRebuildStageDesc> stages) {
    ++latestGeneration_;
    for (TopologyRebuildTicket& ticket : tickets_) {
        if (ticket.active() && ticket.generation() < latestGeneration_) {
            (void)ticket.cancelAsStale();
        }
    }
    const uint64_t id = nextTicketId_++;
    tickets_.emplace_back(id, latestGeneration_, std::move(label), std::move(stages));
    return id;
}

TopologyRebuildStepResult TopologyRebuildTicketQueue::stepFrame(const TopologyRebuildFrameBudget& budget) {
    TopologyRebuildStepResult combined;
    double remainingMs = std::max(0.0, budget.maxCpuMs);
    uint32_t remainingStages = budget.maxStages;
    for (TopologyRebuildTicket& ticket : tickets_) {
        if (!ticket.active()) {
            continue;
        }
        const TopologyRebuildFrameBudget perTicketBudget{
            .maxCpuMs = remainingMs,
            .maxStages = remainingStages,
        };
        const TopologyRebuildStepResult result = ticket.stepFrame(perTicketBudget, nextTimelineValue_);
        combined.completedStages += result.completedStages;
        combined.consumedMs += result.consumedMs;
        combined.budgetExhausted = combined.budgetExhausted || result.budgetExhausted;
        remainingMs = result.consumedMs >= remainingMs ? 0.0 : remainingMs - result.consumedMs;
        if (remainingStages != 0) {
            remainingStages = result.completedStages >= remainingStages ? 0u : remainingStages - static_cast<uint32_t>(result.completedStages);
        }
        if (remainingMs == 0.0 || remainingStages == 0) {
            combined.budgetExhausted = true;
            break;
        }
    }
    combined.ticketComplete = std::all_of(tickets_.begin(), tickets_.end(), [](const TopologyRebuildTicket& ticket) {
        return !ticket.active();
    });
    return combined;
}

bool TopologyRebuildTicketQueue::completeRetirementFence(uint64_t completedTimelineValue) {
    bool changed = false;
    for (TopologyRebuildTicket& ticket : tickets_) {
        changed = ticket.completeRetirementFence(completedTimelineValue) || changed;
    }
    return changed;
}

std::vector<TopologyRebuildTicketSnapshot> TopologyRebuildTicketQueue::snapshots(bool includeStages) const {
    std::vector<TopologyRebuildTicketSnapshot> out;
    out.reserve(tickets_.size());
    for (const TopologyRebuildTicket& ticket : tickets_) {
        out.push_back(ticket.snapshot(includeStages));
    }
    return out;
}

} // namespace rtv
