#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

enum class TopologyRebuildStage : uint8_t {
    CpuSceneExtraction,
    GpuSceneBufferBuild,
    BufferUploads,
    TextureUploads,
    BlasBuildBatch,
    TlasBuildOrRefit,
    RendererDescriptorUpdate,
    FinalRendererSwap,
    RetireOldRenderer,
};

enum class TopologyRebuildStageState : uint8_t {
    Pending,
    Running,
    Complete,
    Cancelled,
};

enum class TopologyRebuildTicketState : uint8_t {
    Queued,
    Building,
    WaitingForRetirementFence,
    Cancelled,
    Complete,
    Failed,
};

struct TopologyRebuildStageDesc {
    TopologyRebuildStage stage = TopologyRebuildStage::CpuSceneExtraction;
    double estimatedCostMs = 1.0;
    std::string label;
};

struct TopologyRebuildFrameBudget {
    double maxCpuMs = 4.0;
    uint32_t maxStages = 0;
};

struct TopologyRebuildStageSnapshot {
    size_t index = 0;
    TopologyRebuildStage stage = TopologyRebuildStage::CpuSceneExtraction;
    TopologyRebuildStageState state = TopologyRebuildStageState::Pending;
    double estimatedCostMs = 0.0;
    std::string label;
};

struct TopologyRebuildTicketSnapshot {
    uint64_t id = 0;
    uint64_t generation = 0;
    TopologyRebuildTicketState state = TopologyRebuildTicketState::Queued;
    std::string label;
    size_t stageCount = 0;
    size_t pendingStages = 0;
    size_t completedStages = 0;
    size_t cancelledStages = 0;
    float progress = 0.0f;
    bool previousRendererVisible = true;
    bool finalRendererSwapped = false;
    bool oldRendererRetained = false;
    bool oldRendererRetired = false;
    uint64_t retirementTimelineValue = 0;
    bool cancellationRequested = false;
    bool staleGeneration = false;
    std::vector<TopologyRebuildStageSnapshot> stages;
};

struct TopologyRebuildStepResult {
    size_t completedStages = 0;
    double consumedMs = 0.0;
    bool budgetExhausted = false;
    bool ticketComplete = false;
};

[[nodiscard]] const char* topologyRebuildStageName(TopologyRebuildStage stage);
[[nodiscard]] const char* topologyRebuildStageStateName(TopologyRebuildStageState state);
[[nodiscard]] const char* topologyRebuildTicketStateName(TopologyRebuildTicketState state);

class TopologyRebuildTicket final {
public:
    TopologyRebuildTicket() = default;
    TopologyRebuildTicket(uint64_t id, uint64_t generation, std::string label, std::vector<TopologyRebuildStageDesc> stages);

    [[nodiscard]] uint64_t id() const { return id_; }
    [[nodiscard]] uint64_t generation() const { return generation_; }
    [[nodiscard]] TopologyRebuildTicketState state() const { return state_; }
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool staleGeneration() const { return staleGeneration_; }

    [[nodiscard]] TopologyRebuildStepResult stepFrame(const TopologyRebuildFrameBudget& budget, uint64_t& nextTimelineValue);
    [[nodiscard]] bool cancelAsStale();
    [[nodiscard]] bool completeRetirementFence(uint64_t completedTimelineValue);
    [[nodiscard]] TopologyRebuildTicketSnapshot snapshot(bool includeStages = true) const;

private:
    struct StageState {
        TopologyRebuildStageDesc desc{};
        TopologyRebuildStageState state = TopologyRebuildStageState::Pending;
    };

    void refreshState();
    [[nodiscard]] size_t completedStageCount() const;
    [[nodiscard]] size_t pendingStageCount() const;
    [[nodiscard]] bool allBuildStagesComplete() const;

    uint64_t id_ = 0;
    uint64_t generation_ = 0;
    std::string label_;
    TopologyRebuildTicketState state_ = TopologyRebuildTicketState::Queued;
    bool previousRendererVisible_ = true;
    bool finalRendererSwapped_ = false;
    bool oldRendererRetained_ = false;
    bool oldRendererRetired_ = false;
    bool cancellationRequested_ = false;
    bool staleGeneration_ = false;
    uint64_t retirementTimelineValue_ = 0;
    std::vector<StageState> stages_;
};

class TopologyRebuildTicketQueue final {
public:
    [[nodiscard]] uint64_t create(std::string label, std::vector<TopologyRebuildStageDesc> stages);
    [[nodiscard]] TopologyRebuildStepResult stepFrame(const TopologyRebuildFrameBudget& budget);
    [[nodiscard]] bool completeRetirementFence(uint64_t completedTimelineValue);
    [[nodiscard]] std::vector<TopologyRebuildTicketSnapshot> snapshots(bool includeStages = true) const;
    [[nodiscard]] uint64_t latestGeneration() const { return latestGeneration_; }
    [[nodiscard]] uint64_t nextTimelineValue() const { return nextTimelineValue_; }

private:
    uint64_t nextTicketId_ = 1;
    uint64_t latestGeneration_ = 0;
    uint64_t nextTimelineValue_ = 1;
    std::vector<TopologyRebuildTicket> tickets_;
};

} // namespace rtv
