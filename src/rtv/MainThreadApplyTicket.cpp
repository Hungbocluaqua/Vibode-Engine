#include "rtv/MainThreadApplyTicket.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace rtv {
namespace {

[[nodiscard]] bool terminal(MainThreadApplyTicketState state) {
    return state == MainThreadApplyTicketState::Cancelled || state == MainThreadApplyTicketState::Complete || state == MainThreadApplyTicketState::Failed;
}

} // namespace

const char* mainThreadApplyOperationKindName(MainThreadApplyOperationKind kind) {
    switch (kind) {
    case MainThreadApplyOperationKind::EntityCreation: return "entityCreation";
    case MainThreadApplyOperationKind::EntityDeletion: return "entityDeletion";
    case MainThreadApplyOperationKind::EntityStateUpdate: return "entityStateUpdate";
    case MainThreadApplyOperationKind::ComponentCreation: return "componentCreation";
    case MainThreadApplyOperationKind::TransformUpdate: return "transformUpdate";
    case MainThreadApplyOperationKind::MaterialBinding: return "materialBinding";
    case MainThreadApplyOperationKind::MeshBinding: return "meshBinding";
    case MainThreadApplyOperationKind::DependencyRestore: return "dependencyRestore";
    case MainThreadApplyOperationKind::SelectionHandoff: return "selectionHandoff";
    }
    return "unknown";
}

