#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AsyncSceneLoader.h"
#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"
#include "rtv/AssetImport.h"
#include "rtv/AssetManager.h"
#include "rtv/AnimationClip.h"
#include "rtv/CameraController.h"
#include "rtv/EditorPanels.h"
#include "rtv/GpuProfiler.h"
#include "rtv/SceneDocument.h"
#include "rtv/SceneEventBus.h"
#include "rtv/SceneToGpuSceneBuilder.h"
#include "rtv/NotificationManager.h"
#include "rtv/UndoStack.h"
#include "rtv/HeadlessDiagnostics.h"

#include <memory>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace rtv {

class CommandSystem;
class BufferUploader;
class ResourceAllocator;
class ResourceDemo;
class PipelineDemo;
class PathTracerRenderer;
class Swapchain;
class UiOverlay;
class UploadContext;
class VulkanContext;

class Application final : private NonCopyable {
public:
    explicit Application(
        RendererDebugView debugView = RendererDebugView::Beauty,
        std::optional<std::filesystem::path> gltfPath = std::nullopt,
        std::optional<std::filesystem::path> hdrPath = std::nullopt,
        std::optional<std::filesystem::path> scenePath = std::nullopt,
        std::optional<bool> denoiserOverride = std::nullopt,
        std::optional<RestirMode> restirModeOverride = std::nullopt,
        std::optional<RenderPreset> renderPresetOverride = std::nullopt,
        std::optional<bool> restirGiOverride = std::nullopt,
        std::optional<bool> opacityMicromapOverride = std::nullopt,
        std::optional<uint32_t> opacityMicromapSubdivisionOverride = std::nullopt,
        bool debugViewOverride = false,
        bool validationCameraMotion = false,
        bool validationObjectMotion = false,
        bool headless = false,
        bool disableAsyncCompute = false,
        bool singleQueueFallback = false,
        bool disableResourceAliasing = false);
    ~Application();

    void run(uint32_t maxFrames = 0);
    void runHeadless(uint32_t warmupFrames, uint32_t totalFrames);
    void renderFrames(uint32_t count);
    [[nodiscard]] bool runDescriptorLifetimeStress(
        const std::filesystem::path& outputPath,
        uint32_t cycles,
        uint32_t framesPerCycle);
    void resetDiagnosticFrameCounter(uint32_t frameIndex = 0);
    void setFrameCaptureCallbacks(std::function<void(uint32_t)> begin, std::function<void(uint32_t)> end);
    void resetAccumulation();
    void applyDebugView(RendererDebugView view);
    [[nodiscard]] bool applyNamedCamera(std::string_view cameraName);
    void onWindowFocusChanged(bool focused);
    void onFilesDropped(int count, const char** paths);

    [[nodiscard]] PathTracerRenderer* pathTracer() { return pathTracer_.get(); }
    [[nodiscard]] const VulkanContext* vulkanContext() const { return context_.get(); }
    [[nodiscard]] ResourceAllocator* resourceAllocator() { return allocator_.get(); }
    [[nodiscard]] BufferUploader* bufferUploader() { return uploader_.get(); }
    [[nodiscard]] UiOverlay* uiOverlay() { return uiOverlay_.get(); }
    [[nodiscard]] Swapchain* swapchain() { return swapchain_.get(); }
    [[nodiscard]] const std::vector<float>& cpuFrameTimings() const { return cpuFrameTimings_; }
    [[nodiscard]] const std::vector<float>& gpuFrameTimings() const { return gpuFrameTimings_; }
    [[nodiscard]] const std::vector<GpuFrameTimings>& perFrameGpuTimings() const { return perFrameGpuTimings_; }
    [[nodiscard]] uint32_t warmupFrameCount() const { return warmupFrameCount_; }

private:
    struct RetiredPathTracer {
        std::unique_ptr<PathTracerRenderer> renderer;
        uint64_t releaseFrame = 0;
    };
    enum class SunDragPhase {
        Idle,
        Armed,
        Dragging,
    };
    enum class DirtyScenePromptResult {
        Save,
        Discard,
        Cancel,
    };
    struct SunDragState {
        SunDragPhase phase = SunDragPhase::Idle;
        EntityId entity{};
        Transform originalTransform{};
        std::optional<SceneDocument> beforeDocument;
        double startMouseX = 0.0;
        double startMouseY = 0.0;
        double lastMouseX = 0.0;
        double lastMouseY = 0.0;
        float elevation = 0.97f;
        float azimuth = 0.0f;
        int previousCursorMode = 0;
        double armedTimeSeconds = 0.0;
        bool dragEligible = false;
        bool suppressOpenLevel = false;
    };
    enum class AsyncAssetImportKind {
        Import,
        Reimport,
    };
    struct AsyncAssetImportJob {
        AsyncAssetImportKind kind = AsyncAssetImportKind::Import;
        uint64_t serial = 0;
        AssetImportRequest request{};
        AssetImportWorkspace workspace{};
        AssetGuid assetGuid;
        AssetType originalType = AssetType::Unknown;
        bool placeAfterImport = false;
    };
    struct AsyncAssetImportProgress {
        mutable std::mutex mutex;
        float progress = 0.0f;
        std::string stage = "Queued";
    };
    struct ActiveAsyncAssetImportJob {
        AsyncAssetImportJob job{};
        std::shared_ptr<AsyncAssetImportProgress> progress;
        std::future<StagedAssetImportResult> future;
    };

