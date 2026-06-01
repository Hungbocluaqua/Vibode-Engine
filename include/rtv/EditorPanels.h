#pragma once

#include "rtv/EditorSelection.h"
#include "rtv/MeshAsset.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/Project.h"
#include "rtv/AssetRegistry.h"
#include "rtv/SceneDocument.h"

#include <Volk/volk.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
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

struct EditorPlacementStatus {
    EntityId entity{};
    uint64_t serial = 0;
    std::string label;
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
    bool sceneDirty = false;
    const std::vector<EntityId>* instanceEntities = nullptr;
    const std::string* sceneLoadingStatus = nullptr;
    bool sceneLoadRunning = false;
    float sceneLoadProgress = 0.0f;
    CameraController* camera = nullptr;
    const UndoStack* undoStack = nullptr;
    EditorLog* log = nullptr;
    EditorTimeline* timeline = nullptr;
    EditorPreferences* editorPrefs = nullptr;
    EditorUiTextureProvider uiTextures{};
    CameraBookmarkManager* cameraBookmarks = nullptr;
    const EditorRenderJobStatus* renderJob = nullptr;
    const EditorPlacementStatus* placement = nullptr;
    VkExtent2D swapchainExtent{};
    float cpuFrameMs = 0.0f;
    EditorViewportState viewport{};
};

struct ProjectManagerRuntimeState {
    const ProjectContext* project = nullptr;
    const std::string* sceneLoadingStatus = nullptr;
    bool sceneLoadRunning = false;
    float sceneLoadProgress = 0.0f;
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

struct EditorEntityBoolChange {
    EntityId entity{};
    bool value = false;
};

struct EditorEntityTransformChange {
    EntityId entity{};
    Transform oldTransform{};
    Transform newTransform{};
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

enum class EditorEntityCreateKind : uint32_t {
    Empty,
    Camera,
    Light,
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

struct EditorRequests {
    std::optional<RendererSettings> settings;
    std::optional<AccumulationResetReason> resetAccumulation;
    bool newScene = false;
    std::optional<std::filesystem::path> openScene;
    std::optional<std::filesystem::path> saveScene;
    std::optional<std::filesystem::path> saveSceneAs;
    std::optional<EditorImportAssetRequest> importAsset;
    std::optional<EditorImportAssetRequest> importAndPlace;
    std::optional<AssetGuid> reimportAsset;
    std::optional<AssetGuid> placeAsset;
    std::optional<std::filesystem::path> importSceneAsNewScene;
    std::optional<std::filesystem::path> mergeScene;
    std::optional<CreateProjectRequest> createProject;
    std::optional<OpenProjectRequest> openProject;
    std::optional<ProjectContext> projectSettingsUpdate;
    std::optional<std::filesystem::path> loadGltf;
    std::optional<std::filesystem::path> loadHdr;
    std::optional<std::filesystem::path> saveSceneJson;
    std::optional<std::filesystem::path> loadSceneJson;
    std::optional<EditorMaterialUpdate> materialUpdate;
    std::optional<EditorMaterialAssignment> materialAssignment;
    std::optional<SceneUpdateKind> sceneUpdate;
    std::optional<float> cameraMoveSpeed;
    std::optional<EntityId> duplicateEntity;
    std::optional<EntityId> deleteEntity;
    std::optional<EditorEntityRenameRequest> renameEntity;
    std::optional<EditorEntityCreateRequest> createEntity;
    std::optional<EditorComponentRequest> addComponent;
    std::optional<EditorComponentRequest> removeComponent;
    std::optional<EntityId> focusOnEntity;
    std::optional<std::pair<EntityId, EntityId>> reparentEntity; // child, newParent
    std::optional<EditorEntityBoolChange> setEntityVisibility;
    std::optional<EditorEntityBoolChange> setEntityLocked;
    std::optional<EditorEntityTransformChange> setEntityTransform;
    std::optional<EditorEntityTransformPreview> previewEntityTransform;
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
    bool resetLayout = false;
    bool saveLayout = false;
    bool toggleDenoiser = false;
    bool toggleDebugView = false;
    bool cycleIntermediateView = false;
    bool renderCurrentViewport = false;
    bool renderImage = false;
    bool renderSequence = false;
    bool stopRender = false;
    bool openOutputFolder = false;
    bool ensurePrimarySun = false;
    bool closeProject = false;
    bool closeScene = false;
    bool continueWithoutProject = false;
    bool saveProjectSettings = false;
    bool showProjectManager = false;
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

[[nodiscard]] const std::array<RendererDebugView, 95>& editorDebugViews();
[[nodiscard]] int editorDebugViewIndex(RendererDebugView view);
void editorDebugViewCombo(const char* label, RendererSettings& settings, bool& changed);
void requestSettings(EditorRequests& requests, const RendererSettings& settings);

} // namespace rtv
