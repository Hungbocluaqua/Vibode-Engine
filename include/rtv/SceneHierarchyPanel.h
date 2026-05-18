#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class SceneHierarchyPanel {
public:
    void draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);

private:
    void drawImportedNode(const SceneAsset& scene, uint32_t nodeIndex, EditorSelection& selection);
};

} // namespace rtv
