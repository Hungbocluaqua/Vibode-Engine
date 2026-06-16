#pragma once

#include "rtv/EditorSelection.h"
#include "rtv/EditorPreferences.h"
#include "rtv/FrameWorkScheduler.h"
#include "rtv/GpuUploadTicket.h"
#include "rtv/MainThreadApplyTicket.h"
#include "rtv/MeshAsset.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/Project.h"
#include "rtv/AssetRegistry.h"
#include "rtv/SceneDocument.h"
#include "rtv/TopologyRebuildTicket.h"

#include <Volk/volk.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace rtv {

class AssetManager;
class CameraController;
class CameraBookmarkManager;
class EditorLog;
class EditorTimeline;
class UndoStack;
struct EditorPreferences;

struct EditorPanelVisibility {
    bool viewport = true;
    bool sceneHierarchy = true;
    bool inspector = true;
    bool assetBrowser = true;
    bool renderWorldSettings = false;
    bool timeline = true;
    bool log = true;
    bool console = false;
    bool materialEditor = false;
    bool renderSettings = true;
    bool debugProfiler = false;
    bool sceneStats = false;
    bool gpuDiagnostics = false;
    bool jobCenter = false;
};

struct EditorViewportState {
    VkDescriptorSet texture = VK_NULL_HANDLE;
    VkExtent2D renderExtent{};
    VkExtent2D displayExtent{};
    glm::vec2 imageOrigin{};
    glm::vec2 imageSize{};
    glm::vec2 mousePosition{};
    glm::vec2 mouseUv{};
    bool textureReady = false;
    bool focused = false;
    bool hovered = false;
    bool mouseCaptureActive = false;
    bool leftClicked = false;
};

enum class EditorRenderJobKind : uint32_t {
    None,
    CurrentViewport,
    Image,
    Sequence,
};

struct EditorRenderJobStatus {
    EditorRenderJobKind kind = EditorRenderJobKind::None;
    bool active = false;
    bool completed = false;
    bool cancelled = false;
    bool failed = false;
    float progress = 0.0f;
    int currentFrame = 0;
    int totalFrames = 0;
    uint64_t serial = 0;
    std::string title;
    std::string status;
    std::filesystem::path outputRoot;
    std::filesystem::path manifestPath;
};

struct EditorRenderRequest {
    EditorRenderJobKind kind = EditorRenderJobKind::Image;
    std::filesystem::path outputRoot;
    std::string outputPresetName;
    uint32_t requestedWidth = 0;
    uint32_t requestedHeight = 0;
    float renderResolutionScale = 1.0f;
    uint32_t samplesPerPixel = 1;
    bool limitSamplesPerPixel = false;
    uint32_t targetSamplesPerPixel = 64;
    uint32_t imageAccumulationFrames = 64;
    int sequenceStartFrame = 0;
    int sequenceEndFrame = 0;
    uint32_t sequenceFramesPerTimelineFrame = 1;
    bool saveSequenceFramesAsDefault = false;
};

enum class EditorNativeTextureTargetSetProfile : uint32_t {
    ActiveAndAllBc,
    ActiveOnly,
    AllBcAudit,
    ActiveAllBcAndRgbaFallback,
    Custom,
    CustomLibrary,
};

struct EditorCookProjectRequest {
    std::filesystem::path projectFile;
    std::filesystem::path outputDir;
    bool emitNativeTextureTargetSets = false;
    EditorNativeTextureTargetSetProfile nativeTextureTargetSetProfile = EditorNativeTextureTargetSetProfile::ActiveAndAllBc;
    EditorNativeTextureTargetSetCustomProfile customNativeTextureTargetSet{};
    std::vector<EditorNativeTextureTargetSetLibraryProfile> customNativeTextureTargetSetLibrary;
};

struct EditorPlacementStatus {
    EntityId entity{};
    uint64_t serial = 0;
    std::string label;
};

