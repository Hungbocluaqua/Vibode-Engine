#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AsyncSceneLoader.h"
#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"
#include "rtv/AssetImport.h"
#include "rtv/AssetManager.h"
#include "rtv/AnimationClip.h"
#include "rtv/AnimationController.h"
#include "rtv/CameraController.h"
#include "rtv/EditorPanels.h"
#include "rtv/GpuSceneStreamingState.h"
#include "rtv/GpuProfiler.h"
#include "rtv/IncrementalGpuSceneUpdateQueue.h"
#include "rtv/SceneDocument.h"
#include "rtv/SceneEventBus.h"
#include "rtv/SceneToGpuSceneBuilder.h"
#include "rtv/NotificationManager.h"
#include "rtv/Prefab.h"
#include "rtv/NativeGpuAssetCache.h"
#include "rtv/AnimationStreamingManager.h"
#include "rtv/GeometryPagingManager.h"
#include "rtv/HlodStreamingManager.h"
#include "rtv/MaterialStreamingManager.h"
#include "rtv/PreviewProxyManager.h"
#include "rtv/ProgressiveCookManager.h"
#include "rtv/StreamingGpuTransferExecutor.h"
#include "rtv/StreamingGpuWorkQueue.h"
#include "rtv/StreamingIoBackend.h"
#include "rtv/StreamingAsyncComputeBudgeter.h"
#include "rtv/StreamingDebugOverlay.h"
#include "rtv/StreamingRuntime.h"
#include "rtv/TextureStreamingManager.h"
#include "rtv/UndoStack.h"
#include "rtv/HeadlessDiagnostics.h"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <array>
#include <chrono>
#include <cstddef>
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
#include <utility>
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
struct RendererOnlyRequests;
struct NativeRuntimeLoadReport;
class UiOverlay;
class UploadContext;
class VulkanContext;
struct NativeRuntimeLoadReport;

enum class ApplicationMode : uint8_t {
    Editor,
    RendererOnly,
    Headless,
};

struct NativePackageAnimationSelection {
    std::string controllerGuid;
    std::filesystem::path controllerPath;
    std::string entityName;
    uint64_t entityUuid = 0;
};

class Application final : private NonCopyable {
public:
    explicit Application(
        RendererDebugView debugView = RendererDebugView::Beauty,
        std::optional<std::filesystem::path> gltfPath = std::nullopt,
        std::optional<std::filesystem::path> hdrPath = std::nullopt,
        std::optional<std::filesystem::path> scenePath = std::nullopt,
        std::optional<std::filesystem::path> nativePackageScenePath = std::nullopt,
        NativePackageAnimationSelection nativePackageAnimationSelection = {},
        std::optional<bool> denoiserOverride = std::nullopt,
        std::optional<RestirMode> restirModeOverride = std::nullopt,
        std::optional<RenderPreset> renderPresetOverride = std::nullopt,
        std::optional<bool> restirGiOverride = std::nullopt,
        std::optional<bool> opacityMicromapOverride = std::nullopt,
        std::optional<bool> opacityMicromapBlendOverride = std::nullopt,
        std::optional<bool> hardwareBackfaceCullingOverride = std::nullopt,
        std::optional<uint32_t> opacityMicromapSubdivisionOverride = std::nullopt,
        bool debugViewOverride = false,
        bool validationCameraMotion = false,
        bool validationObjectMotion = false,
        bool validationLightReorder = false,
        bool validationLightFlicker = false,
        bool headless = false,
        ApplicationMode mode = ApplicationMode::Editor,
        uint32_t headlessWidth = 1280,
        uint32_t headlessHeight = 720,
        bool disableAsyncCompute = false,
        bool singleQueueFallback = false,
        bool disableResourceAliasing = false,
        StreamingRuntimeOptions streamingOptions = {});
    ~Application();

