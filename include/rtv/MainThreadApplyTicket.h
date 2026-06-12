#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace rtv {

enum class MainThreadApplyOperationKind : uint8_t {
    EntityCreation,
    EntityDeletion,
    EntityStateUpdate,
    ComponentCreation,
    TransformUpdate,
    MaterialBinding,
    MeshBinding,
    DependencyRestore,
    SelectionHandoff,
};

enum class MainThreadApplyOperationState : uint8_t {
    Pending,
    Applied,
    Cancelled,
};

enum class MainThreadApplyTicketState : uint8_t {
    Queued,
    Applying,
    Cancelled,
    Complete,
    Failed,
};

struct MainThreadApplyOperationDesc {
    MainThreadApplyOperationKind kind = MainThreadApplyOperationKind::EntityCreation;
    uint64_t entity = 0;
    double estimatedCostMs = 0.25;
    std::string label;
};

struct MainThreadApplyFrameBudget {
    double maxApplyMs = 2.0;
    uint32_t maxOperations = 0;
};

struct MainThreadApplyOperationSnapshot {
    size_t index = 0;
    MainThreadApplyOperationKind kind = MainThreadApplyOperationKind::EntityCreation;
    MainThreadApplyOperationState state = MainThreadApplyOperationState::Pending;
    uint64_t entity = 0;
    double estimatedCostMs = 0.0;
    std::string label;
};

struct MainThreadApplyAppliedOperation {
    uint64_t ticketId = 0;
    size_t index = 0;
    MainThreadApplyOperationDesc desc{};
};

struct MainThreadApplyTicketSnapshot {
    uint64_t id = 0;
    MainThreadApplyTicketState state = MainThreadApplyTicketState::Queued;
    std::string label;
    size_t operationCount = 0;
    size_t pendingOperations = 0;
    size_t appliedOperations = 0;
    size_t cancelledOperations = 0;
    float progress = 0.0f;
    bool undoSnapshotOpen = false;
    bool undoSnapshotCommitted = false;
    bool cancellationRequested = false;
    bool canCancel = false;
    std::vector<uint64_t> lockedEntities;
    std::vector<MainThreadApplyOperationSnapshot> operations;
};

struct MainThreadApplyStepResult {
    size_t appliedOperations = 0;
    double consumedMs = 0.0;
    bool budgetExhausted = false;
    bool ticketComplete = false;
    std::vector<MainThreadApplyAppliedOperation> appliedOperationRecords;
};

[[nodiscard]] const char* mainThreadApplyOperationKindName(MainThreadApplyOperationKind kind);
[[nodiscard]] const char* mainThreadApplyOperationStateName(MainThreadApplyOperationState state);
[[nodiscard]] const char* mainThreadApplyTicketStateName(MainThreadApplyTicketState state);

class MainThreadApplyTicket final {
public:
    MainThreadApplyTicket() = default;
    MainThreadApplyTicket(uint64_t id, std::string label, std::vector<MainThreadApplyOperationDesc> operations);

    [[nodiscard]] uint64_t id() const { return id_; }
    [[nodiscard]] MainThreadApplyTicketState state() const { return state_; }
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool ownsEntity(uint64_t entity) const;
    [[nodiscard]] const std::unordered_set<uint64_t>& lockedEntities() const { return lockedEntities_; }

    [[nodiscard]] MainThreadApplyStepResult applyFrame(const MainThreadApplyFrameBudget& budget);
    [[nodiscard]] bool requestCancel();
    [[nodiscard]] MainThreadApplyTicketSnapshot snapshot(bool includeOperations = true) const;

private:
    struct Operation {
        MainThreadApplyOperationDesc desc{};
        MainThreadApplyOperationState state = MainThreadApplyOperationState::Pending;
    };

    void refreshState();
    [[nodiscard]] size_t appliedOperationCount() const;
    [[nodiscard]] size_t pendingOperationCount() const;

    uint64_t id_ = 0;
    std::string label_;
    MainThreadApplyTicketState state_ = MainThreadApplyTicketState::Queued;
    bool undoSnapshotOpen_ = false;
    bool undoSnapshotCommitted_ = false;
    bool cancellationRequested_ = false;
    std::unordered_set<uint64_t> lockedEntities_;
    std::vector<Operation> operations_;
};

class MainThreadApplyTicketQueue final {
public:
    [[nodiscard]] uint64_t create(std::string label, std::vector<MainThreadApplyOperationDesc> operations);
    [[nodiscard]] MainThreadApplyStepResult applyFrame(const MainThreadApplyFrameBudget& budget);
    [[nodiscard]] bool requestCancel(uint64_t id);
    [[nodiscard]] bool entityLocked(uint64_t entity) const;
    [[nodiscard]] std::vector<MainThreadApplyTicketSnapshot> snapshots(bool includeOperations = true) const;

private:
    uint64_t nextTicketId_ = 1;
    std::vector<MainThreadApplyTicket> tickets_;
};

} // namespace rtv