    struct CookProjectResult {
        uint64_t serial = 0;
        std::filesystem::path projectFile;
        std::filesystem::path outputDir;
        std::filesystem::path manifestPath;
        std::filesystem::path validationReportPath;
        std::filesystem::path logPath;
        std::string commandLine;
        int exitCode = -1;
        double workerTotalMs = 0.0;
    };

    struct ActiveCookProjectJob {
        uint64_t serial = 0;
        std::filesystem::path projectFile;
        std::filesystem::path outputDir;
        std::filesystem::path manifestPath;
        std::filesystem::path validationReportPath;
        std::filesystem::path logPath;
        std::future<CookProjectResult> future;
    };

    void initWindow();
    void initVulkan();
    void mainLoop(uint32_t maxFrames);
    void applyValidationCameraMotion(uint32_t frameIndex);
    void applyValidationObjectMotion(uint32_t frameIndex);
    void updateAutosave(float deltaSeconds);
    bool writeAutosave();
    void writeCrashMarker(bool running);
    void serializeEditorSceneData();
    void deserializeEditorSceneData();
    void queueProjectThumbnailCapture();
    void captureProjectThumbnailIfReady();
    void processRuntimeControls(float deltaSeconds);
    void updateAnimationPlayers(float deltaSeconds);
    [[nodiscard]] std::filesystem::path assetResolutionRoot() const;
    [[nodiscard]] std::optional<std::filesystem::path> resolveAnimationClipPath(const AnimationPlayer& player) const;
    [[nodiscard]] const AnimationClip* animationClipForPlayer(const AnimationPlayer& player);
    void processSunDragControls(bool shortcutsBlocked, bool viewportHovered, bool viewportInteraction, bool ctrlDown);
    void beginSunDragArm(bool dragEligible);
    void startSunDrag(double mouseX, double mouseY);
    void updateSunDrag(double mouseX, double mouseY);
    void finishSunDrag(bool cancel);
    void updateWindowTitle(float seconds);
    void showMainWindowIfHidden();
    void toggleBorderlessFullscreen();
    void reloadGltfScene(const std::filesystem::path& path);
    bool requestSceneLoad(SceneLoadRequest request);
    void recordCompletedSceneLoadJob(const SceneLoadResult& result, bool success, bool cancelled, const std::string& status, const std::string& error = {}, const std::string& warning = {});
    void pollAsyncSceneLoad();
    bool applySceneLoadResult(SceneLoadResult&& result);
    bool applyReplacementSceneResult(SceneLoadResult&& result, bool sceneDirtyAfterApply);
    bool applyMergeSceneResult(SceneLoadResult&& result);
    void applyEditorRequests(const EditorRequests& requests, bool allowResourceRebuild);
    [[nodiscard]] DirtyScenePromptResult promptDirtySceneBefore(std::string_view action) const;
    [[nodiscard]] bool saveCurrentSceneForDirtyPrompt();
    [[nodiscard]] bool saveAllEditorState();
    [[nodiscard]] std::optional<AssetRecord> materialAssetRecordForMaterial(uint32_t materialId) const;
    [[nodiscard]] std::optional<uint32_t> loadedMaterialIndexForRecord(const AssetRecord& record) const;
    [[nodiscard]] bool writeMaterialAssetFile(const AssetRecord& record, const MaterialAsset& material, const std::filesystem::path& path, bool autosave);
    [[nodiscard]] bool saveDirtyMaterialAsset(const AssetGuid& guid, std::string& saved, std::string& failure);
    [[nodiscard]] bool saveDirtyMaterialAssets(std::vector<std::string>& saved, std::vector<std::string>& failures);
    [[nodiscard]] bool autosaveDirtyMaterialAssets();
    [[nodiscard]] bool restoreMaterialAssetAutosaves();
    [[nodiscard]] bool confirmDestructiveSceneAction(std::string_view action);
    [[nodiscard]] bool createProjectFromRequest(const CreateProjectRequest& request);
    [[nodiscard]] bool openProjectFromFile(const std::filesystem::path& projectFile, bool promptForDirtyScene);
    [[nodiscard]] bool deleteProjectFromRequest(const DeleteProjectRequest& request);
    [[nodiscard]] bool closeCurrentProject();
    [[nodiscard]] std::filesystem::path activeEditorPreferencesPath() const;
    void reloadEditorPreferencesForActiveProject();
    [[nodiscard]] bool saveActiveEditorPreferences();
    [[nodiscard]] bool loadProjectStartupScene(const ProjectContext& project);
    [[nodiscard]] bool writeDefaultProjectScene(const ProjectContext& project, std::string_view templateName);
    [[nodiscard]] std::optional<AssetImportWorkspace> prepareAssetImportWorkspace(const std::filesystem::path& sourcePath);
    [[nodiscard]] bool queueAssetImportNonMutating(const EditorImportAssetRequest& request, bool placeAfterImport);
    [[nodiscard]] bool placePrefabAsset(const AssetGuid& prefabGuid, const std::optional<Transform>& placementTransform = std::nullopt);
    [[nodiscard]] std::optional<uint32_t> loadedMeshIndexForRecord(const AssetRecord& record) const;
    [[nodiscard]] bool placeMeshAsset(const EditorMeshAssetPlacement& request);
    [[nodiscard]] bool assignMaterialAssetToEntity(const EditorMaterialAssetAssignment& request);
    [[nodiscard]] bool assignEnvironmentPath(const std::filesystem::path& environmentPath, bool allowResourceRebuild, std::string_view undoLabel, std::string_view notificationLabel);
    [[nodiscard]] bool assignEnvironmentAsset(const AssetGuid& environmentGuid, bool allowResourceRebuild);
    [[nodiscard]] bool relinkAssetSource(const EditorAssetRelinkSourceRequest& request);
    [[nodiscard]] bool replaceAssetReferences(const EditorReplaceAssetReferencesRequest& request, bool allowResourceRebuild);
    [[nodiscard]] bool renameAssetRecord(const EditorRenameAssetRequest& request);
    [[nodiscard]] bool updateAssetTags(const EditorAssetTagsRequest& request);
    [[nodiscard]] bool bulkAddAssetTag(const EditorBulkAssetTagRequest& request);
    [[nodiscard]] bool bulkRemoveAssetTag(const EditorBulkAssetTagRequest& request);
    [[nodiscard]] bool moveAssetsToFolder(const EditorMoveAssetsToFolderRequest& request);
    [[nodiscard]] bool deleteAssetsFromRegistry(const EditorDeleteAssetRequest& request);
    [[nodiscard]] bool startCookProject(const EditorCookProjectRequest& request);
    void pollCookProjectJob();
    [[nodiscard]] bool queueAssetReimport(const AssetGuid& assetGuid);
    void queueMergeScenes(std::vector<std::filesystem::path> paths);
    void startNextPendingMergeScene();
    void startNextAssetImportWorker();
    void pollAssetImportWorker();
    void waitForAssetImportWorker();
    [[nodiscard]] bool applyCompletedAssetImport(AsyncAssetImportJob&& job, StagedAssetImportResult&& result);
    [[nodiscard]] bool mergeSceneIntoCurrent(const std::filesystem::path& path, bool allowResourceRebuild);
    bool applyPendingSceneUpdate(bool allowResourceRebuild);
    void applyRendererSettingsSafely(const RendererSettings& settings, bool allowRenderResolutionChange);
    void reloadShadersFromEditor();
    void startEditorRenderJob(EditorRenderJobKind kind, const std::filesystem::path& renderOutputRoot, const EditorRenderRequest* request = nullptr);
    void prepareEditorRenderJobFrame();
    void updateEditorRenderJob(float deltaSeconds);
    void writeEditorRenderJobManifest(const char* eventLabel);
    void cancelEditorRenderJob(const std::filesystem::path& renderOutputRoot);
    [[nodiscard]] bool exportEditorRenderJobImage(const std::filesystem::path& outputPath);
    void restoreEditorRenderJobSceneState();
    void preparePathTracerForRendererReplacement(const RendererSettings& previousSettings);
    void retirePathTracer(std::unique_ptr<PathTracerRenderer> renderer);
    void releaseRetiredPathTracers();
    [[nodiscard]] std::optional<std::filesystem::path> currentSceneCachePathForRenderer() const;
    [[nodiscard]] std::unique_ptr<PathTracerRenderer> makePathTracer(
        const SceneAsset* sceneAsset,
        const AssetManager* assets,
        std::optional<std::filesystem::path> sceneCachePath,
        const RendererSettings* settingsToRestore);
    void createPathTracer(const RendererSettings* settingsToRestore = nullptr);
    void applyActiveSceneCamera();
    void syncActiveSceneCameraFromController();
    void rebuildGpuSceneAsset();
    void initializeFallbackSceneDocument();
    void initializeProjectManagerStartupSceneDocument();
    void initializeRendererFromCurrentScene(const RendererSettings* settingsToRestore = nullptr);
    [[nodiscard]] bool pressedOnce(int key);