    void run(uint32_t maxFrames = 0, uint32_t warmupFrames = 0, bool collectProfile = false);
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
    void configureCaptureReady(uint32_t afterFrames, bool log);
    void setRendererOnlyLingerAfterCaptureReadyMs(uint32_t milliseconds);
    void setCaptureReadyFilePath(std::optional<std::filesystem::path> path);
    void setSavePresentFramePath(std::optional<std::filesystem::path> path);
    void setSavePresentFrameOnHotkeyPath(std::optional<std::filesystem::path> path);
    bool savePresentFrame(const std::filesystem::path& path);
    void dumpRendererOnlyProfileJson(const std::filesystem::path& path);
    void exportRendererOnlyDebugViews(const std::filesystem::path& dir);
    void printCaptureReadyMarker();
    [[nodiscard]] nlohmann::json textureDiagnosticsJson() const;
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
    [[nodiscard]] std::vector<GpuUploadTicketSnapshot> editorGpuUploadTicketSnapshots(bool includeChunks = false) const;
    [[nodiscard]] uint64_t editorGpuUploadNextTimelineValue() const;
    [[nodiscard]] std::vector<MainThreadApplyTicketSnapshot> editorMainThreadApplyTicketSnapshots(bool includeOperations = false) const;
    [[nodiscard]] std::vector<TopologyRebuildTicketSnapshot> editorTopologyRebuildTicketSnapshots(bool includeStages = false) const;
    [[nodiscard]] uint64_t editorTopologyRebuildLatestGeneration() const { return editorTopologyRebuildTickets_.latestGeneration(); }
    [[nodiscard]] uint64_t editorTopologyRebuildNextTimelineValue() const { return editorTopologyRebuildTickets_.nextTimelineValue(); }
    [[nodiscard]] nlohmann::json streamingRuntimeReport() const;
    [[nodiscard]] FrameWorkSchedulerSnapshot frameWorkSchedulerSnapshot() const { return frameWorkScheduler_.snapshot(); }
    [[nodiscard]] const AnimatedGeometryStats& latestAnimatedGeometryStats() const { return latestAnimatedGeometryStats_; }
    [[nodiscard]] const std::vector<GpuSkinningInstancePlan>& latestGpuSkinningPlan() const { return latestGpuSkinningPlan_; }
    [[nodiscard]] const std::vector<glm::mat4>& latestGpuSkinningJointMatrices() const { return latestGpuSkinningJointMatrices_; }
    [[nodiscard]] const std::vector<glm::mat4>& latestGpuSkinningPreviousJointMatrices() const { return latestGpuSkinningPreviousJointMatrices_; }
    [[nodiscard]] const std::vector<PathTracerRenderer::GpuSkinningSourceVertex>& latestGpuSkinningSourceVertices() const { return latestGpuSkinningSourceVertices_; }
    [[nodiscard]] const std::vector<GpuLocalVertex>& latestGpuSkinningMorphDeltas() const { return latestGpuSkinningMorphDeltas_; }

private:
    struct UsdRuntimeScenePlacementResult {
        size_t cameraCount = 0;
        size_t lightCount = 0;
        size_t hierarchyEntityCount = 0;
    };

    struct UsdRuntimeMeshHierarchyPlacementResult {
        size_t meshCount = 0;
        size_t transformCount = 0;
        size_t hierarchyEntityCount = 0;
    };

    struct RetiredPathTracer {
        std::unique_ptr<PathTracerRenderer> renderer;
        uint64_t releaseFrame = 0;
    };
    struct MountedNativePackageWatch {
        std::filesystem::path packagePath;
        std::filesystem::file_time_type lastWriteTime{};
        uint32_t textureCount = 0;
        uint32_t materialCount = 0;
        uint32_t meshCount = 0;
        uint64_t generation = 0;
        bool changeDetected = false;
        std::filesystem::file_time_type detectedWriteTime{};
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
        std::chrono::steady_clock::time_point workerStartedAt{};
        std::chrono::steady_clock::time_point stageStartedAt{};
    };
    struct ActiveAsyncAssetImportJob {
        AsyncAssetImportJob job{};
        std::shared_ptr<AsyncAssetImportProgress> progress;
        std::future<StagedAssetImportResult> future;
    };

    enum class LiveMainThreadApplyOperationKind : uint8_t {
        ImportAsset,
        ImportAndPlaceAsset,
        MergeScene,
        PlacePrefabAsset,
        PlaceMeshAsset,
        PlaceMeshScatterAssets,
        CreateEntity,
        DuplicateEntity,
        DeleteEntity,
        DeleteEntities,
        RenameEntity,
        ReparentEntity,
        SetEntityVisibility,
        SetEntityLocked,
        SetEntityTransform,
        SetEntityTransforms,
        SetMeshRenderer,
        AddComponent,
        RemoveComponent,
        SetLight,
        SetSun,
        SetCamera,
        UpdateMaterial,
        AssignMaterial,
        AssignMaterialAsset,
        AlignDistributeEntities,
        AssignEnvironmentPath,
        AssignEnvironmentAsset,
        ApplySceneSnapshot,
        EnsurePrimarySun,
        TogglePrimarySun,
        UpdateAssetTags,
        RenameAsset,
        BulkAddAssetTag,
        BulkRemoveAssetTag,
        MoveAssetsToFolder,
        DeleteAssets,
        ReimportAsset,
        RelinkAssetSource,
        ReplaceAssetReferences,
        RepairMissingAssetDependencies,
        MountNativePackage,
        UnloadNativePackage,
        RefreshNativePackage,
        UpdateTimeline,
        UpdateProjectSettings,
        MarkSceneUpdate,
        ApplyRendererSettings,
        ToggleDenoiser,
        ToggleDebugView,
        CycleIntermediateView,
        RestoreRecoveryAutosaves,
        DiscardRecovery,
    };

