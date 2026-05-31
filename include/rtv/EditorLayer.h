#pragma once

#include "rtv/AssetBrowserPanel.h"
#include "rtv/CameraBookmark.h"
#include "rtv/DebugProfilerPanel.h"
#include "rtv/EditorDockspace.h"
#include "rtv/EditorPreferences.h"
#include "rtv/EditorSelection.h"
#include "rtv/GpuDiagnosticsPanel.h"
#include "rtv/InspectorPanel.h"
#include "rtv/MaterialEditorPanel.h"
#include "rtv/RenderSettingsPanel.h"
#include "rtv/SceneHierarchyPanel.h"
#include "rtv/SceneStatsPanel.h"
#include "rtv/ViewportPanel.h"

#include <array>

namespace rtv {

class EditorLayer {
public:
    EditorRequests draw(EditorRuntimeState& state);
    void resetLayout();
    void invalidateAssetThumbnails() { assetBrowserPanel_.invalidateThumbnails(); }

    [[nodiscard]] EditorPreferences& editorPrefs() { return editorPrefs_; }
    [[nodiscard]] CameraBookmarkManager& cameraBookmarks() { return cameraBookmarks_; }
    [[nodiscard]] VkExtent2D desiredRenderExtent(VkExtent2D fallback) const;
    [[nodiscard]] bool viewportInteractionActive() const;
    [[nodiscard]] bool viewportHovered() const;

private:
    void drawProjectManager(EditorRuntimeState& state, EditorRequests& requests);
    void drawRenderWorldSettingsPanel(EditorRuntimeState& state, EditorRequests& requests);
    void drawTimelinePanel();
    void drawLogPanel(const EditorRuntimeState& state);
    void drawConsolePanel();

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
    SceneStatsPanel sceneStatsPanel_{};
    GpuDiagnosticsPanel gpuDiagnosticsPanel_{};
    EditorPreferences editorPrefs_{};
    CameraBookmarkManager cameraBookmarks_{};
    bool showProjectManager_ = true;
    bool projectManagerDismissed_ = false;
    std::array<char, 128> newProjectName_{};
    std::array<char, 512> newProjectLocation_{};
    std::array<char, 512> openProjectPath_{};
    int newProjectTemplate_ = 1;
    bool createDefaultScene_ = true;
    bool createDefaultContentFolders_ = true;
};

} // namespace rtv