struct EditorMountedNativePackageWatchSnapshot {
    std::filesystem::path packagePath;
    std::filesystem::path detectionReportPath;
    uint64_t generation = 0;
    int64_t lastWriteTimeTicks = 0;
    int64_t detectedWriteTimeTicks = 0;
    uint32_t textureCount = 0;
    uint32_t materialCount = 0;
    uint32_t meshCount = 0;
    bool changeDetected = false;
};

struct EditorJobCenterState {
    uint64_t sceneLoadJobSerial = 0;
    bool sceneLoadRunning = false;
    float sceneLoadProgress = 0.0f;
    std::string sceneLoadStatus;
    std::string sceneLoadTitle;
    std::filesystem::path sceneLoadSourcePath;
    std::string sceneLoadStage;
    size_t queuedSceneMerges = 0;
    uint64_t completedSceneLoadSerial = 0;
    bool completedSceneLoadSuccess = false;
    bool completedSceneLoadCancelled = false;
    std::string completedSceneLoadTitle;
    std::string completedSceneLoadStatus;
    std::filesystem::path completedSceneLoadSourcePath;
    std::string completedSceneLoadError;
    std::string completedSceneLoadWarning;
    double completedSceneLoadWorkerTotalMs = 0.0;
    double completedSceneLoadWorkerSceneParseMs = 0.0;
    double completedSceneLoadWorkerGltfLoadMs = 0.0;
    double completedSceneLoadWorkerDocumentBuildMs = 0.0;
    uint64_t assetImportJobSerial = 0;
    bool assetImportRunning = false;
    float assetImportProgress = 0.0f;
    std::string assetImportTitle;
    std::string assetImportStatus;
    bool assetImportCanRetry = false;
    bool assetImportPlaceAfterImport = false;
    std::filesystem::path assetImportSourcePath;
    std::filesystem::path assetImportDestinationFolder;
    std::string assetImportMode;
    AssetImportSettings assetImportSettings{};
    AssetGuid assetReimportGuid;
    double assetImportWorkerElapsedMs = 0.0;
    double assetImportStageElapsedMs = 0.0;
    uint64_t completedAssetImportSerial = 0;
    bool completedAssetImportSuccess = false;
    std::string completedAssetImportTitle;
    std::string completedAssetImportStatus;
    std::filesystem::path completedAssetImportSourcePath;
    std::filesystem::path completedAssetImportReportPath;
    std::vector<std::string> completedAssetImportErrors;
    std::vector<std::string> completedAssetImportWarnings;
    bool completedAssetImportCanRetry = false;
    bool completedAssetImportPlaceAfterImport = false;
    std::filesystem::path completedAssetImportDestinationFolder;
    std::string completedAssetImportMode;
    AssetImportSettings completedAssetImportSettings{};
    AssetGuid completedAssetReimportGuid;
    double completedAssetImportWorkerTotalMs = 0.0;
    double completedAssetImportWorkerValidateMs = 0.0;
    double completedAssetImportWorkerDirectoryMs = 0.0;
    double completedAssetImportWorkerInspectMs = 0.0;
    double completedAssetImportWorkerWriteMs = 0.0;
    size_t queuedAssetImports = 0;
    FrameWorkSchedulerSnapshot frameWorkScheduler{};
    bool frameWorkSchedulerAvailable = false;
    std::vector<GpuUploadTicketSnapshot> gpuUploadTickets;
    uint64_t gpuUploadNextTimelineValue = 0;
    bool gpuUploadTicketsAvailable = false;
    std::vector<MainThreadApplyTicketSnapshot> mainThreadApplyTickets;
    bool mainThreadApplyTicketsAvailable = false;
    std::vector<TopologyRebuildTicketSnapshot> topologyRebuildTickets;
    uint64_t topologyRebuildLatestGeneration = 0;
    uint64_t topologyRebuildNextTimelineValue = 0;
    bool topologyRebuildTicketsAvailable = false;
    std::vector<EditorMountedNativePackageWatchSnapshot> mountedNativePackageWatches;
    bool mountedNativePackageWatchesAvailable = false;
    uint64_t cookProjectJobSerial = 0;
    bool cookProjectRunning = false;
    float cookProjectProgress = 0.0f;
    std::string cookProjectStatus;
    std::filesystem::path cookProjectFile;
    std::filesystem::path cookProjectOutputDir;
    std::filesystem::path cookProjectManifestPath;
    std::filesystem::path cookProjectValidationReportPath;
    std::filesystem::path cookProjectLogPath;
    std::string cookProjectManifestStatus;
    size_t cookProjectPlannedFileCount = 0;
    size_t cookProjectCopiedFileCount = 0;
    uint64_t completedCookProjectSerial = 0;
    bool completedCookProjectSuccess = false;
    std::string completedCookProjectStatus;
    std::filesystem::path completedCookProjectFile;
    std::filesystem::path completedCookProjectOutputDir;
    std::filesystem::path completedCookProjectManifestPath;
    std::filesystem::path completedCookProjectValidationReportPath;
    std::filesystem::path completedCookProjectLogPath;
    int completedCookProjectExitCode = 0;
    double completedCookProjectWorkerTotalMs = 0.0;
    uint64_t nativeFileMigrationJobSerial = 0;
    bool nativeFileMigrationRunning = false;
    float nativeFileMigrationProgress = 0.0f;
    bool nativeFileMigrationPackage = false;
    bool nativeFileMigrationDryRun = false;
    std::string nativeFileMigrationTitle;
    std::string nativeFileMigrationStatus;
    std::filesystem::path nativeFileMigrationSourcePath;
    std::filesystem::path nativeFileMigrationReportPath;
    double nativeFileMigrationWorkerElapsedMs = 0.0;
    size_t queuedNativeFileMigrations = 0;
    uint64_t completedNativeFileMigrationSerial = 0;
    bool completedNativeFileMigrationSuccess = false;
    bool completedNativeFileMigrationPackage = false;
    bool completedNativeFileMigrationDryRun = false;
    bool completedNativeFileMigrationMutationAttempted = false;
    bool completedNativeFileMigrationMutated = false;
    bool completedNativeFileMigrationRequired = false;
    bool completedNativeFileMigrationAvailable = false;
    std::string completedNativeFileMigrationTitle;
    std::string completedNativeFileMigrationStatus;
    std::filesystem::path completedNativeFileMigrationSourcePath;
    std::filesystem::path completedNativeFileMigrationReportPath;
    std::filesystem::path completedNativeFileMigrationBackupPath;
    std::vector<std::string> completedNativeFileMigrationErrors;
    std::vector<std::string> completedNativeFileMigrationWarnings;
    double completedNativeFileMigrationWorkerTotalMs = 0.0;
};