    struct LiveMainThreadApplyOperation {
        LiveMainThreadApplyOperationKind kind = LiveMainThreadApplyOperationKind::ImportAsset;
        EditorImportAssetRequest importRequest{};
        std::filesystem::path scenePath;
        AssetGuid prefabGuid;
        std::optional<Transform> prefabPlacementTransform;
        EditorMeshAssetPlacement meshPlacement{};
        EditorMeshScatterPlacement meshScatterPlacement{};
        EditorEntityCreateRequest entityCreateRequest{};
        EntityId duplicateEntity{};
        EntityId deleteEntity{};
        std::vector<EntityId> deleteEntities;
        EditorEntityRenameRequest renameEntity{};
        std::pair<EntityId, EntityId> reparentEntity{};
        EditorEntityBoolChange entityBoolChange{};
        EditorEntityTransformChange entityTransform{};
        EditorEntityTransformBatchChange entityTransforms{};
        EditorMeshRendererChange meshRendererChange{};
        EditorComponentRequest componentRequest{};
        EditorLightChange lightChange{};
        EditorSunChange sunChange{};
        EditorCameraChange cameraChange{};
        EditorMaterialUpdate materialUpdate{};
        EditorMaterialAssignment materialAssignment{};
        EditorMaterialAssetAssignment materialAssetAssignment{};
        EditorAlignDistributeRequest alignDistributeRequest{};
        std::filesystem::path environmentPath;
        AssetGuid environmentGuid;
        EditorSceneSnapshotChange sceneSnapshotChange{};
        EditorAssetTagsRequest assetTagsRequest{};
        EditorRenameAssetRequest renameAssetRequest{};
        EditorBulkAssetTagRequest bulkAssetTagRequest{};
        EditorMoveAssetsToFolderRequest moveAssetsRequest{};
        EditorDeleteAssetRequest deleteAssetsRequest{};
        AssetGuid assetGuid;
        EditorAssetRelinkSourceRequest relinkAssetSourceRequest{};
        EditorReplaceAssetReferencesRequest replaceAssetReferencesRequest{};
        EditorRepairMissingAssetDependenciesRequest repairMissingDependenciesRequest{};
        EditorNativePackageMountRequest mountNativePackageRequest{};
        EditorNativePackageUnloadRequest unloadNativePackageRequest{};
        EditorNativePackageRefreshRequest refreshNativePackageRequest{};
        nlohmann::json timelineJson;
        ProjectContext projectSettingsUpdate{};
        SceneUpdateKind sceneUpdateKind = SceneUpdateKind::None;
        RendererSettings rendererSettings{};
        bool executed = false;
    };

    struct LiveMainThreadApplyBatch {
        uint64_t ticketId = 0;
        std::string label;
        std::vector<LiveMainThreadApplyOperation> operations;
    };

    struct CookProjectResult {
        uint64_t serial = 0;
        std::filesystem::path projectFile;
        std::filesystem::path outputDir;
        std::filesystem::path manifestPath;
        std::filesystem::path validationReportPath;
        std::filesystem::path logPath;
        NativeTextureFormatSupport nativeTextureFormatSupport;
        bool emitNativeTextureTargetSets = false;
        EditorNativeTextureTargetSetProfile nativeTextureTargetSetProfile = EditorNativeTextureTargetSetProfile::ActiveAndAllBc;
        EditorNativeTextureTargetSetCustomProfile customNativeTextureTargetSet{};
        std::vector<EditorNativeTextureTargetSetLibraryProfile> customNativeTextureTargetSetLibrary;
        std::string packageTextureTargetSetJson;
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
        bool emitNativeTextureTargetSets = false;
        EditorNativeTextureTargetSetProfile nativeTextureTargetSetProfile = EditorNativeTextureTargetSetProfile::ActiveAndAllBc;
        EditorNativeTextureTargetSetCustomProfile customNativeTextureTargetSet{};
        std::vector<EditorNativeTextureTargetSetLibraryProfile> customNativeTextureTargetSetLibrary;
        std::future<CookProjectResult> future;
    };

    struct NativeFileMigrationWorkerProgress {
        mutable std::mutex mutex;
        float progress = 0.0f;
        std::string stage = "Queued";
        std::chrono::steady_clock::time_point workerStartedAt{};
        std::chrono::steady_clock::time_point stageStartedAt{};
    };

    struct ActiveNativeFileMigrationJob {
        EditorNativeFileMigrationJobRequest request{};
        std::shared_ptr<NativeFileMigrationWorkerProgress> progress;
        std::future<EditorNativeFileMigrationJobResult> future;
    };

    struct StreamedMaterialTextureBinding {
        uint32_t slot = 0;
        AssetGuid textureGuid;
    };

