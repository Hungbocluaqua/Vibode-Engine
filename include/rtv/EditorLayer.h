#pragma once

#include "rtv/AssetBrowserPanel.h"
#include "rtv/DebugProfilerPanel.h"
#include "rtv/EditorDockspace.h"
#include "rtv/EditorSelection.h"
#include "rtv/InspectorPanel.h"
#include "rtv/MaterialEditorPanel.h"
#include "rtv/RenderSettingsPanel.h"
#include "rtv/SceneHierarchyPanel.h"
#include "rtv/ViewportPanel.h"

namespace rtv {

class EditorLayer {
public:
    EditorRequests draw(EditorRuntimeState& state);
    void resetLayout();

    [[nodiscard]] VkExtent2D desiredRenderExtent(VkExtent2D fallback) const;
    [[nodiscard]] bool viewportInteractionActive() const;
    [[nodiscard]] bool viewportHovered() const;

private:
    EditorPanelVisibility visibility_{};
    EditorSelection selection_{};
    EditorDockspace dockspace_{};
    ViewportPanel viewportPanel_{};
    SceneHierarchyPanel sceneHierarchyPanel_{};
    InspectorPanel inspectorPanel_{};
    AssetBrowserPanel assetBrowserPanel_{};
    MaterialEditorPanel materialEditorPanel_{};
    RenderSettingsPanel renderSettingsPanel_{};
    DebugProfilerPanel debugProfilerPanel_{};
};

} // namespace rtv