struct EditorUiTextureProvider {
    void* user = nullptr;
    VkDescriptorSet (*acquire)(void* user, VkImageView imageView, VkImageLayout imageLayout) = nullptr;
    VkDescriptorSet (*acquireAssetPreview)(void* user, const std::filesystem::path& path, uint32_t* width, uint32_t* height) = nullptr;

    [[nodiscard]] bool valid() const { return user != nullptr && acquire != nullptr; }
    [[nodiscard]] VkDescriptorSet texture(VkImageView imageView, VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) const {
        if (!valid() || imageView == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }
        return acquire(user, imageView, imageLayout);
    }
    [[nodiscard]] VkDescriptorSet assetPreviewTexture(const std::filesystem::path& path, uint32_t* width = nullptr, uint32_t* height = nullptr) const {
        if (!valid() || acquireAssetPreview == nullptr || path.empty()) {
            return VK_NULL_HANDLE;
        }
        return acquireAssetPreview(user, path, width, height);
    }
};

struct EditorRuntimeState {
    PathTracerRenderer& renderer;
    const SceneAsset* importedScene = nullptr;
    SceneDocument* sceneDocument = nullptr;
    const AssetManager* assets = nullptr;
    const std::optional<std::filesystem::path>* gltfPath = nullptr;
    const std::optional<std::filesystem::path>* hdrPath = nullptr;
    const std::optional<std::filesystem::path>* scenePath = nullptr;
    const ProjectContext* project = nullptr;
    const AssetRegistry* assetRegistry = nullptr;
    const std::unordered_map<AssetGuid, MaterialAsset>* dirtyMaterialAssets = nullptr;
    const std::unordered_map<AssetGuid, std::filesystem::path>* materialAssetAutosavePaths = nullptr;
    bool sceneDirty = false;
    bool projectSettingsDirty = false;
    const std::vector<EntityId>* instanceEntities = nullptr;
    const std::string* sceneLoadingStatus = nullptr;
    bool sceneLoadRunning = false;
    float sceneLoadProgress = 0.0f;
    CameraController* camera = nullptr;
    const UndoStack* undoStack = nullptr;
    EditorLog* log = nullptr;
    EditorTimeline* timeline = nullptr;
    EditorPreferences* editorPrefs = nullptr;
    std::filesystem::path editorPreferencesPath;
    EditorUiTextureProvider uiTextures{};
    CameraBookmarkManager* cameraBookmarks = nullptr;
    const EditorRenderJobStatus* renderJob = nullptr;
    const EditorPlacementStatus* placement = nullptr;
    const EditorJobCenterState* jobCenter = nullptr;
    const std::vector<std::filesystem::path>* pendingDroppedFiles = nullptr;
    VkExtent2D swapchainExtent{};
    float cpuFrameMs = 0.0f;
    EditorViewportState viewport{};
};