    struct ProgressiveRuntimeLoadBatchResult {
        uint64_t serial = 0;
        uint32_t firstFile = 0;
        uint32_t fileCount = 0;
        uint32_t loadedFiles = 0;
        uint64_t bytes = 0;
        double elapsedMs = 0.0;
        AssetManager assets;
        PrefabRuntimeBindings bindings;
        std::unordered_map<AssetGuid, TextureAssetHandle> textures;
        std::unordered_map<AssetGuid, std::vector<StreamedMaterialTextureBinding>> materialTextureBindings;
        std::vector<std::filesystem::path> files;
        std::vector<std::string> errors;
        StreamingIoMetrics ioMetrics{};
        std::vector<std::string> ioErrors;
        bool ok = false;
    };

    struct ActiveProgressiveRuntimeLoadJob {
        enum class State : uint8_t {
            Loading,       // Batches in progress — all topology rebuilds blocked.
            Completing,    // Last batch applied — about to trigger final rebuild.
            FinalRebuild,  // Final rebuild in flight — bypass guard for this one call.
            Done,          // Streaming complete — guard removed.
        };

        uint64_t serial = 0;
        State state = State::Loading;
        AssetGuid rootGuid;
        EntityId rootEntity{};
        std::string label;
        AssetRecord record{};
        std::filesystem::path root;
        std::filesystem::path nativeLoadRoot;
        std::vector<std::filesystem::path> files;
        uint64_t totalBytes = 0;
        uint64_t loadedBytes = 0;
        uint64_t appliedBytes = 0;
        uint32_t nextFile = 0;
        uint32_t loadedFiles = 0;
        uint32_t failedFiles = 0;
        uint32_t reboundRenderers = 0;
        std::unordered_map<AssetGuid, std::vector<StreamedMaterialTextureBinding>> materialTextureBindings;
        bool cancelled = false;
        bool failed = false;
        bool batchInFlight = false;
        std::future<ProgressiveRuntimeLoadBatchResult> future;
    };

