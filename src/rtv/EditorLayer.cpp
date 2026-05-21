#include "rtv/EditorLayer.h"

namespace rtv {

EditorRequests EditorLayer::draw(EditorRuntimeState& state) {
    EditorRequests requests;
    if (state.sceneDocument != nullptr && state.sceneDocument->sourceGltfPath().has_value()) {
        dockspace_.setProfilePath(*state.sceneDocument->sourceGltfPath());
    } else if (state.gltfPath != nullptr && state.gltfPath->has_value()) {
        dockspace_.setProfilePath(**state.gltfPath);
    }
    dockspace_.begin(visibility_, requests);

    if (visibility_.viewport) {
        viewportPanel_.draw(state, selection_, requests);
    }
    if (visibility_.sceneHierarchy) {
        sceneHierarchyPanel_.draw(state, selection_, requests);
    }
    if (visibility_.inspector) {
        inspectorPanel_.draw(state, selection_, requests);
    }
    if (visibility_.assetBrowser) {
        assetBrowserPanel_.draw(state, selection_, requests);
    }
    if (visibility_.materialEditor) {
        materialEditorPanel_.draw(state, selection_, requests);
    }
    if (visibility_.renderSettings) {
        renderSettingsPanel_.draw(state, requests);
    }
    if (visibility_.debugProfiler) {
        debugProfilerPanel_.draw(state, requests);
    }

    dockspace_.end();
    return requests;
}

void EditorLayer::resetLayout() {
    dockspace_.requestResetLayout();
}

VkExtent2D EditorLayer::desiredRenderExtent(VkExtent2D fallback) const {
    return viewportPanel_.desiredRenderExtent(fallback);
}

bool EditorLayer::viewportInteractionActive() const {
    return viewportPanel_.interactionActive();
}

bool EditorLayer::viewportHovered() const {
    return viewportPanel_.hovered();
}

} // namespace rtv
