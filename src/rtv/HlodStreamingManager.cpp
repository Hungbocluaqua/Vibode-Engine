#include "rtv/HlodStreamingManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace rtv {

void HlodStreamingManager::upsertNode(const HlodNode& node) {
    // Remove existing node with same ID.
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
        [&node](const HlodNode& n) { return n.nodeId == node.nodeId; }),
        nodes_.end());
    nodes_.push_back(node);
}

void HlodStreamingManager::removeNode(uint64_t nodeId) {
    // Remove children first.
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
        [nodeId, this](const HlodNode& n) {
            return n.nodeId == nodeId || n.parentId == nodeId;
        }),
        nodes_.end());
}

void HlodStreamingManager::selectVisibleNodes(const std::array<float, 3>& cameraPosition,
                                                const std::array<float, 3>& cameraDirection,
                                                float fovYRadians, float screenHeight) {
    // Reset visibility.
    for (HlodNode& node : nodes_) {
        node.renderable = false;
    }

    // Recursive helper: evaluate a node and optionally refine its children.
    std::function<void(HlodNode&)> evaluateNode = [&](HlodNode& node) -> void {
        const float screenSize = computeScreenSize(node.boundsMin, node.boundsMax,
            cameraPosition, cameraDirection, fovYRadians, screenHeight);

        if (screenSize < node.minScreenSize) {
            // Too small — don't render.
            node.renderable = false;
        } else if (screenSize >= node.minScreenSize && screenSize < node.maxScreenSize && !node.childIds.empty()) {
            // Show this node's representation (proxy or detail).
            node.renderable = true;
        } else if (screenSize >= node.maxScreenSize && !node.childIds.empty()) {
            // Refine children — show them instead of this node.
            node.renderable = false;
            for (uint64_t childId : node.childIds) {
                HlodNode* child = const_cast<HlodNode*>(findNode(childId));
                if (child) {
                    evaluateNode(*child);
                }
            }
        } else {
            // Full detail or no children.
            node.renderable = true;
        }
    };

    // Evaluate all root nodes (nodes with no parent).
    for (HlodNode& node : nodes_) {
        if (node.parentId == 0) {
            evaluateNode(node);
        }
    }
}

void HlodStreamingManager::markResident(uint64_t nodeId, bool resident) {
    for (HlodNode& node : nodes_) {
        if (node.nodeId == nodeId) {
            node.resident = resident;
            return;
        }
    }
}

uint64_t HlodStreamingManager::createStreamingGroup(const std::string& label) {
    StreamingGroup group;
    group.groupId = nextGroupId_++;
    group.label = label;
    groups_.push_back(std::move(group));
    return groups_.back().groupId;
}

void HlodStreamingManager::addToStreamingGroup(uint64_t groupId, AssetGuid guid) {
    for (StreamingGroup& group : groups_) {
        if (group.groupId == groupId) {
            group.assetGuids.push_back(guid);
            return;
        }
    }
}

void HlodStreamingManager::markGroupResident(uint64_t groupId, bool allResident) {
    for (StreamingGroup& group : groups_) {
        if (group.groupId == groupId) {
            group.allResident = allResident;
            return;
        }
    }
}

const HlodNode* HlodStreamingManager::findNode(uint64_t nodeId) const {
    for (const HlodNode& node : nodes_) {
        if (node.nodeId == nodeId) return &node;
    }
    return nullptr;
}

const StreamingGroup* HlodStreamingManager::findGroup(uint64_t groupId) const {
    for (const StreamingGroup& group : groups_) {
        if (group.groupId == groupId) return &group;
    }
    return nullptr;
}

std::vector<const HlodNode*> HlodStreamingManager::visibleNodes() const {
    std::vector<const HlodNode*> visible;
    for (const HlodNode& node : nodes_) {
        if (node.renderable) {
            visible.push_back(&node);
        }
    }
    return visible;
}

float HlodStreamingManager::computeScreenSize(const std::array<float, 3>& boundsMin,
                                                const std::array<float, 3>& boundsMax,
                                                const std::array<float, 3>& cameraPos,
                                                const std::array<float, 3>& cameraDir,
                                                float fovYRadians, float screenHeight) const {
    // Approximate screen-space size using bounding sphere.
    const float cx = (boundsMin[0] + boundsMax[0]) * 0.5f;
    const float cy = (boundsMin[1] + boundsMax[1]) * 0.5f;
    const float cz = (boundsMin[2] + boundsMax[2]) * 0.5f;
    const float radius = std::sqrt(
        (boundsMax[0] - boundsMin[0]) * (boundsMax[0] - boundsMin[0]) +
        (boundsMax[1] - boundsMin[1]) * (boundsMax[1] - boundsMin[1]) +
        (boundsMax[2] - boundsMin[2]) * (boundsMax[2] - boundsMin[2])) * 0.5f;

    const float dx = cx - cameraPos[0];
    const float dy = cy - cameraPos[1];
    const float dz = cz - cameraPos[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (distance <= radius) return 1.0f;  // Inside the bounding sphere.

    // Project sphere onto screen.
    const float pixelSize = (radius / distance) * (screenHeight / (2.0f * std::tan(fovYRadians * 0.5f)));
    return pixelSize / screenHeight;
}

nlohmann::json hlodStreamingToJson(const std::vector<HlodNode>& nodes,
                                     const std::vector<StreamingGroup>& groups) {
    nlohmann::json nodesJson = nlohmann::json::array();
    for (const HlodNode& node : nodes) {
        nodesJson.push_back({
            {"node_id", node.nodeId},
            {"parent_id", node.parentId},
            {"bounds_min", {node.boundsMin[0], node.boundsMin[1], node.boundsMin[2]}},
            {"bounds_max", {node.boundsMax[0], node.boundsMax[1], node.boundsMax[2]}},
            {"min_screen_size", node.minScreenSize},
            {"max_screen_size", node.maxScreenSize},
            {"mesh_guid", node.meshGuid},
            {"proxy_guid", node.proxyGuid},
            {"resident", node.resident},
            {"renderable", node.renderable},
            {"streaming_group_id", node.streamingGroupId},
            {"label", node.label},
        });
    }

    nlohmann::json groupsJson = nlohmann::json::array();
    for (const StreamingGroup& group : groups) {
        groupsJson.push_back({
            {"group_id", group.groupId},
            {"label", group.label},
            {"asset_count", group.assetGuids.size()},
            {"all_resident", group.allResident},
        });
    }

    return nlohmann::json{
        {"nodes", nodesJson},
        {"groups", groupsJson},
    };
}

} // namespace rtv