    void initWindow();
    void initVulkan();
    void mainLoop(uint32_t maxFrames);
    void applyValidationCameraMotion(uint32_t frameIndex);
    void applyValidationObjectMotion(uint32_t frameIndex);
    void applyValidationLightReorder(uint32_t frameIndex);
    void applyValidationLightFlicker(uint32_t frameIndex);
    void updateAutosave(float deltaSeconds);
    bool writeAutosave();
    void writeCrashMarker(bool running);
    void serializeEditorSceneData();
    void deserializeEditorSceneData();
    void queueProjectThumbnailCapture();
    void captureProjectThumbnailIfReady();
    void processRuntimeControls(float deltaSeconds);
    void updateFrameWorkAccelerationStructureBudgetFeedback(const GpuFrameTimings& timings);
    void updateAnimationPlayers(float deltaSeconds);
    [[nodiscard]] bool attachNativePackageAnimationPlayer(const NativeRuntimeLoadReport& loadReport);
    [[nodiscard]] std::filesystem::path assetResolutionRoot() const;
    [[nodiscard]] std::optional<std::filesystem::path> resolveAnimationClipPath(const AnimationPlayer& player) const;
    [[nodiscard]] std::optional<std::filesystem::path> resolveAnimationControllerPath(const AnimationPlayer& player) const;
    [[nodiscard]] const AnimationClip* animationClipForPlayer(const AnimationPlayer& player);
    [[nodiscard]] const AnimationController* animationControllerForPlayer(const AnimationPlayer& player);
    [[nodiscard]] const AnimationClip* controllerClipForPlayer(
        AnimationPlayer& player,
        const AnimationController& controller,
        std::vector<AnimationController::Event>* routedEvents = nullptr,
        const AnimationClip** blendToClip = nullptr,
        float* blendAlpha = nullptr);
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
    [[nodiscard]] bool mountProjectStartupNativePackage(AssetManager& assets);
    [[nodiscard]] bool mountNativePackageFromEditor(const std::filesystem::path& packagePath);
    [[nodiscard]] bool unloadNativePackageFromEditor(const std::filesystem::path& packagePath);
    [[nodiscard]] bool refreshNativePackageFromEditor(const std::filesystem::path& packagePath);
    void rememberMountedNativePackage(const std::filesystem::path& packagePath, const NativeRuntimeLoadReport& loadReport);
    void forgetMountedNativePackage(const std::filesystem::path& packagePath);
    void pollMountedNativePackageChanges(const EditorRequests& requests);
    [[nodiscard]] bool writeDefaultProjectScene(const ProjectContext& project, std::string_view templateName);
    [[nodiscard]] std::optional<AssetImportWorkspace> prepareAssetImportWorkspace(const std::filesystem::path& sourcePath);
    [[nodiscard]] bool queueAssetImportNonMutating(const EditorImportAssetRequest& request, bool placeAfterImport);
    [[nodiscard]] bool placePrefabAsset(const AssetGuid& prefabGuid, const std::optional<Transform>& placementTransform = std::nullopt);
    [[nodiscard]] bool queueProgressiveRuntimeLoadForPrefab(
        const AssetRecord& prefabRecord,
        const PrefabAsset& prefab,
        const std::filesystem::path& root,
        EntityId rootEntity,
        const std::vector<std::filesystem::path>& files,
        uint64_t totalBytes);
    void pollProgressiveRuntimeLoadJob();
    void startNextProgressiveRuntimeLoadBatch();
    void applyProgressiveRuntimeLoadBatch(ProgressiveRuntimeLoadBatchResult&& result);
    [[nodiscard]] std::optional<uint32_t> loadedMeshIndexForRecord(const AssetRecord& record) const;
    [[nodiscard]] bool placeMeshAsset(const EditorMeshAssetPlacement& request, bool deferSceneUpdate = false);
    [[nodiscard]] std::optional<UsdRuntimeMeshHierarchyPlacementResult> placeUsdRuntimeMeshHierarchy(
        const AssetRecord& sceneRecord,
        const std::filesystem::path& root,
        const std::vector<EditorMeshAssetPlacement>& meshPlacements);
    [[nodiscard]] std::optional<UsdRuntimeScenePlacementResult> placeUsdRuntimeSceneEntities(const AssetRecord& sceneRecord, const std::filesystem::path& root);
    [[nodiscard]] bool placeMeshScatterAssets(const EditorMeshScatterPlacement& request);
    [[nodiscard]] bool createEntityFromEditor(const EditorEntityCreateRequest& request);
    [[nodiscard]] bool duplicateEntityFromEditor(EntityId entity);
    [[nodiscard]] bool deleteEntityFromEditor(EntityId entity);
    [[nodiscard]] bool deleteEntitiesFromEditor(const std::vector<EntityId>& entities);
    [[nodiscard]] bool renameEntityFromEditor(const EditorEntityRenameRequest& request);
    [[nodiscard]] bool reparentEntityFromEditor(EntityId child, EntityId newParent);
    [[nodiscard]] bool setEntityVisibilityFromEditor(const EditorEntityBoolChange& request);
    [[nodiscard]] bool setEntityLockedFromEditor(const EditorEntityBoolChange& request);
    [[nodiscard]] bool setEntityTransformFromEditor(const EditorEntityTransformChange& request);
    [[nodiscard]] bool setEntityTransformsFromEditor(const EditorEntityTransformBatchChange& request);
    [[nodiscard]] bool setMeshRendererFromEditor(const EditorMeshRendererChange& request);
    [[nodiscard]] bool addComponentFromEditor(const EditorComponentRequest& request);
    [[nodiscard]] bool removeComponentFromEditor(const EditorComponentRequest& request);
    [[nodiscard]] bool setLightFromEditor(const EditorLightChange& request);
    [[nodiscard]] bool setSunFromEditor(const EditorSunChange& request);
    [[nodiscard]] bool setCameraFromEditor(const EditorCameraChange& request);
    void clearDeletedEditorEntityState(EntityId entity);
    [[nodiscard]] bool alignDistributeEntitiesFromEditor(const EditorAlignDistributeRequest& request);
    [[nodiscard]] bool updateMaterialFromEditor(const EditorMaterialUpdate& request);
    [[nodiscard]] bool assignMaterialFromEditor(const EditorMaterialAssignment& request);
    [[nodiscard]] bool assignMaterialAssetToEntity(const EditorMaterialAssetAssignment& request);
    [[nodiscard]] bool assignEnvironmentPathFromEditor(const std::filesystem::path& environmentPath, bool allowResourceRebuild);
    [[nodiscard]] bool assignEnvironmentAssetFromEditor(const AssetGuid& environmentGuid, bool allowResourceRebuild);
    [[nodiscard]] bool applySceneSnapshotFromEditor(const EditorSceneSnapshotChange& request);
    [[nodiscard]] bool ensurePrimarySunFromEditor();
    [[nodiscard]] bool togglePrimarySunFromEditor(bool allowResourceRebuild);
    [[nodiscard]] bool updateTimelineFromEditor(const nlohmann::json& timelineJson);
    [[nodiscard]] bool updateProjectSettingsFromEditor(const ProjectContext& updatedProject);
    [[nodiscard]] bool markSceneUpdateFromEditor(SceneUpdateKind updateKind, bool allowResourceRebuild);
    [[nodiscard]] bool applyRendererSettingsFromEditor(const RendererSettings& settings, bool allowRenderResolutionChange);
    [[nodiscard]] bool toggleDenoiserFromEditor(bool allowRenderResolutionChange);
    [[nodiscard]] bool toggleDebugViewFromEditor(bool allowRenderResolutionChange);
    [[nodiscard]] bool cycleIntermediateViewFromEditor(bool allowRenderResolutionChange);
    [[nodiscard]] bool restoreRecoveryAutosavesFromEditor();
    [[nodiscard]] bool discardRecoveryFromEditor();
    [[nodiscard]] bool assignEnvironmentPath(const std::filesystem::path& environmentPath, bool allowResourceRebuild, std::string_view undoLabel, std::string_view notificationLabel);
    [[nodiscard]] bool assignEnvironmentAsset(const AssetGuid& environmentGuid, bool allowResourceRebuild);
    [[nodiscard]] bool relinkAssetSource(const EditorAssetRelinkSourceRequest& request);
    [[nodiscard]] bool replaceAssetReferences(const EditorReplaceAssetReferencesRequest& request, bool allowResourceRebuild);
    [[nodiscard]] bool repairMissingAssetDependencies(const EditorRepairMissingAssetDependenciesRequest& request);
    [[nodiscard]] bool renameAssetRecord(const EditorRenameAssetRequest& request);
    [[nodiscard]] bool updateAssetTags(const EditorAssetTagsRequest& request);
    [[nodiscard]] bool bulkAddAssetTag(const EditorBulkAssetTagRequest& request);
    [[nodiscard]] bool bulkRemoveAssetTag(const EditorBulkAssetTagRequest& request);
    [[nodiscard]] bool moveAssetsToFolder(const EditorMoveAssetsToFolderRequest& request);
    [[nodiscard]] bool deleteAssetsFromRegistry(const EditorDeleteAssetRequest& request);
    [[nodiscard]] bool startCookProject(const EditorCookProjectRequest& request);
    void pollCookProjectJob();
    [[nodiscard]] bool startNativeFileMigrationJob(EditorNativeFileMigrationJobRequest request);
    void startNextNativeFileMigrationWorker();
    void pollNativeFileMigrationJob();
    void recordCompletedNativeFileMigrationJob(const EditorNativeFileMigrationJobResult& result);
    [[nodiscard]] bool queueAssetReimport(const AssetGuid& assetGuid);
    void queueMergeScenes(std::vector<std::filesystem::path> paths);
    [[nodiscard]] bool queueLiveMainThreadApplyBatch(std::string label, std::vector<LiveMainThreadApplyOperation> operations);
    void executeLiveMainThreadApplyOperations(const MainThreadApplyStepResult& applyResult);
    void startNextPendingMergeScene();
    void startNextAssetImportWorker();
    void pollAssetImportWorker();
    void waitForAssetImportWorker();
    [[nodiscard]] bool applyCompletedAssetImport(AsyncAssetImportJob&& job, StagedAssetImportResult&& result);
    [[nodiscard]] bool mergeSceneIntoCurrent(const std::filesystem::path& path, bool allowResourceRebuild);
    bool applyPendingSceneUpdate(bool allowResourceRebuild, bool interactiveLightPreview = false);
    void applyRendererSettingsSafely(const RendererSettings& settings, bool allowRenderResolutionChange);
    void reloadShadersFromEditor();
    void initializeEditorTicketProbeQueues();
    void stepEditorTicketProbeQueues();
    void configureStreamingAsyncComputeBudgeter();
    void shutdownStreamingRuntime();
    void stepStreamingGpuWorkQueue();
    void stepStreamingGpuSceneUpdateQueue();
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
    [[nodiscard]] SceneCachePolicy currentSceneCachePolicyForRenderer() const;
    [[nodiscard]] std::unique_ptr<PathTracerRenderer> makePathTracer(
        const SceneAsset* sceneAsset,
        const AssetManager* assets,
        SceneCachePolicy sceneCachePolicy,
        const RendererSettings* settingsToRestore,
        uint32_t materialTextureMaxDimension = 0);
    void createPathTracer(const RendererSettings* settingsToRestore = nullptr);
    void applyActiveSceneCamera();
    void syncActiveSceneCameraFromController();
    void rebuildGpuSceneAsset(const RendererSettings* settingsOverride = nullptr);
    void processRendererOnlyRequests(const RendererOnlyRequests& requests);
    void updateCaptureReadyState(uint32_t frameNumber);
    [[nodiscard]] std::string activeCaptureSceneName() const;
    [[nodiscard]] bool rebuildRendererAfterNativePackageUnload(bool affectedActiveRenderer, nlohmann::json& report);
    void initializeFallbackSceneDocument();
    void initializeProjectManagerStartupSceneDocument();
    void initializeRendererFromCurrentScene(const RendererSettings* settingsToRestore = nullptr);
    [[nodiscard]] bool pressedOnce(int key);