struct ProjectManagerRuntimeState {
    const ProjectContext* project = nullptr;
    const AssetRegistry* assetRegistry = nullptr;
    const std::optional<std::filesystem::path>* scenePath = nullptr;
    const std::string* sceneLoadingStatus = nullptr;
    bool sceneLoadRunning = false;
    float sceneLoadProgress = 0.0f;
    bool sceneDirty = false;
    bool projectSettingsDirty = false;
    size_t dirtyMaterialAssetCount = 0;
    bool cookProjectRunning = false;
    std::filesystem::path cookProjectOutputDir;
    uint64_t completedCookProjectSerial = 0;
    bool completedCookProjectSuccess = false;
    std::string completedCookProjectStatus;
    std::filesystem::path completedCookProjectOutputDir;
    std::filesystem::path completedCookProjectManifestPath;
    std::filesystem::path completedCookProjectValidationReportPath;
    std::filesystem::path completedCookProjectLogPath;
    int completedCookProjectExitCode = 0;
    bool standaloneLauncher = false;
};

struct EditorMaterialUpdate {
    uint32_t materialId = UINT32_MAX;
    MaterialAsset material{};
};

struct EditorMaterialAssignment {
    EntityId entity{};
    MeshAssetHandle mesh{};
    uint32_t primitiveIndex = UINT32_MAX;
    MaterialAssetHandle material{};
};

struct EditorMaterialAssetAssignment {
    AssetGuid materialGuid;
    EntityId entity{};
    uint32_t primitiveIndex = UINT32_MAX;
};

struct EditorMeshAssetPlacement {
    AssetGuid meshGuid;
    std::optional<Transform> placementTransform;
    EntityId replaceEntity{};
    EntityId attachEntity{};
};

struct EditorMeshScatterInstancePlacement {
    AssetGuid meshGuid;
    AssetGuid materialGuid;
    Transform transform{};
};

struct EditorMeshScatterPlacement {
    std::vector<EditorMeshScatterInstancePlacement> instances;
    uint32_t seed = 1;
    std::string label;
};

struct EditorEntityBoolChange {
    EntityId entity{};
    bool value = false;
};

struct EditorEntityTransformChange {
    EntityId entity{};
    Transform oldTransform{};
    Transform newTransform{};
};

enum class EditorAlignDistributeAxis : uint32_t {
    X,
    Y,
    Z,
};

enum class EditorAlignDistributeMode : uint32_t {
    AlignMin,
    AlignCenter,
    AlignMax,
    DistributeSpacing,
};