const char* mainThreadApplyOperationStateName(MainThreadApplyOperationState state) {
    switch (state) {
    case MainThreadApplyOperationState::Pending: return "pending";
    case MainThreadApplyOperationState::Applied: return "applied";
    case MainThreadApplyOperationState::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* mainThreadApplyTicketStateName(MainThreadApplyTicketState state) {
    switch (state) {
    case MainThreadApplyTicketState::Queued: return "queued";
    case MainThreadApplyTicketState::Applying: return "applying";
    case MainThreadApplyTicketState::Cancelled: return "cancelled";
    case MainThreadApplyTicketState::Complete: return "complete";
    case MainThreadApplyTicketState::Failed: return "failed";
    }
    return "unknown";
}

MainThreadApplyTicket::MainThreadApplyTicket(uint64_t id, std::string label, std::vector<MainThreadApplyOperationDesc> operations)
    : id_(id), label_(std::move(label)) {
    operations_.reserve(operations.size());
    for (MainThreadApplyOperationDesc& desc : operations) {
        if (desc.entity != 0) {
            lockedEntities_.insert(desc.entity);
        }
        operations_.push_back(Operation{.desc = std::move(desc)});
    }
    if (operations_.empty()) {
        state_ = MainThreadApplyTicketState::Complete;
        undoSnapshotCommitted_ = true;
    }
}

bool MainThreadApplyTicket::active() const {
    return !terminal(state_);
}

bool MainThreadApplyTicket::ownsEntity(uint64_t entity) const {
    return entity != 0 && active() && lockedEntities_.find(entity) != lockedEntities_.end();
}

MainThreadApplyStepResult MainThreadApplyTicket::applyFrame(const MainThreadApplyFrameBudget& budget) {
    MainThreadApplyStepResult result;
    if (terminal(state_) || cancellationRequested_) {
        result.ticketComplete = state_ == MainThreadApplyTicketState::Complete;
        return result;
    }
    undoSnapshotOpen_ = true;
    state_ = MainThreadApplyTicketState::Applying;
    double remainingMs = std::max(0.0, budget.maxApplyMs);
    uint32_t remainingOperations = budget.maxOperations == 0 ? std::numeric_limits<uint32_t>::max() : budget.maxOperations;

    for (size_t operationIndex = 0; operationIndex < operations_.size(); ++operationIndex) {
        Operation& operation = operations_[operationIndex];
        if (operation.state != MainThreadApplyOperationState::Pending) {
            continue;
        }
        const double cost = std::max(0.0, operation.desc.estimatedCostMs);
        const bool oversizedFirstOperation = result.appliedOperations == 0 && cost > 0.0 && cost > remainingMs;
        if (remainingOperations == 0 || (cost > 0.0 && cost > remainingMs && !oversizedFirstOperation)) {
            result.budgetExhausted = true;
            break;
        }
        operation.state = MainThreadApplyOperationState::Applied;
        result.consumedMs += cost;
        ++result.appliedOperations;
        result.appliedOperationRecords.push_back(MainThreadApplyAppliedOperation{
            .ticketId = id_,
            .index = operationIndex,
            .desc = operation.desc,
        });
        remainingMs = cost >= remainingMs ? 0.0 : remainingMs - cost;
        --remainingOperations;
    }

    refreshState();
    result.ticketComplete = state_ == MainThreadApplyTicketState::Complete;
    return result;
}

bool MainThreadApplyTicket::requestCancel() {
    if (terminal(state_)) {
        return false;
    }
    cancellationRequested_ = true;
    for (Operation& operation : operations_) {
        if (operation.state == MainThreadApplyOperationState::Pending) {
            operation.state = MainThreadApplyOperationState::Cancelled;
        }
    }
    state_ = MainThreadApplyTicketState::Cancelled;
    undoSnapshotOpen_ = false;
    undoSnapshotCommitted_ = false;
    lockedEntities_.clear();
    return true;
}

MainThreadApplyTicketSnapshot MainThreadApplyTicket::snapshot(bool includeOperations) const {
    MainThreadApplyTicketSnapshot out;
    out.id = id_;
    out.state = state_;
    out.label = label_;
    out.operationCount = operations_.size();
    out.appliedOperations = appliedOperationCount();
    out.pendingOperations = pendingOperationCount();
    out.cancelledOperations = 0;
    for (const Operation& operation : operations_) {
        if (operation.state == MainThreadApplyOperationState::Cancelled) {
            ++out.cancelledOperations;
        }
    }
    out.progress = operations_.empty() ? 1.0f : static_cast<float>(out.appliedOperations) / static_cast<float>(operations_.size());
    out.undoSnapshotOpen = undoSnapshotOpen_;
    out.undoSnapshotCommitted = undoSnapshotCommitted_;
    out.cancellationRequested = cancellationRequested_;
    out.canCancel = active();
    out.lockedEntities.assign(lockedEntities_.begin(), lockedEntities_.end());
    std::sort(out.lockedEntities.begin(), out.lockedEntities.end());
    if (includeOperations) {
        out.operations.reserve(operations_.size());
        for (size_t i = 0; i < operations_.size(); ++i) {
            const Operation& operation = operations_[i];
            out.operations.push_back({
                .index = i,
                .kind = operation.desc.kind,
                .state = operation.state,
                .entity = operation.desc.entity,
                .estimatedCostMs = operation.desc.estimatedCostMs,
                .label = operation.desc.label,
            });
        }
    }
    return out;
}

void MainThreadApplyTicket::refreshState() {
    if (terminal(state_)) {
        return;
    }
    const bool anyPending = std::any_of(operations_.begin(), operations_.end(), [](const Operation& operation) {
        return operation.state == MainThreadApplyOperationState::Pending;
    });
    if (!anyPending) {
        state_ = MainThreadApplyTicketState::Complete;
        undoSnapshotOpen_ = false;
        undoSnapshotCommitted_ = true;
        lockedEntities_.clear();
    } else if (appliedOperationCount() > 0u) {
        state_ = MainThreadApplyTicketState::Applying;
    } else {
        state_ = MainThreadApplyTicketState::Queued;
    }
}

size_t MainThreadApplyTicket::appliedOperationCount() const {
    return static_cast<size_t>(std::count_if(operations_.begin(), operations_.end(), [](const Operation& operation) {
        return operation.state == MainThreadApplyOperationState::Applied;
    }));
}

size_t MainThreadApplyTicket::pendingOperationCount() const {
    return static_cast<size_t>(std::count_if(operations_.begin(), operations_.end(), [](const Operation& operation) {
        return operation.state == MainThreadApplyOperationState::Pending;
    }));
}

uint64_t MainThreadApplyTicketQueue::create(std::string label, std::vector<MainThreadApplyOperationDesc> operations) {
    std::unordered_set<uint64_t> requestedLocks;
    for (const MainThreadApplyOperationDesc& operation : operations) {
        if (operation.entity != 0) {
            requestedLocks.insert(operation.entity);
        }
    }
    for (uint64_t entity : requestedLocks) {
        if (entityLocked(entity)) {
            return 0;
        }
    }
    const uint64_t id = nextTicketId_++;
    tickets_.emplace_back(id, std::move(label), std::move(operations));
    return id;
}

MainThreadApplyStepResult MainThreadApplyTicketQueue::applyFrame(const MainThreadApplyFrameBudget& budget) {
    MainThreadApplyStepResult combined;
    double remainingMs = std::max(0.0, budget.maxApplyMs);
    uint32_t remainingOperations = budget.maxOperations;
    for (MainThreadApplyTicket& ticket : tickets_) {
        if (!ticket.active()) {
            continue;
        }
        const MainThreadApplyFrameBudget perTicketBudget{
            .maxApplyMs = remainingMs,
            .maxOperations = remainingOperations,
        };
        const MainThreadApplyStepResult result = ticket.applyFrame(perTicketBudget);
        combined.appliedOperations += result.appliedOperations;
        combined.consumedMs += result.consumedMs;
        combined.appliedOperationRecords.insert(combined.appliedOperationRecords.end(), result.appliedOperationRecords.begin(), result.appliedOperationRecords.end());
        combined.budgetExhausted = combined.budgetExhausted || result.budgetExhausted;
        remainingMs = result.consumedMs >= remainingMs ? 0.0 : remainingMs - result.consumedMs;
        if (remainingOperations != 0) {
            remainingOperations = result.appliedOperations >= remainingOperations ? 0u : remainingOperations - static_cast<uint32_t>(result.appliedOperations);
        }
        if (remainingMs == 0.0 || remainingOperations == 0) {
            combined.budgetExhausted = true;
            break;
        }
    }
    combined.ticketComplete = std::all_of(tickets_.begin(), tickets_.end(), [](const MainThreadApplyTicket& ticket) {
        return !ticket.active();
    });
    return combined;
}

bool MainThreadApplyTicketQueue::requestCancel(uint64_t id) {
    for (MainThreadApplyTicket& ticket : tickets_) {
        if (ticket.id() == id) {
            return ticket.requestCancel();
        }
    }
    return false;
}

bool MainThreadApplyTicketQueue::entityLocked(uint64_t entity) const {
    if (entity == 0) {
        return false;
    }
    return std::any_of(tickets_.begin(), tickets_.end(), [&](const MainThreadApplyTicket& ticket) {
        return ticket.ownsEntity(entity);
    });
}

std::vector<MainThreadApplyTicketSnapshot> MainThreadApplyTicketQueue::snapshots(bool includeOperations) const {
    std::vector<MainThreadApplyTicketSnapshot> out;
    out.reserve(tickets_.size());
    for (const MainThreadApplyTicket& ticket : tickets_) {
        out.push_back(ticket.snapshot(includeOperations));
    }
    return out;
}

} // namespace rtv