    GLFWwindow* window_ = nullptr;
    ApplicationMode mode_ = ApplicationMode::Editor;
    bool mainWindowHiddenUntilRenderer_ = false;
    bool headless_ = false;
    bool rendererOnly_ = false;
    VkExtent2D headlessExtent_{1280, 720};
    uint32_t captureReadyAfterFrames_ = 60;
    bool captureReadyLog_ = false;
    bool captureReadyPrinted_ = false;
    uint32_t captureReadyRenderedFrames_ = 0;
    uint64_t captureReadyFrameSerial_ = 0;
    uint32_t rendererOnlyLingerAfterCaptureReadyMs_ = 0;
    std::chrono::steady_clock::time_point captureReadyPrintedAt_{};
    std::optional<std::filesystem::path> captureReadyFilePath_;
    uint64_t captureReadyImageUploadCount_ = 0;
    bool captureReadyUploadSnapshotValid_ = false;
    std::optional<std::filesystem::path> savePresentFramePath_;
    std::optional<std::filesystem::path> savePresentFrameOnHotkeyPath_;
    bool savePresentFrameHotkeyDown_ = false;
    bool initialPresentFrameSaveComplete_ = false;
    uint32_t nextDiagnosticFrameIndex_ = 0;
    uint32_t warmupFrameCount_ = 0;
    uint32_t totalFrameCount_ = 0;
    bool interactiveProfileCollectionEnabled_ = false;
    std::vector<float> cpuFrameTimings_;
    std::vector<float> gpuFrameTimings_;
    std::vector<GpuFrameTimings> perFrameGpuTimings_;
    std::function<void(uint32_t)> beginFrameCapture_;
    std::function<void(uint32_t)> endFrameCapture_;
    RendererDebugView debugView_ = RendererDebugView::Beauty;
    std::optional<std::filesystem::path> gltfPath_;
    std::optional<std::filesystem::path> hdrPath_;
    std::optional<std::filesystem::path> scenePath_;
    std::optional<std::filesystem::path> nativePackageScenePath_;
    NativePackageAnimationSelection nativePackageAnimationSelection_{};
    std::vector<MountedNativePackageWatch> mountedNativePackages_;
    uint64_t nativePackageWatchGeneration_ = 1;
    bool sceneUnsavedDirty_ = false;
    bool projectSettingsDirty_ = false;
    std::optional<ProjectContext> project_;
    AssetRegistry assetRegistry_;
    std::optional<bool> denoiserOverride_;
    std::optional<RestirMode> restirModeOverride_;
    std::optional<RenderPreset> renderPresetOverride_;
    std::optional<bool> restirGiOverride_;
    std::optional<bool> opacityMicromapOverride_;
    std::optional<bool> opacityMicromapBlendOverride_;
    std::optional<bool> hardwareBackfaceCullingOverride_;
    std::optional<uint32_t> opacityMicromapSubdivisionOverride_;
    bool debugViewOverride_ = false;
    bool validationCameraMotion_ = false;
    bool validationObjectMotion_ = false;
    bool validationLightReorder_ = false;
    bool validationLightReorderApplied_ = false;
    bool validationLightFlicker_ = false;
    bool validationLightFlickerUnavailableLogged_ = false;
    EntityId validationObjectMotionEntity_{};
    Transform validationObjectMotionBaseTransform_{};
    bool disableAsyncCompute_ = false;
    bool singleQueueFallback_ = false;
    bool disableResourceAliasing_ = false;
    StreamingRuntimeOptions streamingOptions_{};
    NativeAssetCatalog nativeAssetCatalog_;
    NativeGpuAssetCache nativeGpuAssetCache_;
    NativeGpuAssetEvictionResult lastStreamingEviction_{};
    std::deque<std::pair<uint64_t, NativeGpuAssetEvictionResult>> streamingEvictionHistory_;
    StreamingGpuWorkQueue streamingGpuWorkQueue_;
    StreamingGpuTransferExecutor streamingGpuTransferExecutor_;
    StreamingAsyncComputeBudgeter streamingAsyncComputeBudgeter_;
    AnimationStreamingManager animationStreamingManager_;
    GeometryPagingManager geometryPagingManager_;
    HlodStreamingManager hlodStreamingManager_;
    PreviewProxyManager previewProxyManager_;
    ProgressiveCookManager progressiveCookManager_;
    StreamingDebugOverlay streamingDebugOverlay_;
    MaterialStreamingManager materialStreamingManager_;
    TextureStreamingManager textureStreamingManager_;
    bool streamingGpuTransferExecutorReady_ = false;
    bool streamingGpuTransferExecutorInitAttempted_ = false;
    bool streamingGpuMarkerOnlyCompletionEventEmitted_ = false;
    struct StreamingGpuBufferUploadPayload {
        AssetGuid ownerGuid;
        uint64_t destinationOffset = 0;
        std::vector<uint8_t> bytes;
    };
    struct StreamingGpuImageMipUploadPayload {
        AssetGuid ownerGuid;
        uint32_t mipLevel = 0;
        uint32_t width = 1;
        uint32_t height = 1;
        std::vector<uint8_t> bytes;
    };
    struct StreamingGpuBlasBuildPayload {
        AssetGuid ownerGuid;
        uint64_t indexDataOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t vertexStride = 0;
    };
    std::unordered_map<uint64_t, StreamingGpuBufferUploadPayload> streamingGpuBufferUploadPayloads_;
    std::unordered_map<uint64_t, StreamingGpuImageMipUploadPayload> streamingGpuImageMipUploadPayloads_;
    std::unordered_map<uint64_t, StreamingGpuBlasBuildPayload> streamingGpuBlasBuildPayloads_;
    std::unordered_map<uint64_t, AssetGuid> streamingGpuBlasCompactionPayloads_;
    // Maps a streaming work-queue submitted timeline value to the real device
    // transfer-executor timeline marker that gates its completion. Front-to-back
    // ascending; drained as the device signals each marker.
    std::deque<std::pair<uint64_t, uint64_t>> streamingGpuWorkTimelineMarkers_;
    IncrementalGpuSceneUpdateQueue streamingGpuSceneUpdateQueue_;
    IncrementalGpuSceneApplyFrameResult lastStreamingGpuSceneApply_{};
    StreamingIoMetrics streamingIoMetrics_{};
    StreamingRuntimeState streamingRuntimeState_;
    std::vector<GpuSceneStreamingInstanceSnapshot> lastStreamingGpuSceneSnapshots_;
    bool pendingOpenLevel_ = false;
    bool pendingSaveLevel_ = false;
    bool pendingSaveAll_ = false;
    bool pendingReloadShaders_ = false;
    bool pendingUndo_ = false;
    bool pendingRedo_ = false;
    AssetManager assets_;
    std::unordered_map<std::string, AnimationClip> animationClipCache_;
    std::unordered_map<std::string, AnimationController> animationControllerCache_;
    std::unordered_map<uint32_t, std::vector<glm::vec3>> animationMeshBasePositions_;
    std::unordered_map<AssetGuid, std::filesystem::path> nativeRuntimeAnimationPathsByGuid_;
    std::unordered_set<std::string> failedAnimationClipLoads_;
    std::unordered_set<std::string> failedAnimationControllerLoads_;
    CameraController cameraController_;
    SunDragState sunDrag_{};
    std::array<unsigned char, 512> keyState_{};
    float lastFrameSeconds_ = 0.0f;
    float autosaveElapsedSeconds_ = 0.0f;
    uint64_t frameSerial_ = 0;
    uint64_t topologyRouteGeneration_ = 0;
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
    uint64_t nextNativeFileMigrationJobSerial_ = 1;
    EditorJobCenterState completedNativeFileMigrationJob_{};
    std::optional<ActiveNativeFileMigrationJob> activeNativeFileMigrationJob_{};
    std::optional<ActiveProgressiveRuntimeLoadJob> activeProgressiveRuntimeLoadJob_{};
    uint64_t nextProgressiveRuntimeLoadJobSerial_ = 1;
    uint64_t lastStreamingTopologyBlockLogSerial_ = 0;
    std::deque<EditorNativeFileMigrationJobRequest> pendingNativeFileMigrationJobs_;
    uint64_t frameWorkProbeJobId_ = 0;
    bool frameWorkProbeCompletionPending_ = false;
    uint64_t editorGpuUploadCompletedTimeline_ = 0;
    uint64_t streamingGpuWorkCompletedTimeline_ = 0;
    uint64_t editorTopologyRebuildCompletedTimeline_ = 0;
    UndoStack undoStack_;
    SceneToGpuSceneBuilder sceneBuilder_;
    FrameWorkScheduler frameWorkScheduler_;
    GpuUploadTicketQueue editorGpuUploadTickets_;
    MainThreadApplyTicketQueue editorMainThreadApplyTickets_;
    TopologyRebuildTicketQueue editorTopologyRebuildTickets_;
    std::deque<LiveMainThreadApplyBatch> liveMainThreadApplyBatches_;
    std::optional<SceneAsset> gpuSceneAsset_;
    std::vector<EntityId> gpuInstanceEntities_;
    AnimatedGeometryStats latestAnimatedGeometryStats_{};
    std::vector<GpuSkinningInstancePlan> latestGpuSkinningPlan_;
    std::vector<glm::mat4> latestGpuSkinningJointMatrices_;
    std::vector<glm::mat4> latestGpuSkinningPreviousJointMatrices_;
    std::vector<PathTracerRenderer::GpuSkinningSourceVertex> latestGpuSkinningSourceVertices_;
    std::vector<GpuLocalVertex> latestGpuSkinningMorphDeltas_;
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



