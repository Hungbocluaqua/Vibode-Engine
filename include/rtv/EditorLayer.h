#pragma once

#include "rtv/AssetBrowserPanel.h"
#include "rtv/CameraBookmark.h"
#include "rtv/DebugProfilerPanel.h"
#include "rtv/EditorDockspace.h"
#include "rtv/EditorLog.h"
#include "rtv/EditorPreferences.h"
#include "rtv/EditorSelection.h"
#include "rtv/EditorTimeline.h"
#include "rtv/GpuDiagnosticsPanel.h"
#include "rtv/InspectorPanel.h"
#include "rtv/MaterialEditorPanel.h"
#include "rtv/NotificationManager.h"
#include "rtv/RenderSettingsPanel.h"
#include "rtv/SceneHierarchyPanel.h"
#include "rtv/SceneStatsPanel.h"
#include "rtv/ViewportPanel.h"

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rtv {

class EditorLayer {
public:
    EditorRequests draw(EditorRuntimeState& state);
    EditorRequests drawProjectManagerLauncher(ProjectManagerRuntimeState state);
    void resetLayout();
    void showProjectManager() { showProjectManager_ = true; projectManagerDismissed_ = false; }
    void dismissProjectManager() { showProjectManager_ = false; projectManagerDismissed_ = true; }
    void handleNotificationAction(NotificationAction action, EditorRequests& requests);
    void showRecoveryPrompt(std::filesystem::path markerPath, std::filesystem::path autosavePath, std::filesystem::path projectAutosavePath = {});
    void invalidateAssetThumbnails() { assetBrowserPanel_.invalidateThumbnails(); }
    void clearSelection() { selection_.clear(); }

    [[nodiscard]] EditorPreferences& editorPrefs() { return editorPrefs_; }
    [[nodiscard]] CameraBookmarkManager& cameraBookmarks() { return cameraBookmarks_; }
    [[nodiscard]] EditorLog& log() { return log_; }
    [[nodiscard]] EditorTimeline& timeline() { return timeline_; }
    [[nodiscard]] VkExtent2D desiredRenderExtent(VkExtent2D fallback) const;
    [[nodiscard]] bool viewportInteractionActive() const;
    [[nodiscard]] bool viewportHovered() const;

private:
    void drawProjectManager(const ProjectManagerRuntimeState& state, EditorRequests& requests);
    void drawRecoveryPrompt(EditorRequests& requests);
    void drawSceneLoadingOverlay(const EditorRuntimeState& state, EditorRequests& requests);
    void drawRenderJobModal(const EditorRuntimeState& state, EditorRequests& requests);
    void drawRenderWorldSettingsPanel(EditorRuntimeState& state, EditorRequests& requests);
    void drawTimelinePanel(EditorRuntimeState& state, EditorRequests& requests);
    void drawLogPanel(const EditorRuntimeState& state, EditorRequests& requests);
    void drawConsolePanel(EditorRuntimeState& state, EditorRequests& requests);
    void updateJobCenterHistory(const EditorRuntimeState& state);
    void drawJobCenterPanel(const EditorRuntimeState& state, EditorRequests& requests);
    void drawJobHistoryEntry(size_t index, EditorRequests& requests);
    void drawCommandPalette(EditorRuntimeState& state, EditorRequests& requests);
    void applyCaptureFocusOverride();
    bool executeCommandPaletteCommand(EditorCommandId id, EditorRuntimeState& state, EditorRequests& requests);
    bool executeConsoleCommand(std::string command, EditorRuntimeState& state, EditorRequests& requests);
    void applyThemePreset();
    void applyWorkspacePreset();

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
    EditorLog log_{};
    EditorTimeline timeline_{};
    std::vector<uint64_t> timelineSelectedKeyIds_{};
    bool timelineDraggingKeys_ = false;
    float timelineDragStartMouseX_ = 0.0f;
    std::vector<std::pair<uint64_t, int>> timelineDragStartFrames_{};
    std::vector<std::string> consoleHistory_{};
    struct EditorJobHistoryEntry {
        uint64_t id = 0;
        uint64_t serial = 0;
        std::string key;
        std::string title;
        std::string kind;
        std::string status;
        float progress = 0.0f;
        bool active = false;
        bool completed = false;
        bool failed = false;
        bool cancelled = false;
        bool hidden = false;
        std::filesystem::path sourcePath;
        std::filesystem::path outputRoot;
        std::filesystem::path manifestPath;
        std::filesystem::path reportPath;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        double workerTotalMs = 0.0;
        double workerSceneParseMs = 0.0;
        double workerGltfLoadMs = 0.0;
        double workerDocumentBuildMs = 0.0;
        double importValidateMs = 0.0;
        double importDirectoryMs = 0.0;
        double importInspectMs = 0.0;
        double importWriteMs = 0.0;
        bool hasAssetImportRetry = false;
        bool assetImportPlaceAfterImport = false;
        EditorImportAssetRequest assetImportRetry{};
        AssetGuid assetReimportGuid;
    };
    std::vector<EditorJobHistoryEntry> jobHistory_{};
    uint64_t nextJobHistoryId_ = 1;
    bool observedSceneLoadRunningForHistory_ = false;
    uint64_t activeSceneLoadHistorySerial_ = 0;
    uint64_t observedSceneLoadResultSerial_ = 0;
    bool observedAssetImportRunningForHistory_ = false;
    uint64_t activeAssetImportHistorySerial_ = 0;
    uint64_t observedAssetImportResultSerial_ = 0;
    std::string activeSceneLoadHistoryKey_{};
    std::string activeAssetImportHistoryKey_{};
    std::string lastSceneLoadHistoryStatus_{};
    std::string lastAssetImportHistoryTitle_{};
    std::string lastAssetImportHistoryStatus_{};
    float lastSceneLoadHistoryProgress_ = 0.0f;
    float lastAssetImportHistoryProgress_ = 0.0f;
    bool commandPaletteOpen_ = false;
    std::array<char, 128> commandPaletteSearch_{};
    bool commandPaletteShortcutEditor_ = false;
    bool captureFocusOverrideInitialized_ = false;
    std::string captureFocusWindow_{};
    int captureFocusFramesRemaining_ = 0;
    bool recoveryPromptVisible_ = false;
    bool renderJobModalOpen_ = false;
    uint64_t observedRenderJobSerial_ = 0;
    uint64_t observedPlacementSerial_ = 0;
    std::filesystem::path recoveryMarkerPath_;
    std::filesystem::path recoveryAutosavePath_;
    std::filesystem::path recoveryProjectAutosavePath_;
    bool showProjectManager_ = true;
    bool projectManagerDismissed_ = false;
    int projectManagerSection_ = 0;
    std::array<char, 128> newProjectName_{};
    std::array<char, 512> newProjectLocation_{};
    std::array<char, 512> openProjectPath_{};
    int newProjectTemplate_ = 1;
    bool createDefaultScene_ = true;
    bool createDefaultContentFolders_ = true;
    int appliedThemePreset_ = -1;
    int appliedWorkspacePreset_ = -1;
};

} // namespace rtv
