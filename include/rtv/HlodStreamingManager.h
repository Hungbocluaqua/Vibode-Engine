#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AssetRegistry.h"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

// HLOD (Hierarchical Level of Detail) node in a streaming group.
struct HlodNode {
    // Stable identifier for this HLOD node.
    uint64_t nodeId = 0;

    // Parent node ID (0 for root).
    uint64_t parentId = 0;

    // World-space bounding box.
    std::array<float, 3> boundsMin = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> boundsMax = {0.0f, 0.0f, 0.0f};

    // Screen-size thresholds for detail selection.
    float minScreenSize = 0.0f;   // Below this, use parent's proxy.
    float maxScreenSize = 1.0f;   // Above this, fully refine children.

    // Asset GUID for this node's mesh (or empty if group-only).
    AssetGuid meshGuid;

    // Proxy GUID for distant rendering (impostor or decimated mesh).
    AssetGuid proxyGuid;

    // Child HLOD nodes (refined when screen size > minScreenSize).
    std::vector<uint64_t> childIds;

    // Streaming group for "must appear together" assets.
    uint64_t streamingGroupId = 0;

    // Whether this node is currently resident.
    bool resident = false;

    // Whether this node should be rendered (visible + appropriate LOD).
    bool renderable = false;

    // Custom label for debugging.
    std::string label;
};

struct StreamingGroup {
    uint64_t groupId = 0;
    std::string label;
    std::vector<AssetGuid> assetGuids;  // Must stream together.
    bool allResident = false;
};

// Manages HLOD streaming for world-scale scenes.
//
// Key behaviors:
// - Per-prefab root bounds and per-subtree bounds.
// - HLOD proxy meshes for distant viewing.
// - Distance-based mesh selection.
// - Screen-size-based mip selection.
// - Streaming groups for co-residency requirements.
// - Optional impostor cards for foliage clusters.
class HlodStreamingManager final : private NonCopyable {
public:
    HlodStreamingManager() = default;

    // Add or update an HLOD node.
    void upsertNode(const HlodNode& node);

    // Remove an HLOD node and its children.
    void removeNode(uint64_t nodeId);

    // Select visible nodes based on camera position and screen size.
    void selectVisibleNodes(const std::array<float, 3>& cameraPosition,
                            const std::array<float, 3>& cameraDirection,
                            float fovYRadians, float screenHeight);

    // Mark a node as resident (its mesh and proxy are GPU-resident).
    void markResident(uint64_t nodeId, bool resident);

    // Create a streaming group for co-residency.
    uint64_t createStreamingGroup(const std::string& label);

    // Add an asset to a streaming group.
    void addToStreamingGroup(uint64_t groupId, AssetGuid guid);

    // Mark a streaming group as fully resident.
    void markGroupResident(uint64_t groupId, bool allResident);

    // Query.
    [[nodiscard]] const HlodNode* findNode(uint64_t nodeId) const;
    [[nodiscard]] const StreamingGroup* findGroup(uint64_t groupId) const;
    [[nodiscard]] std::vector<const HlodNode*> visibleNodes() const;

    [[nodiscard]] const std::vector<HlodNode>& nodes() const { return nodes_; }
    [[nodiscard]] const std::vector<StreamingGroup>& groups() const { return groups_; }

private:
    float computeScreenSize(const std::array<float, 3>& boundsMin,
                            const std::array<float, 3>& boundsMax,
                            const std::array<float, 3>& cameraPos,
                            const std::array<float, 3>& cameraDir,
                            float fovYRadians, float screenHeight) const;

    std::vector<HlodNode> nodes_;
    std::vector<StreamingGroup> groups_;
    uint64_t nextNodeId_ = 1;
    uint64_t nextGroupId_ = 1;
};

[[nodiscard]] nlohmann::json hlodStreamingToJson(
    const std::vector<HlodNode>& nodes,
    const std::vector<StreamingGroup>& groups);

} // namespace rtv
