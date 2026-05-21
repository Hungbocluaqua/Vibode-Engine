#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class SceneHierarchyPanel {
public:
    void draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);

private:
    bool entityContainsSelection(const SceneRegistry& registry, const Entity& entity, EntityId selected) const;
    bool entityContainsFilter(const SceneRegistry& registry, const Entity& entity, const std::string& filter) const;
    void drawEntityNode(SceneRegistry& registry, Entity& entity, EditorSelection& selection, EditorRequests& requests, const std::string& filter);
    void drawImportedNode(const SceneAsset& scene, uint32_t nodeIndex, EditorSelection& selection);

    EntityId lastScrolledSelection_{};
};

} // namespace rtv