struct EditorAlignDistributeEntityBounds {
    EntityId entity{};
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool available = false;
};

struct EditorAlignDistributeRequest {
    std::vector<EntityId> entities;
    std::vector<EditorAlignDistributeEntityBounds> bounds;
    EditorAlignDistributeAxis axis = EditorAlignDistributeAxis::X;
    EditorAlignDistributeMode mode = EditorAlignDistributeMode::AlignCenter;
};

struct EditorEntityRenameRequest {
    EntityId entity{};
    std::string name;
};

struct EditorEntityTransformPreview {
    EntityId entity{};
    Transform transform{};
    SceneUpdateKind updateKind = SceneUpdateKind::TransformOnly;
};

struct EditorEntityTransformBatchChange {
    std::vector<EditorEntityTransformChange> changes;
};

struct EditorEntityTransformBatchPreview {
    std::vector<EditorEntityTransformPreview> previews;
};

enum class EditorEntityCreateKind : uint32_t {
    Empty,
    Camera,
    Light,
    Sun,
    SpotLight,
    AreaLight,
    EnvironmentLight,
    SkyAtmosphere,
    HeightFog,
    VolumetricCloud,
    PostProcessVolume,
};

enum class EditorComponentKind : uint32_t {
    Light,
    Sun,
    Camera,
    MeshRenderer,
    EnvironmentLight,
    SkyAtmosphere,
    HeightFog,
    VolumetricCloud,
    PostProcessVolume,
    CameraPostProcess,
    AnimationPlayer,
};

struct EditorSceneSnapshotChange {
    SceneDocument before{};
    SceneUpdateKind updateKind = SceneUpdateKind::TopologyChanged;
    std::string label;
};

struct EditorEntityCreateRequest {
    EditorEntityCreateKind kind = EditorEntityCreateKind::Empty;
    EntityId parent{};
};

struct EditorComponentRequest {
    EntityId entity{};
    EditorComponentKind kind = EditorComponentKind::Light;
};

struct EditorLightChange {
    EntityId entity{};
    Light oldLight{};
    Light newLight{};
};

struct EditorSunChange {
    EntityId entity{};
    Sun oldSun{};
    Sun newSun{};
};

struct EditorCameraChange {
    EntityId entity{};
    Camera oldCamera{};
    Camera newCamera{};
    EntityId oldActiveCamera{};
    EntityId newActiveCamera{};
};

struct EditorMeshRendererChange {
    EntityId entity{};
    MeshRenderer oldRenderer{};
    MeshRenderer newRenderer{};
    SceneUpdateKind updateKind = SceneUpdateKind::TopologyChanged;
};

struct EditorTimelineTransformSample {
    EntityId entity{};
    Transform transform{};
};

struct EditorImportAssetRequest {
    std::filesystem::path sourcePath;
    std::filesystem::path destinationFolder;
    std::string mode = "ImportAsset";
    AssetImportSettings settings{};
};

struct EditorAssetRelinkSourceRequest {
    AssetGuid guid;
    std::filesystem::path sourcePath;
};

struct EditorReplaceAssetReferencesRequest {
    AssetGuid oldGuid;
    AssetGuid newGuid;
    bool includeSavedProjectFiles = false;
};

struct EditorRepairMissingAssetDependenciesRequest {
    AssetGuid ownerGuid;
    bool saveRegistry = true;
};

struct EditorAssetTagsRequest {
    AssetGuid guid;
    std::vector<std::string> tags;
};

struct EditorRenameAssetRequest {
    AssetGuid guid;
    std::string displayName;
};

struct EditorBulkAssetTagRequest {
    std::vector<AssetGuid> guids;
    std::string tag;
};

struct EditorMoveAssetsToFolderRequest {
    std::vector<AssetGuid> guids;
    std::string folderName;
};

struct EditorDeleteAssetRequest {
    std::vector<AssetGuid> guids;
    bool deleteGeneratedFiles = false;
};

struct EditorNativePackageMountRequest {
    std::filesystem::path packagePath;
};