    GLFWwindow* window_ = nullptr;
    bool mainWindowHiddenUntilRenderer_ = false;
    bool headless_ = false;
    uint32_t nextDiagnosticFrameIndex_ = 0;
    uint32_t warmupFrameCount_ = 0;
    uint32_t totalFrameCount_ = 0;
    std::vector<float> cpuFrameTimings_;
    std::vector<float> gpuFrameTimings_;
    std::vector<GpuFrameTimings> perFrameGpuTimings_;
    std::function<void(uint32_t)> beginFrameCapture_;
    std::function<void(uint32_t)> endFrameCapture_;
    RendererDebugView debugView_ = RendererDebugView::Beauty;
    std::optional<std::filesystem::path> gltfPath_;
    std::optional<std::filesystem::path> hdrPath_;
    std::optional<std::filesystem::path> scenePath_;
    bool sceneUnsavedDirty_ = false;
    bool projectSettingsDirty_ = false;
    std::optional<ProjectContext> project_;
    AssetRegistry assetRegistry_;
    std::optional<bool> denoiserOverride_;
    std::optional<RestirMode> restirModeOverride_;
    std::optional<RenderPreset> renderPresetOverride_;
    std::optional<bool> restirGiOverride_;
    std::optional<bool> opacityMicromapOverride_;
    std::optional<uint32_t> opacityMicromapSubdivisionOverride_;
    bool debugViewOverride_ = false;
    bool validationCameraMotion_ = false;
    bool validationObjectMotion_ = false;
    EntityId validationObjectMotionEntity_{};
    Transform validationObjectMotionBaseTransform_{};
    bool disableAsyncCompute_ = false;
    bool singleQueueFallback_ = false;
    bool disableResourceAliasing_ = false;
    bool pendingOpenLevel_ = false;
    bool pendingSaveLevel_ = false;
    bool pendingSaveAll_ = false;
    bool pendingReloadShaders_ = false;
    bool pendingUndo_ = false;
    bool pendingRedo_ = false;
    AssetManager assets_;
    std::unordered_map<std::string, AnimationClip> animationClipCache_;
    std::unordered_set<std::string> failedAnimationClipLoads_;
    CameraController cameraController_;
    SunDragState sunDrag_{};
    std::array<unsigned char, 512> keyState_{};
    float lastFrameSeconds_ = 0.0f;
    float autosaveElapsedSeconds_ = 0.0f;
    uint64_t frameSerial_ = 0;
    float lastTitleUpdateSeconds_ = -1.0f;
    bool borderlessFullscreen_ = false;
    int windowedX_ = 100;
    int windowedY_ = 100;
    int windowedWidth_ = 1280;
    int windowedHeight_ = 720;
    std::optional<SceneAsset> importedScene_;
    SceneDocument sceneDocument_;
    SceneEventBus sceneEventBus_;
    NotificationManager notifications_;
    EditorRenderJobStatus editorRenderJob_{};
    std::vector<std::filesystem::path> pendingDroppedFiles_;
    float editorRenderJobElapsedSeconds_ = 0.0f;
    uint64_t nextEditorRenderJobSerial_ = 1;
    uint32_t editorRenderJobFramesRendered_ = 0;
    bool editorRenderJobFramePrepared_ = false;
    int editorRenderJobSequenceStartFrame_ = 0;
    int editorRenderJobSequenceEndFrame_ = 0;
    uint32_t editorRenderJobSequenceFramesPerTimelineFrame_ = 1;
    uint32_t editorRenderJobSequenceOutputFramesWritten_ = 0;
    uint32_t editorRenderJobSequenceAccumulationFrame_ = 0;
    bool editorRenderJobTimelineWasPlaying_ = false;
    int editorRenderJobPreviousTimelineFrame_ = 0;
    std::optional<SceneDocument> editorRenderJobSceneSnapshot_;
    std::optional<RendererSettings> editorRenderJobSettingsSnapshot_;
    std::optional<RendererSettings> editorRenderJobAppliedSettings_;
    std::optional<EditorRenderRequest> editorRenderJobRequest_;
    std::vector<std::filesystem::path> editorRenderJobOutputFiles_;
    EditorPlacementStatus editorPlacement_{};
    uint64_t nextEditorPlacementSerial_ = 1;
    uint64_t nextSceneLoadJobSerial_ = 1;
    EditorJobCenterState completedSceneLoadJob_{};
    uint64_t nextAssetImportJobSerial_ = 1;
    EditorJobCenterState completedAssetImportJob_{};
    uint64_t nextCookProjectJobSerial_ = 1;
    EditorJobCenterState completedCookProjectJob_{};
    UndoStack undoStack_;
    SceneToGpuSceneBuilder sceneBuilder_;
    std::optional<SceneAsset> gpuSceneAsset_;
    std::vector<EntityId> gpuInstanceEntities_;
    std::unique_ptr<VulkanContext> context_;
    std::unique_ptr<ResourceAllocator> allocator_;
    std::unique_ptr<UploadContext> uploadContext_;
    std::unique_ptr<BufferUploader> uploader_;
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<CommandSystem> commandSystem_;
    std::unique_ptr<UiOverlay> uiOverlay_;
    std::unique_ptr<ResourceDemo> resourceDemo_;
    std::unique_ptr<PipelineDemo> pipelineDemo_;
    std::unique_ptr<PathTracerRenderer> pathTracer_;
    std::vector<RetiredPathTracer> retiredPathTracers_;
    std::optional<RendererSettings> pendingPostFrameSettings_;
    AsyncSceneLoader asyncSceneLoader_;
    std::deque<std::filesystem::path> pendingMergeScenes_;
    std::deque<AsyncAssetImportJob> pendingAssetImportJobs_;
    std::optional<ActiveAsyncAssetImportJob> activeAssetImportJob_;
    std::optional<ActiveCookProjectJob> activeCookProjectJob_;
    std::optional<SceneLoadRequest> activeSceneLoadRequest_;
    std::optional<std::filesystem::path> pendingRecoveryScenePath_;
    std::optional<std::filesystem::path> pendingRecoveryAutosavePath_;
    std::optional<std::filesystem::path> pendingRecoveryProjectAutosavePath_;
    std::optional<std::filesystem::path> pendingProjectThumbnailPath_;
    uint64_t pendingProjectThumbnailFrame_ = 0;
    uint32_t pendingProjectThumbnailAttempts_ = 0;
    std::string sceneLoadingStatus_;
    std::unordered_map<AssetGuid, MaterialAsset> dirtyMaterialAssets_;
    std::unordered_map<AssetGuid, std::filesystem::path> materialAssetAutosavePaths_;
    std::vector<std::pair<AssetGuid, std::filesystem::path>> pendingRecoveryMaterialAssetAutosaves_;
    std::optional<std::filesystem::path> pendingRecoveryAssetRegistryAutosavePath_;
};

} // namespace rtv
