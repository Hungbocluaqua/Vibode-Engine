#pragma once

#include "rtv/EditorSelection.h"
#include "rtv/MeshAsset.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/SceneDocument.h"

#include <Volk/volk.h>
#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace rtv {

class AssetManager;
class CameraController;

struct EditorPanelVisibility {
    bool viewport = true;
    bool sceneHierarchy = true;
    bool inspector = true;
    bool assetBrowser = true;
    bool materialEditor = true;
    bool renderSettings = true;
    bool debugProfiler = true;
};

struct EditorViewportState {
    VkDescriptorSet texture = VK_NULL_HANDLE;
    VkExtent2D renderExtent{};
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

struct EditorRuntimeState {
    PathTracerRenderer& renderer;
    const SceneAsset* importedScene = nullptr;
    SceneDocument* sceneDocument = nullptr;
    const AssetManager* assets = nullptr;
    const std::optional<std::filesystem::path>* gltfPath = nullptr;
    const std::optional<std::filesystem::path>* hdrPath = nullptr;
    const std::vector<EntityId>* instanceEntities = nullptr;
    const std::string* sceneLoadingStatus = nullptr;
    const CameraController* camera = nullptr;
    VkExtent2D swapchainExtent{};
    float cpuFrameMs = 0.0f;
    EditorViewportState viewport{};
};

struct EditorMaterialUpdate {
    uint32_t materialId = UINT32_MAX;
    MaterialAsset material{};
};

struct EditorMaterialAssignment {
    MeshAssetHandle mesh{};
    uint32_t primitiveIndex = UINT32_MAX;
    MaterialAssetHandle material{};
};

struct EditorEntityBoolChange {
    EntityId entity{};
    bool value = false;
};

struct EditorRequests {
    std::optional<RendererSettings> settings;
    std::optional<AccumulationResetReason> resetAccumulation;
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
    std::optional<EntityId> focusOnEntity;
    std::optional<EditorEntityBoolChange> setEntityVisibility;
    std::optional<EditorEntityBoolChange> setEntityLocked;
    bool resetCamera = false;
    bool reloadShaders = false;
    bool undo = false;
    bool redo = false;
    bool resetLayout = false;
    bool saveLayout = false;
    bool toggleDenoiser = false;
    bool toggleDebugView = false;
    bool exit = false;
};

[[nodiscard]] const std::array<RendererDebugView, 38>& editorDebugViews();
[[nodiscard]] int editorDebugViewIndex(RendererDebugView view);
void editorDebugViewCombo(const char* label, RendererSettings& settings, bool& changed);
void requestSettings(EditorRequests& requests, const RendererSettings& settings);

} // namespace rtv