struct EditorNativePackageUnloadRequest {
    std::filesystem::path packagePath;
};

struct EditorNativePackageRefreshRequest {
    std::filesystem::path packagePath;
};

struct EditorNativeFileMigrationJobRequest {
    uint64_t serial = 0;
    bool package = false;
    bool dryRun = false;
    std::filesystem::path sourcePath;
    std::filesystem::path reportPath;
};

struct EditorNativeFileMigrationJobResult {
    uint64_t serial = 0;
    bool package = false;
    bool dryRun = false;
    bool success = false;
    bool mutationAttempted = false;
    bool mutated = false;
    bool migrationRequired = false;
    bool migrationAvailable = false;
    std::filesystem::path sourcePath;
    std::filesystem::path reportPath;
    std::filesystem::path backupPath;
    std::string title;
    std::string status;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    double elapsedMs = 0.0;
};

struct EditorRequests {
    std::optional<RendererSettings> settings;
    std::optional<AccumulationResetReason> resetAccumulation;
    bool newScene = false;
    std::optional<std::filesystem::path> openScene;
    std::optional<std::filesystem::path> saveScene;
    std::optional<std::filesystem::path> saveSceneAs;
    std::optional<EditorImportAssetRequest> importAsset;
    std::vector<EditorImportAssetRequest> importAssets;
    std::optional<EditorImportAssetRequest> importAndPlace;
    std::vector<EditorImportAssetRequest> importAndPlaceAssets;
    std::optional<AssetGuid> reimportAsset;
    std::optional<EditorAssetRelinkSourceRequest> relinkAssetSource;
    std::optional<EditorReplaceAssetReferencesRequest> replaceAssetReferences;
    std::optional<EditorRepairMissingAssetDependenciesRequest> repairMissingAssetDependencies;
    std::optional<EditorAssetTagsRequest> updateAssetTags;
    std::optional<EditorRenameAssetRequest> renameAsset;
    std::optional<EditorBulkAssetTagRequest> bulkAddAssetTag;
    std::optional<EditorBulkAssetTagRequest> bulkRemoveAssetTag;
    std::optional<EditorMoveAssetsToFolderRequest> moveAssetsToFolder;
    std::optional<EditorDeleteAssetRequest> deleteAssets;
    std::optional<EditorNativePackageMountRequest> mountNativePackage;
    std::optional<EditorNativePackageUnloadRequest> unloadNativePackage;
    std::optional<EditorNativePackageRefreshRequest> refreshNativePackage;
    std::optional<EditorNativeFileMigrationJobRequest> nativeFileMigrationJobRequest;
    std::vector<EditorNativeFileMigrationJobRequest> nativeFileMigrationJobRequests;
    std::optional<EditorNativeFileMigrationJobResult> nativeFileMigrationJobResult;
    std::optional<AssetGuid> placeAsset;
    std::optional<Transform> placeAssetTransform;
    std::optional<std::filesystem::path> importSceneAsNewScene;
    std::optional<std::filesystem::path> mergeScene;
    std::vector<std::filesystem::path> mergeScenes;
    std::optional<CreateProjectRequest> createProject;
    std::optional<OpenProjectRequest> openProject;
    std::optional<DeleteProjectRequest> deleteProject;
    std::optional<ProjectContext> projectSettingsUpdate;
    std::optional<std::filesystem::path> loadGltf;
    std::optional<std::filesystem::path> loadHdr;
    std::optional<std::filesystem::path> saveSceneJson;
    std::optional<std::filesystem::path> loadSceneJson;
    std::optional<EditorMaterialUpdate> materialUpdate;
    std::optional<EditorMaterialAssignment> materialAssignment;
    std::optional<EditorMaterialAssetAssignment> materialAssetAssignment;
    std::optional<EditorMeshAssetPlacement> meshAssetPlacement;
    std::optional<EditorMeshScatterPlacement> meshScatterPlacement;
    std::optional<AssetGuid> environmentAssetAssignment;
    std::optional<std::filesystem::path> dismissDroppedFile;
    bool dismissAllDroppedFiles = false;
    std::optional<SceneUpdateKind> sceneUpdate;
    std::optional<float> cameraMoveSpeed;
    std::optional<float> cameraFastMoveSpeed;
    std::optional<float> cameraMouseSensitivity;
    std::optional<bool> cameraInvertLookX;
    std::optional<bool> cameraInvertLookY;
    std::optional<EntityId> duplicateEntity;
    std::optional<EntityId> deleteEntity;
    std::vector<EntityId> deleteEntities;
    std::optional<EditorEntityRenameRequest> renameEntity;
    std::optional<EditorEntityCreateRequest> createEntity;
    std::optional<EditorComponentRequest> addComponent;
    std::optional<EditorComponentRequest> removeComponent;
    std::optional<EntityId> focusOnEntity;
    std::optional<std::pair<EntityId, EntityId>> reparentEntity; // child, newParent
    std::optional<EditorEntityBoolChange> setEntityVisibility;
    std::optional<EditorEntityBoolChange> setEntityLocked;
    std::optional<EditorEntityTransformChange> setEntityTransform;
    std::optional<EditorEntityTransformBatchChange> setEntityTransforms;
    std::optional<EditorAlignDistributeRequest> alignDistributeEntities;
    std::optional<EditorEntityTransformPreview> previewEntityTransform;
    std::optional<EditorEntityTransformBatchPreview> previewEntityTransforms;
    std::optional<EditorMeshRendererChange> setMeshRenderer;
    std::optional<EditorSceneSnapshotChange> sceneSnapshot;
    std::optional<nlohmann::json> timelineChanged;
    std::vector<EditorTimelineTransformSample> timelinePlaybackTransforms;
    std::optional<EditorLightChange> setLight;
    std::optional<EditorSunChange> setSun;
    std::optional<EditorCameraChange> setCamera;
    bool resetCamera = false;
    bool reloadShaders = false;
    bool undo = false;
    bool redo = false;
    bool saveAll = false;
    std::optional<AssetGuid> saveMaterialAsset;
    bool resetLayout = false;
    bool saveLayout = false;
    bool toggleDenoiser = false;
    bool togglePrimarySun = false;
    bool toggleDebugView = false;
    bool cycleIntermediateView = false;
    bool renderCurrentViewport = false;
    bool renderImage = false;
    bool renderSequence = false;
    std::optional<EditorRenderRequest> renderRequest;
    std::optional<EditorCookProjectRequest> cookProject;
    bool openProjectDirectory = false;
    bool openLogFolder = false;
    bool openSelectedAsset = false;
    bool stopRender = false;
    bool openOutputFolder = false;
    std::optional<std::filesystem::path> openOutputFolderPath;
    std::optional<std::filesystem::path> openDirectoryPath;
    std::optional<std::filesystem::path> openFilePath;
    bool toggleFullscreen = false;
    bool ensurePrimarySun = false;
    bool closeProject = false;
    bool closeScene = false;
    bool continueWithoutProject = false;
    bool saveProjectSettings = false;
    bool showProjectManager = false;
    bool showProjectSettings = false;
    bool showInspector = false;
    bool showMaterialEditor = false;
    bool closeMaterialEditor = false;
    bool showControls = false;
    bool showRendererInfo = false;
    bool showCommandPalette = false;
    bool cancelSceneLoad = false;
    bool restoreAutosave = false;
    bool discardRecovery = false;
    bool exit = false;
    std::optional<std::string> saveCameraBookmark;
    std::optional<size_t> loadCameraBookmarkIndex;
    std::optional<size_t> deleteCameraBookmarkIndex;
    std::optional<std::string> removeFavorite;
};

[[nodiscard]] const std::array<RendererDebugView, 100>& editorDebugViews();
[[nodiscard]] int editorDebugViewIndex(RendererDebugView view);
void editorDebugViewCombo(const char* label, RendererSettings& settings, bool& changed);
void requestSettings(EditorRequests& requests, const RendererSettings& settings);

} // namespace rtv


