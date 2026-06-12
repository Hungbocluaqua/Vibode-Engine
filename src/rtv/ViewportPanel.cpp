#include "rtv/ViewportPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/AssetImport.h"
#include "rtv/CameraController.h"
#include "rtv/EditorCommands.h"
#include "rtv/EditorLog.h"
#include "rtv/EditorPreferences.h"
#include "rtv/EditorTransformUtils.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/Prefab.h"
#include "rtv/RendererDebug.h"
#include "rtv/SceneOperations.h"
#include "rtv/ScatterPalette.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rtv {

namespace {

std::string payloadString(const ImGuiPayload& payload) {
    if (payload.Data == nullptr || payload.DataSize <= 0) {
        return {};
    }
    std::string value(static_cast<const char*>(payload.Data), static_cast<size_t>(payload.DataSize));
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string placementPayloadKey(const ImGuiPayload& payload) {
    const std::string value = payloadString(payload);
    if (payload.IsDataType("PREFAB_ASSET")) {
        return "prefab:" + value;
    }
    if (payload.IsDataType("MESH_ASSET")) {
        return "mesh:" + value;
    }
    return {};
}

const AssetRecord* assetRecordForGuid(const AssetRegistry* registry, const AssetGuid& guid) {
    if (registry == nullptr || guid.empty()) {
        return nullptr;
    }
    for (const AssetRecord& record : registry->records()) {
        if (record.guid == guid) {
            return &record;
        }
    }
    return nullptr;
}

std::string assetLabelForGuid(const EditorRuntimeState& state, const AssetGuid& guid) {
    if (const AssetRecord* record = assetRecordForGuid(state.assetRegistry, guid)) {
        if (!record->displayName.empty()) {
            return record->displayName;
        }
    }
    return guid.empty() ? std::string{"Asset"} : guid;
}

std::filesystem::path resolvedAssetRecordPath(const EditorRuntimeState& state, const std::string& value) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path(value);
    if (path.is_absolute()) {
        return path;
    }
    if (state.project != nullptr) {
        return state.project->projectRoot / path;
    }
    if (state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty()) {
        return state.assetRegistry->state().path.parent_path() / path;
    }
    return path;
}

std::filesystem::path viewportAdvancedToolsReadinessReportPath(const EditorRuntimeState& state) {
    if (state.project != nullptr && !state.project->savedRoot.empty()) {
        return state.project->savedRoot / "Reports" / "viewport_advanced_tools_readiness.json";
    }
    if (state.scenePath != nullptr && state.scenePath->has_value() && !state.scenePath->value().empty()) {
        return state.scenePath->value().parent_path() / "Saved" / "Reports" / "viewport_advanced_tools_readiness.json";
    }
    return std::filesystem::current_path() / "Saved" / "Reports" / "viewport_advanced_tools_readiness.json";
}

nlohmann::json buildViewportAdvancedToolsReadinessReport(
    const EditorRuntimeState& state,
    bool snapEnabled,
    float translationSnap,
    float rotationSnap,
    float scaleSnap,
    bool localGizmoMode,
    bool showGrid) {
    const std::string projectName = state.project != nullptr ? state.project->name : std::string{};
    const std::string scenePath = state.scenePath != nullptr && state.scenePath->has_value()
        ? state.scenePath->value().generic_string()
        : std::string{};

    return {
        {"schema", "ViewportAdvancedToolsReadinessV1"},
        {"project", {
            {"name", projectName},
            {"scenePath", scenePath},
            {"sceneDirty", state.sceneDirty},
            {"projectSettingsDirty", state.projectSettingsDirty},
        }},
        {"currentViewportState", {
            {"showGrid", showGrid},
            {"localGizmoMode", localGizmoMode},
            {"snapEnabled", snapEnabled},
            {"translationSnap", translationSnap},
            {"rotationSnapDegrees", rotationSnap},
            {"scaleSnap", scaleSnap},
        }},
        {"currentSupport", {
            {"singleSelectionImplemented", true},
            {"gizmoMoveRotateScaleImplemented", true},
            {"localWorldTransformFrameImplemented", true},
            {"translationRotationScaleSnapImplemented", true},
            {"raycastGridPlacementImplemented", true},
            {"surfaceAlignDropPreviewImplemented", true},
            {"duplicateNextPlacementBrushImplemented", true},
            {"multiPlaceBrushImplemented", true},
        }},
        {"advancedWorkflowReadiness", {
            {"multiSelectGroupTransformImplemented", true},
            {"customPivotOriginImplemented", true},
            {"surfaceSnappingWorkflowImplemented", true},
            {"alignDistributeImplemented", true},
            {"scatterPlacementPaletteImplemented", true},
            {"levelInstancesSublevelsImplemented", true},
        }},
        {"notes", {
            {"surfaceSnappingScope", "Viewport placement and translate gizmo moves share the CPU scene-query path, snap to loaded mesh triangles, fall back to the grid, and support normal alignment, yaw preservation, offset, bounds-bottom snapping, and axis constraints."},
            {"brushScope", "Duplicate-next and multi-place brushes exist for prefab/Mesh placement; mesh scatter palette placement expands the active mesh brush into deterministic CPU scene-query placements and commits one undo snapshot."},
            {"scatterPaletteScope", "Transparent .rtscatterpalette.json metadata is supported for mesh/prefab/material GUID entries and runtime viewport scatter currently places mesh entries as one batch operation."},
            {"multiSelectGroupTransformScope", "Viewport move/rotate/scale gizmos apply preview transforms to unlocked multi-selected entities and commit one undo snapshot for the group."},
            {"customPivotScope", "Viewport pivots support active entity, selection center, bounds center, and persisted custom transforms stored in rtlevel editor metadata."},
            {"alignDistributeScope", "Align/distribute tools use world bounds when mesh geometry is available and fall back to world transform positions otherwise."},
            {"levelInstanceScope", "Rtlevel drops can become visible level-instance roots with scene GUID/path metadata, loaded/editable/dirty state, cycle rejection, undoable load/unload/edit/break-link operations, and cook-manifest sublevel scene inclusion."},
        }},
        {"policy", {
            {"description", "This report is a read-only readiness inventory for advanced viewport/editor tooling."},
            {"mutationExecuted", false},
            {"performedActions", nlohmann::json::array()},
            {"unsupportedActions", nlohmann::json::array()},
        }},
    };
}

bool writeViewportAdvancedToolsReadinessReport(
    const EditorRuntimeState& state,
    bool snapEnabled,
    float translationSnap,
    float rotationSnap,
    float scaleSnap,
    bool localGizmoMode,
    bool showGrid,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = viewportAdvancedToolsReadinessReportPath(state);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create viewport advanced tools readiness report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write viewport advanced tools readiness report: " + outPath.string();
        return false;
    }
    file << buildViewportAdvancedToolsReadinessReport(state, snapEnabled, translationSnap, rotationSnap, scaleSnap, localGizmoMode, showGrid).dump(2);
    return true;
}

struct ViewportDropPreview {
    bool active = false;
    bool placement = false;
    std::string title;
    std::string detail;
};

struct ViewportPlacementBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

struct ViewportSceneRayHit {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    std::array<glm::vec3, 3> triangleWorld{};
    EntityId entity{};
    uint32_t primitiveIndex = UINT32_MAX;
};

float activeCameraFov(const SceneDocument& document);
glm::mat4 editorViewMatrix(const CameraController& camera);
glm::mat4 editorProjectionMatrix(float fovY, float aspect);
float viewportAspect(const EditorRuntimeState& state);
std::optional<ImVec2> projectViewToScreen(
    const EditorRuntimeState& state,
    const glm::mat4& projection,
    glm::vec3 viewPoint,
    float nearPlane);
bool screenPointInsideViewport(const EditorRuntimeState& state, ImVec2 point, float padding);
std::optional<ViewportSceneRayHit> viewportSceneRaycastUnderCursor(const EditorRuntimeState& state);
std::optional<ViewportSceneRayHit> viewportSceneRaycast(
    const EditorRuntimeState& state,
    const glm::vec3& origin,
    const glm::vec3& rayDir,
    const std::vector<EntityId>& excludedEntities = {});
glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity);
std::optional<Transform> viewportDropPlacementTransform(
    const EditorRuntimeState& state,
    bool snapEnabled,
    float translationSnap,
    bool forceGrid,
    bool surfaceAlign,
    float yawRadians = 0.0f,
    std::optional<ViewportPlacementBounds> bounds = std::nullopt);
bool viewportScatterPaletteActive(const EditorRuntimeState& state);
ScatterPaletteSettings scatterPaletteSettingsFromPreferences(const EditorPreferences* preferences);
std::vector<EditorMeshScatterInstancePlacement> buildScatterMeshPlacements(
    const EditorRuntimeState& state,
    const AssetGuid& meshGuid,
    float yawRadians,
    const ScatterPaletteSettings& settings,
    std::optional<ViewportPlacementBounds> meshBounds,
    size_t maxInstances = 0);
void drawScatterMeshPlacementPreview(
    const EditorRuntimeState& state,
    const std::vector<EditorMeshScatterInstancePlacement>& instances);

std::optional<MeshAssetHandle> loadedMeshHandleForGuid(const EditorRuntimeState& state, const AssetGuid& guid) {
    if (state.assets == nullptr || guid.empty()) {
        return std::nullopt;
    }
    if (state.importedScene != nullptr && state.assetRegistry != nullptr) {
        if (const AssetRecord* record = assetRecordForGuid(state.assetRegistry, guid)) {
            if (record->type == AssetType::Mesh && !record->sourceHash.empty() && !record->importSettingsHash.empty()) {
                for (size_t i = 0; i < state.importedScene->meshes.size(); ++i) {
                    const MeshAssetHandle handle = state.importedScene->meshes[i];
                    if (handle.valid() && importedAssetGuidFor(record->sourceHash, record->importSettingsHash, "Mesh", i) == guid &&
                        state.assets->mesh(handle) != nullptr) {
                        return handle;
                    }
                }
            }
        }
    }
    if (state.sceneDocument != nullptr) {
        for (const Entity* entity : state.sceneDocument->registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            if (renderer.meshGuid == guid && renderer.mesh.valid() && state.assets->mesh(renderer.mesh) != nullptr) {
                return renderer.mesh;
            }
        }
    }
    return std::nullopt;
}

std::optional<ViewportPlacementBounds> meshPlacementBounds(const AssetManager* assets, MeshAssetHandle handle) {
    if (assets == nullptr || !handle.valid()) {
        return std::nullopt;
    }
    const MeshAsset* mesh = assets->mesh(handle);
    if (mesh == nullptr || mesh->vertices.empty()) {
        return std::nullopt;
    }
    ViewportPlacementBounds bounds;
    bounds.min = glm::vec3(std::numeric_limits<float>::max());
    bounds.max = glm::vec3(-std::numeric_limits<float>::max());
    bool hasFiniteVertex = false;
    for (const MeshVertex& vertex : mesh->vertices) {
        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) || !std::isfinite(vertex.position.z)) {
            continue;
        }
        bounds.min = glm::min(bounds.min, vertex.position);
        bounds.max = glm::max(bounds.max, vertex.position);
        hasFiniteVertex = true;
    }
    if (!hasFiniteVertex) {
        return std::nullopt;
    }
    return bounds;
}

void includeTransformedBoundsCorners(ViewportPlacementBounds& outBounds, const ViewportPlacementBounds& localBounds, const glm::mat4& transform, bool& hasPoint) {
    const std::array<glm::vec3, 8> corners = {
        glm::vec3{localBounds.min.x, localBounds.min.y, localBounds.min.z},
        glm::vec3{localBounds.max.x, localBounds.min.y, localBounds.min.z},
        glm::vec3{localBounds.min.x, localBounds.max.y, localBounds.min.z},
        glm::vec3{localBounds.max.x, localBounds.max.y, localBounds.min.z},
        glm::vec3{localBounds.min.x, localBounds.min.y, localBounds.max.z},
        glm::vec3{localBounds.max.x, localBounds.min.y, localBounds.max.z},
        glm::vec3{localBounds.min.x, localBounds.max.y, localBounds.max.z},
        glm::vec3{localBounds.max.x, localBounds.max.y, localBounds.max.z},
    };
    for (const glm::vec3& corner : corners) {
        const glm::vec3 world = glm::vec3(transform * glm::vec4(corner, 1.0f));
        if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)) {
            continue;
        }
        if (!hasPoint) {
            outBounds.min = world;
            outBounds.max = world;
            hasPoint = true;
        } else {
            outBounds.min = glm::min(outBounds.min, world);
            outBounds.max = glm::max(outBounds.max, world);
        }
    }
}

EditorAlignDistributeEntityBounds alignDistributeBoundsForEntity(const EditorRuntimeState& state, const Entity& entity) {
    EditorAlignDistributeEntityBounds result;
    result.entity = entity.id;
    if (state.assets == nullptr || !entity.meshRenderer.has_value()) {
        return result;
    }
    const std::optional<ViewportPlacementBounds> localBounds = meshPlacementBounds(state.assets, entity.meshRenderer->mesh);
    if (!localBounds.has_value()) {
        return result;
    }
    ViewportPlacementBounds worldBounds;
    bool hasPoint = false;
    const glm::mat4 world = state.sceneDocument != nullptr
        ? entityWorldMatrix(state.sceneDocument->registry(), entity)
        : entity.transform.localMatrix();
    includeTransformedBoundsCorners(worldBounds, *localBounds, world, hasPoint);
    if (!hasPoint) {
        return result;
    }
    result.min = worldBounds.min;
    result.max = worldBounds.max;
    result.available = true;
    return result;
}

std::optional<glm::mat4> prefabNodeWorldTransform(const PrefabAsset& prefab, uint32_t nodeIndex, std::vector<std::optional<glm::mat4>>& cache) {
    if (nodeIndex >= prefab.nodes.size()) {
        return std::nullopt;
    }
    if (cache[nodeIndex].has_value()) {
        return cache[nodeIndex];
    }
    const PrefabNodeAsset& node = prefab.nodes[nodeIndex];
    glm::mat4 parent{1.0f};
    if (node.parent >= 0) {
        const uint32_t parentIndex = static_cast<uint32_t>(node.parent);
        if (parentIndex >= prefab.nodes.size() || parentIndex == nodeIndex) {
            return std::nullopt;
        }
        const std::optional<glm::mat4> parentTransform = prefabNodeWorldTransform(prefab, parentIndex, cache);
        if (!parentTransform.has_value()) {
            return std::nullopt;
        }
        parent = *parentTransform;
    }
    cache[nodeIndex] = parent * node.transform;
    return cache[nodeIndex];
}

std::optional<ViewportPlacementBounds> prefabPlacementBoundsForGuid(const EditorRuntimeState& state, const AssetGuid& guid) {
    const AssetRecord* record = assetRecordForGuid(state.assetRegistry, guid);
    if (record == nullptr || record->type != AssetType::Prefab || record->importedPath.empty()) {
        return std::nullopt;
    }

    PrefabAsset prefab;
    std::string error;
    if (!loadPrefabAsset(resolvedAssetRecordPath(state, record->importedPath), prefab, &error) || prefab.nodes.empty()) {
        return std::nullopt;
    }

    ViewportPlacementBounds bounds;
    bool hasPoint = false;
    std::vector<std::optional<glm::mat4>> worldTransforms(prefab.nodes.size());
    for (uint32_t i = 0; i < prefab.nodes.size(); ++i) {
        const PrefabNodeAsset& node = prefab.nodes[i];
        if (node.meshGuid.empty()) {
            continue;
        }
        const std::optional<MeshAssetHandle> handle = loadedMeshHandleForGuid(state, node.meshGuid);
        if (!handle.has_value()) {
            continue;
        }
        const std::optional<ViewportPlacementBounds> meshBounds = meshPlacementBounds(state.assets, *handle);
        if (!meshBounds.has_value()) {
            continue;
        }
        const std::optional<glm::mat4> nodeWorld = prefabNodeWorldTransform(prefab, i, worldTransforms);
        if (!nodeWorld.has_value()) {
            continue;
        }
        includeTransformedBoundsCorners(bounds, *meshBounds, *nodeWorld, hasPoint);
    }
    return hasPoint ? std::optional<ViewportPlacementBounds>{bounds} : std::nullopt;
}

std::optional<ViewportPlacementBounds> meshPlacementBoundsForPayload(const EditorRuntimeState& state, const ImGuiPayload& payload) {
    if (!payload.IsDataType("MESH_ASSET")) {
        return std::nullopt;
    }
    if (const std::optional<MeshAssetHandle> handle = loadedMeshHandleForGuid(state, payloadString(payload))) {
        return meshPlacementBounds(state.assets, *handle);
    }
    return std::nullopt;
}

std::optional<ViewportPlacementBounds> footprintBoundsForPlacementPayload(const EditorRuntimeState& state, const ImGuiPayload& payload) {
    if (payload.IsDataType("MESH_ASSET")) {
        return meshPlacementBoundsForPayload(state, payload);
    }
    if (payload.IsDataType("PREFAB_ASSET")) {
        return prefabPlacementBoundsForGuid(state, payloadString(payload));
    }
    return std::nullopt;
}

std::optional<ViewportPlacementBounds> footprintBoundsForPlacementBrush(const EditorRuntimeState& state, const ViewportPlacementBrushState& brush) {
    if (brush.kind == ViewportPlacementBrushKind::Mesh) {
        if (const std::optional<MeshAssetHandle> handle = loadedMeshHandleForGuid(state, brush.guid)) {
            return meshPlacementBounds(state.assets, *handle);
        }
    } else if (brush.kind == ViewportPlacementBrushKind::Prefab) {
        return prefabPlacementBoundsForGuid(state, brush.guid);
    }
    return std::nullopt;
}

struct ViewportPlacementFootprint {
    float minX = -0.55f;
    float maxX = 0.55f;
    float minZ = -0.55f;
    float maxZ = 0.55f;
    bool assetSized = false;
};

struct ViewportDropMarkerStyle {
    ImU32 color = IM_COL32(110, 205, 190, 255);
    ImU32 textColor = IM_COL32(232, 238, 244, 255);
    std::string suffix;
};

ViewportPlacementFootprint footprintFromBounds(std::optional<ViewportPlacementBounds> bounds) {
    ViewportPlacementFootprint footprint;
    if (!bounds.has_value()) {
        return footprint;
    }
    footprint.minX = bounds->min.x;
    footprint.maxX = bounds->max.x;
    footprint.minZ = bounds->min.z;
    footprint.maxZ = bounds->max.z;
    footprint.assetSized = true;
    auto padAxis = [](float& minValue, float& maxValue) {
        constexpr float minimumExtent = 0.18f;
        const float extent = maxValue - minValue;
        if (extent >= minimumExtent) {
            return;
        }
        const float center = (minValue + maxValue) * 0.5f;
        minValue = center - minimumExtent * 0.5f;
        maxValue = center + minimumExtent * 0.5f;
    };
    padAxis(footprint.minX, footprint.maxX);
    padAxis(footprint.minZ, footprint.maxZ);
    return footprint;
}

bool viewportDropForceGridActive(const EditorRuntimeState& state) {
    return ImGui::GetIO().KeyCtrl ||
        (state.editorPrefs != nullptr && state.editorPrefs->viewportDropForceGridByDefault);
}

bool viewportDropSurfaceAlignActive(const EditorRuntimeState& state, bool forceGrid) {
    if (forceGrid) {
        return false;
    }
    return ImGui::GetIO().KeyAlt ||
        (state.editorPrefs != nullptr && state.editorPrefs->viewportDropSurfaceAlignByDefault);
}

bool viewportDropWheelRotationEnabled(const EditorRuntimeState& state) {
    return state.editorPrefs == nullptr || state.editorPrefs->viewportDropMouseWheelRotationEnabled;
}

bool viewportDropDuplicatePlacementActive(const EditorRuntimeState& state) {
    const ImGuiIO& io = ImGui::GetIO();
    return (!io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_D)) ||
        (state.editorPrefs != nullptr && state.editorPrefs->viewportDropDuplicatePlacementByDefault);
}

bool viewportDropMultiPlaceActive(const EditorRuntimeState& state) {
    const ImGuiIO& io = ImGui::GetIO();
    return (!io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_M)) ||
        (state.editorPrefs != nullptr && state.editorPrefs->viewportDropMultiPlaceByDefault);
}

const char* placementBrushKindName(ViewportPlacementBrushKind kind) {
    switch (kind) {
    case ViewportPlacementBrushKind::Prefab: return "Prefab";
    case ViewportPlacementBrushKind::Mesh: return "Mesh";
    case ViewportPlacementBrushKind::None: break;
    }
    return "Asset";
}

ViewportDropMarkerStyle viewportDropMarkerStyle(
    const EditorRuntimeState& state,
    const EditorSelection& selection,
    const ImGuiPayload& payload,
    const std::optional<ViewportSceneRayHit>& materialHit) {
    ViewportDropMarkerStyle style;
    if (!payload.IsDataType("MATERIAL_ASSET")) {
        return style;
    }

    if (materialHit.has_value() && materialHit->entity.valid()) {
        style.color = IM_COL32(120, 235, 165, 255);
        style.suffix = "  Hit Slot";
        return style;
    }

    const Entity* selectedEntity = state.sceneDocument != nullptr
        ? state.sceneDocument->registry().entity(selection.entityId())
        : nullptr;
    if (selectedEntity != nullptr && selectedEntity->meshRenderer.has_value()) {
        style.color = IM_COL32(245, 185, 85, 255);
        style.suffix = "  Selected Mesh";
        return style;
    }

    style.color = IM_COL32(235, 110, 105, 255);
    style.textColor = IM_COL32(255, 225, 222, 255);
    style.suffix = "  No Mesh Target";
    return style;
}

ViewportDropPreview viewportDropPreviewFromPayload(const EditorRuntimeState& state, const ImGuiPayload& payload) {
    const std::string value = payloadString(payload);
    ViewportDropPreview preview;
    if (payload.IsDataType("PREFAB_ASSET")) {
        preview.active = !value.empty();
        preview.placement = true;
        preview.title = "Place Prefab";
        preview.detail = assetLabelForGuid(state, value);
    } else if (payload.IsDataType("MESH_ASSET")) {
        preview.active = !value.empty();
        preview.placement = true;
        preview.title = ImGui::GetIO().KeyShift ? "Replace Mesh" : "Place Mesh";
        preview.detail = assetLabelForGuid(state, value);
    } else if (payload.IsDataType("MATERIAL_ASSET")) {
        preview.active = !value.empty();
        preview.title = "Assign Material";
        preview.detail = assetLabelForGuid(state, value);
    } else if (payload.IsDataType("ENVIRONMENT_ASSET")) {
        preview.active = !value.empty();
        preview.title = "Assign Environment";
        preview.detail = assetLabelForGuid(state, value);
    } else if (payload.IsDataType("LEVEL_PATH")) {
        const std::filesystem::path path(value);
        preview.active = !value.empty();
        preview.title = "Level Action";
        preview.detail = path.filename().empty() ? value : path.filename().string();
    }
    return preview;
}

std::optional<ImVec2> projectWorldToScreen(
    const EditorRuntimeState& state,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& worldPoint) {
    constexpr float nearPlane = 0.01f;
    return projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(worldPoint, 1.0f)), nearPlane);
}

void drawViewportDropPlacementFootprint(
    const EditorRuntimeState& state,
    const Transform& transform,
    bool forceGrid,
    bool surfaceAlign,
    const ViewportPlacementFootprint& footprint = {}) {
    if (state.camera == nullptr || state.sceneDocument == nullptr) {
        return;
    }

    const glm::mat4 view = editorViewMatrix(*state.camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));
    const glm::mat4 world = transform.localMatrix();
    const std::array<glm::vec3, 4> localCorners = {
        glm::vec3{footprint.minX, 0.0f, footprint.minZ},
        glm::vec3{footprint.maxX, 0.0f, footprint.minZ},
        glm::vec3{footprint.maxX, 0.0f, footprint.maxZ},
        glm::vec3{footprint.minX, 0.0f, footprint.maxZ},
    };

    std::array<ImVec2, 4> screenCorners{};
    for (size_t i = 0; i < localCorners.size(); ++i) {
        const glm::vec3 worldCorner = glm::vec3(world * glm::vec4(localCorners[i], 1.0f));
        const std::optional<ImVec2> screen = projectWorldToScreen(state, view, projection, worldCorner);
        if (!screen.has_value() || !screenPointInsideViewport(state, *screen, 40.0f)) {
            return;
        }
        screenCorners[i] = *screen;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 footprintColor = forceGrid
        ? IM_COL32(115, 175, 255, 210)
        : (surfaceAlign ? IM_COL32(125, 225, 155, 220) : IM_COL32(110, 205, 190, 220));
    for (size_t i = 0; i < screenCorners.size(); ++i) {
        drawList->AddLine(screenCorners[i], screenCorners[(i + 1) % screenCorners.size()], footprintColor, 2.0f);
    }
    drawList->AddLine(screenCorners[0], screenCorners[2], IM_COL32(110, 205, 190, 90), 1.0f);
    drawList->AddLine(screenCorners[1], screenCorners[3], IM_COL32(110, 205, 190, 90), 1.0f);
    if (footprint.assetSized) {
        const std::optional<ImVec2> midNear = projectWorldToScreen(
            state,
            view,
            projection,
            glm::vec3(world * glm::vec4((footprint.minX + footprint.maxX) * 0.5f, 0.0f, footprint.minZ, 1.0f)));
        const std::optional<ImVec2> midFar = projectWorldToScreen(
            state,
            view,
            projection,
            glm::vec3(world * glm::vec4((footprint.minX + footprint.maxX) * 0.5f, 0.0f, footprint.maxZ, 1.0f)));
        const std::optional<ImVec2> midLeft = projectWorldToScreen(
            state,
            view,
            projection,
            glm::vec3(world * glm::vec4(footprint.minX, 0.0f, (footprint.minZ + footprint.maxZ) * 0.5f, 1.0f)));
        const std::optional<ImVec2> midRight = projectWorldToScreen(
            state,
            view,
            projection,
            glm::vec3(world * glm::vec4(footprint.maxX, 0.0f, (footprint.minZ + footprint.maxZ) * 0.5f, 1.0f)));
        if (midNear.has_value() && midFar.has_value()) {
            drawList->AddLine(*midNear, *midFar, IM_COL32(110, 205, 190, 115), 1.0f);
        }
        if (midLeft.has_value() && midRight.has_value()) {
            drawList->AddLine(*midLeft, *midRight, IM_COL32(110, 205, 190, 115), 1.0f);
        }
    }

    if (const std::optional<ImVec2> center = projectWorldToScreen(state, view, projection, transform.position)) {
        drawList->AddCircleFilled(*center, 3.0f, footprintColor, 16);
    }

    if (!forceGrid) {
        const std::optional<ViewportSceneRayHit> hit = viewportSceneRaycastUnderCursor(state);
        if (hit.has_value()) {
            const glm::vec3 normalEnd = hit->position + hit->normal * 0.65f;
            const std::optional<ImVec2> start = projectWorldToScreen(state, view, projection, hit->position);
            const std::optional<ImVec2> end = projectWorldToScreen(state, view, projection, normalEnd);
            if (start.has_value() && end.has_value()) {
                drawList->AddLine(*start, *end, IM_COL32(135, 235, 165, 230), 2.0f);
                drawList->AddCircleFilled(*end, 3.5f, IM_COL32(135, 235, 165, 230), 16);
            }
        }
    }
}

void drawViewportDropPreview(const EditorRuntimeState& state, const EditorSelection& selection, bool snapEnabled, float translationSnap, float yawRadians) {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload == nullptr) {
        return;
    }
    const ViewportDropPreview preview = viewportDropPreviewFromPayload(state, *payload);
    if (!preview.active) {
        return;
    }

    std::ostringstream label;
    label << preview.title;
    if (!preview.detail.empty()) {
        label << "  " << preview.detail;
    }
    const std::optional<ViewportSceneRayHit> materialHit = payload->IsDataType("MATERIAL_ASSET")
        ? viewportSceneRaycastUnderCursor(state)
        : std::nullopt;
    const ViewportDropMarkerStyle markerStyle = viewportDropMarkerStyle(state, selection, *payload, materialHit);
    if (!markerStyle.suffix.empty()) {
        label << markerStyle.suffix;
    }
    bool forceGrid = false;
    bool surfaceAlign = false;
    if (preview.placement) {
        forceGrid = viewportDropForceGridActive(state);
        surfaceAlign = viewportDropSurfaceAlignActive(state, forceGrid);
        if (forceGrid) {
            label << "  Grid";
        } else if (surfaceAlign) {
            label << "  Align";
        }
        label << (snapEnabled ? "  Snap " : "  Free");
        if (snapEnabled) {
            label << std::fixed << std::setprecision(2) << translationSnap;
        }
        if (std::abs(yawRadians) > 0.0001f) {
            label << "  Rot " << std::fixed << std::setprecision(0) << glm::degrees(yawRadians);
        }
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 cursor(state.viewport.mousePosition.x, state.viewport.mousePosition.y);
    constexpr float markerSize = 11.0f;
    const ImU32 markerColor = markerStyle.color;
    drawList->AddCircle(cursor, markerSize, markerColor, 24, 2.0f);
    drawList->AddLine(ImVec2(cursor.x - markerSize - 4.0f, cursor.y), ImVec2(cursor.x + markerSize + 4.0f, cursor.y), markerColor, 2.0f);
    drawList->AddLine(ImVec2(cursor.x, cursor.y - markerSize - 4.0f), ImVec2(cursor.x, cursor.y + markerSize + 4.0f), markerColor, 2.0f);

    if (preview.placement) {
        const std::optional<ViewportPlacementBounds> footprintBounds = footprintBoundsForPlacementPayload(state, *payload);
        const std::optional<ViewportPlacementBounds> placementBounds = payload->IsDataType("MESH_ASSET")
            ? footprintBounds
            : std::nullopt;
        if (const std::optional<Transform> transform = viewportDropPlacementTransform(state, snapEnabled, translationSnap, forceGrid, surfaceAlign, yawRadians, placementBounds)) {
            const ViewportPlacementFootprint footprint = footprintFromBounds(footprintBounds);
            drawViewportDropPlacementFootprint(state, *transform, forceGrid, surfaceAlign, footprint);
        }
    }

    const std::string text = label.str();
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    ImVec2 textMin(cursor.x + 16.0f, cursor.y + 14.0f);
    const float maxX = state.viewport.imageOrigin.x + state.viewport.imageSize.x - textSize.x - 18.0f;
    const float maxY = state.viewport.imageOrigin.y + state.viewport.imageSize.y - textSize.y - 14.0f;
    textMin.x = std::clamp(textMin.x, state.viewport.imageOrigin.x + 8.0f, std::max(state.viewport.imageOrigin.x + 8.0f, maxX));
    textMin.y = std::clamp(textMin.y, state.viewport.imageOrigin.y + 8.0f, std::max(state.viewport.imageOrigin.y + 8.0f, maxY));
    const ImVec2 rectMin(textMin.x - 8.0f, textMin.y - 5.0f);
    const ImVec2 rectMax(textMin.x + textSize.x + 8.0f, textMin.y + textSize.y + 5.0f);
    drawList->AddRectFilled(rectMin, rectMax, IM_COL32(12, 16, 21, 230), 4.0f);
    drawList->AddRect(rectMin, rectMax, IM_COL32(75, 95, 110, 220), 4.0f);
    drawList->AddText(textMin, markerStyle.textColor, text.c_str());
}

void drawViewportPlacementBrushPreview(
    const EditorRuntimeState& state,
    const ViewportPlacementBrushState& brush,
    bool snapEnabled,
    float translationSnap) {
    if (!brush.active()) {
        return;
    }

    const bool forceGrid = viewportDropForceGridActive(state);
    const bool surfaceAlign = viewportDropSurfaceAlignActive(state, forceGrid);
    const std::optional<ViewportPlacementBounds> footprintBounds = footprintBoundsForPlacementBrush(state, brush);
    const std::optional<ViewportPlacementBounds> placementBounds = brush.kind == ViewportPlacementBrushKind::Mesh
        ? footprintBounds
        : std::nullopt;
    const bool scatterActive = brush.kind == ViewportPlacementBrushKind::Mesh && viewportScatterPaletteActive(state);

    std::ostringstream label;
    label << (scatterActive ? "Scatter " : "Place ") << placementBrushKindName(brush.kind) << "  " << assetLabelForGuid(state, brush.guid);
    if (brush.multiPlace) {
        label << "  Multi";
    } else {
        label << "  Duplicate";
    }
    if (forceGrid) {
        label << "  Grid";
    } else if (surfaceAlign) {
        label << "  Align";
    }
    label << (snapEnabled ? "  Snap " : "  Free");
    if (snapEnabled) {
        label << std::fixed << std::setprecision(2) << translationSnap;
    }
    if (std::abs(brush.yawRadians) > 0.0001f) {
        label << "  Rot " << std::fixed << std::setprecision(0) << glm::degrees(brush.yawRadians);
    }
    std::vector<EditorMeshScatterInstancePlacement> scatterPreview;
    if (scatterActive) {
        scatterPreview = buildScatterMeshPlacements(
            state,
            brush.guid,
            brush.yawRadians,
            scatterPaletteSettingsFromPreferences(state.editorPrefs),
            footprintBounds,
            24u);
        label << "  Count " << scatterPreview.size();
    }

    const ImVec2 cursor(state.viewport.mousePosition.x, state.viewport.mousePosition.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr float markerSize = 11.0f;
    const ImU32 markerColor = scatterActive ? IM_COL32(145, 220, 120, 255) : (brush.multiPlace ? IM_COL32(135, 205, 255, 255) : IM_COL32(185, 215, 115, 255));
    drawList->AddCircle(cursor, markerSize, markerColor, 24, 2.0f);
    drawList->AddLine(ImVec2(cursor.x - markerSize - 4.0f, cursor.y), ImVec2(cursor.x + markerSize + 4.0f, cursor.y), markerColor, 2.0f);
    drawList->AddLine(ImVec2(cursor.x, cursor.y - markerSize - 4.0f), ImVec2(cursor.x, cursor.y + markerSize + 4.0f), markerColor, 2.0f);

    if (scatterActive) {
        drawScatterMeshPlacementPreview(state, scatterPreview);
    } else if (const std::optional<Transform> transform = viewportDropPlacementTransform(state, snapEnabled, translationSnap, forceGrid, surfaceAlign, brush.yawRadians, placementBounds)) {
        const ViewportPlacementFootprint footprint = footprintFromBounds(footprintBounds);
        drawViewportDropPlacementFootprint(state, *transform, forceGrid, surfaceAlign, footprint);
    }

    const std::string text = label.str();
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    ImVec2 textMin(cursor.x + 16.0f, cursor.y + 14.0f);
    const float maxX = state.viewport.imageOrigin.x + state.viewport.imageSize.x - textSize.x - 18.0f;
    const float maxY = state.viewport.imageOrigin.y + state.viewport.imageSize.y - textSize.y - 14.0f;
    textMin.x = std::clamp(textMin.x, state.viewport.imageOrigin.x + 8.0f, std::max(state.viewport.imageOrigin.x + 8.0f, maxX));
    textMin.y = std::clamp(textMin.y, state.viewport.imageOrigin.y + 8.0f, std::max(state.viewport.imageOrigin.y + 8.0f, maxY));
    const ImVec2 rectMin(textMin.x - 8.0f, textMin.y - 5.0f);
    const ImVec2 rectMax(textMin.x + textSize.x + 8.0f, textMin.y + textSize.y + 5.0f);
    drawList->AddRectFilled(rectMin, rectMax, IM_COL32(12, 16, 21, 230), 4.0f);
    drawList->AddRect(rectMin, rectMax, IM_COL32(75, 95, 110, 220), 4.0f);
    drawList->AddText(textMin, IM_COL32(232, 238, 244, 255), text.c_str());
}

glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity) {
    if (!entity.parent.valid()) {
        return entity.transform.localMatrix();
    }
    const Entity* parent = registry.entity(entity.parent);
    if (parent == nullptr) {
        return entity.transform.localMatrix();
    }
    return entityWorldMatrix(registry, *parent) * entity.transform.localMatrix();
}

glm::mat4 parentWorldMatrix(const SceneRegistry& registry, const Entity& entity) {
    const Entity* parent = registry.entity(entity.parent);
    return parent != nullptr ? entityWorldMatrix(registry, *parent) : glm::mat4{1.0f};
}

std::vector<EntityId> editableTransformSelection(const EditorRuntimeState& state, const EditorSelection& selection) {
    std::vector<EntityId> ids = selection.selectedEntitiesOr(selection.entityId());
    ids.erase(
        std::remove_if(
            ids.begin(),
            ids.end(),
            [&](EntityId id) {
                const Entity* entity = state.sceneDocument != nullptr ? state.sceneDocument->registry().entity(id) : nullptr;
                return entity == nullptr || entity->locked;
            }),
        ids.end());
    return ids;
}

glm::vec3 selectionPositionCenter(const SceneRegistry& registry, const Entity& activeEntity, const std::vector<EntityId>& ids) {
    glm::vec3 center{0.0f};
    size_t count = 0;
    for (EntityId id : ids) {
        if (const Entity* entity = registry.entity(id)) {
            center += glm::vec3(entityWorldMatrix(registry, *entity)[3]);
            ++count;
        }
    }
    if (count > 0) {
        center /= static_cast<float>(count);
    } else {
        center = glm::vec3(entityWorldMatrix(registry, activeEntity)[3]);
    }
    return center;
}

std::optional<glm::vec3> selectionBoundsCenter(const EditorRuntimeState& state, const std::vector<EntityId>& ids) {
    if (state.sceneDocument == nullptr) {
        return std::nullopt;
    }

    glm::vec3 minBounds{0.0f};
    glm::vec3 maxBounds{0.0f};
    bool hasBounds = false;
    for (EntityId id : ids) {
        const Entity* entity = state.sceneDocument->registry().entity(id);
        if (entity == nullptr) {
            continue;
        }
        const EditorAlignDistributeEntityBounds bounds = alignDistributeBoundsForEntity(state, *entity);
        if (!bounds.available) {
            continue;
        }
        if (!hasBounds) {
            minBounds = bounds.min;
            maxBounds = bounds.max;
            hasBounds = true;
        } else {
            minBounds = glm::min(minBounds, bounds.min);
            maxBounds = glm::max(maxBounds, bounds.max);
        }
    }
    if (!hasBounds) {
        return std::nullopt;
    }
    return (minBounds + maxBounds) * 0.5f;
}

glm::mat4 activeEntityOrientationMatrix(const SceneRegistry& registry, const Entity& activeEntity, glm::vec3 position) {
    glm::vec3 scale{1.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 translation{};
    glm::vec3 skew{};
    glm::vec4 perspective{};
    glm::mat4 result = glm::translate(glm::mat4{1.0f}, position);
    if (glm::decompose(entityWorldMatrix(registry, activeEntity), scale, orientation, translation, skew, perspective)) {
        result *= glm::mat4_cast(orientation);
    }
    return result;
}

glm::mat4 customPivotMatrix(const EditorPivotSettings& pivot) {
    const glm::mat4 translation = glm::translate(glm::mat4{1.0f}, pivot.customPosition);
    const glm::mat4 rotation = glm::mat4_cast(glm::quat(pivot.customRotationEuler));
    return translation * rotation;
}

const char* editorPivotModeLabel(EditorPivotMode mode) {
    switch (mode) {
    case EditorPivotMode::Active: return "Active";
    case EditorPivotMode::SelectionCenter: return "Selection";
    case EditorPivotMode::BoundsCenter: return "Bounds";
    case EditorPivotMode::Custom: return "Custom";
    }
    return "Active";
}

glm::mat4 pivotWorldMatrixForSelection(const EditorRuntimeState& state, const Entity& activeEntity, const std::vector<EntityId>& ids) {
    const SceneRegistry& registry = state.sceneDocument->registry();
    const EditorPivotSettings& pivot = state.sceneDocument->editorPivot();
    if (pivot.mode == EditorPivotMode::Custom) {
        return customPivotMatrix(pivot);
    }
    if (pivot.mode == EditorPivotMode::BoundsCenter) {
        const glm::vec3 center = selectionBoundsCenter(state, ids).value_or(selectionPositionCenter(registry, activeEntity, ids));
        return activeEntityOrientationMatrix(registry, activeEntity, center);
    }
    if (pivot.mode == EditorPivotMode::SelectionCenter) {
        return activeEntityOrientationMatrix(registry, activeEntity, selectionPositionCenter(registry, activeEntity, ids));
    }

    return entityWorldMatrix(registry, activeEntity);
}

void writeLocalTransformFromMatrix(Entity& entity, const glm::mat4& matrix, std::optional<glm::vec3> linkedScaleReference = std::nullopt) {
    glm::vec3 skew{};
    glm::vec4 perspective{};
    glm::quat orientation{};
    glm::vec3 translation{};
    glm::vec3 scale{1.0f};
    if (!glm::decompose(matrix, scale, orientation, translation, skew, perspective)) {
        return;
    }
    entity.transform.position = translation;
    entity.transform.rotationEuler = glm::eulerAngles(glm::normalize(orientation));
    entity.transform.scale = linkedScaleReference.has_value()
        ? editorLinkedScaleFromReference(*linkedScaleReference, scale)
        : scale;
    entity.transform.dirty = true;
}

float activeCameraFov(const SceneDocument& document) {
    const EntityId active = document.activeCamera();
    if (const Entity* cameraEntity = document.registry().entity(active)) {
        if (cameraEntity->camera.has_value()) {
            return cameraEntity->camera->verticalFovRadians;
        }
    }
    return 60.0f * 0.017453292519943295f;
}

bool cameraBasis(const CameraController& camera, glm::vec3& forward, glm::vec3& right, glm::vec3& up) {
    forward = glm::normalize(camera.direction());
    right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
    if (glm::dot(right, right) <= 0.0001f) {
        right = {1.0f, 0.0f, 0.0f};
    } else {
        right = glm::normalize(right);
    }
    up = glm::normalize(glm::cross(right, forward));
    return glm::dot(forward, forward) > 0.0f && glm::dot(up, up) > 0.0f;
}

glm::mat4 editorViewMatrix(const CameraController& camera) {
    glm::vec3 forward{};
    glm::vec3 right{};
    glm::vec3 up{};
    cameraBasis(camera, forward, right, up);
    return glm::lookAtRH(camera.position(), camera.position() + forward, up);
}

glm::mat4 editorProjectionMatrix(float fovY, float aspect) {
    return glm::perspectiveRH_NO(fovY, aspect, 0.01f, 1000.0f);
}

float viewportAspect(const EditorRuntimeState& state) {
    const float w = static_cast<float>(state.viewport.imageSize.x);
    const float h = static_cast<float>(state.viewport.imageSize.y > 0u ? state.viewport.imageSize.y : 1u);
    return w / h;
}

SceneUpdateKind transformUpdateKind(const SceneDocument& document, const Entity& entity) {
    const bool hasMesh = entity.meshRenderer.has_value();
    const bool hasLight = entity.light.has_value();
    const bool hasSun = entity.sun.has_value();
    const bool hasActiveCamera = entity.camera.has_value() && document.activeCamera() == entity.id;
    if (((hasLight || hasSun) && hasMesh) || (hasActiveCamera && hasMesh) || (hasActiveCamera && (hasLight || hasSun))) {
        return SceneUpdateKind::TopologyChanged;
    }
    if (hasActiveCamera) {
        return SceneUpdateKind::CameraOnly;
    }
    if (hasLight || hasSun) {
        return SceneUpdateKind::LightOnly;
    }
    return SceneUpdateKind::TransformOnly;
}

std::optional<uint32_t> instanceForEntity(const EditorRuntimeState& state, EntityId entityId) {
    if (state.instanceEntities == nullptr || !entityId.valid()) {
        return std::nullopt;
    }
    for (uint32_t i = 0; i < state.instanceEntities->size(); ++i) {
        if ((*state.instanceEntities)[i] == entityId) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<EntityId> entityForInstance(const EditorRuntimeState& state, uint32_t instanceId) {
    if (state.instanceEntities == nullptr || instanceId >= state.instanceEntities->size()) {
        return std::nullopt;
    }
    return (*state.instanceEntities)[instanceId];
}

std::optional<ImVec2> projectViewToScreen(
    const EditorRuntimeState& state,
    const glm::mat4& projection,
    glm::vec3 viewPoint,
    float nearPlane) {
    if (viewPoint.z > -nearPlane) {
        return std::nullopt;
    }
    const glm::vec4 clip = projection * glm::vec4(viewPoint, 1.0f);
    if (clip.w <= 0.0f) {
        return std::nullopt;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return ImVec2{
        state.viewport.imageOrigin.x + (ndc.x * 0.5f + 0.5f) * state.viewport.imageSize.x,
        state.viewport.imageOrigin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * state.viewport.imageSize.y,
    };
}

bool isNonMeshActor(const Entity& entity) {
    return entity.camera.has_value() || entity.light.has_value() || entity.sun.has_value() ||
        entity.environmentLight.has_value() || entity.skyAtmosphere.has_value() || entity.heightFog.has_value() ||
        entity.volumetricCloud.has_value() || entity.postProcessVolume.has_value() || entity.cameraPostProcess.has_value();
}

const char* actorOverlayLabel(const Entity& entity) {
    if (entity.camera.has_value()) return "Camera";
    if (entity.sun.has_value()) return "Sun";
    if (entity.light.has_value()) return "Light";
    if (entity.environmentLight.has_value()) return "Environment";
    if (entity.skyAtmosphere.has_value()) return "Sky";
    if (entity.heightFog.has_value()) return "Fog";
    if (entity.volumetricCloud.has_value()) return "Cloud";
    if (entity.postProcessVolume.has_value()) return "Post";
    if (entity.cameraPostProcess.has_value()) return "Camera FX";
    return "Actor";
}

ImU32 actorOverlayColor(const Entity& entity, bool selected) {
    const int alpha = selected ? 255 : 210;
    if (entity.camera.has_value()) return IM_COL32(105, 180, 255, alpha);
    if (entity.sun.has_value()) return IM_COL32(255, 213, 92, alpha);
    if (entity.light.has_value()) return IM_COL32(255, 188, 78, alpha);
    if (entity.environmentLight.has_value()) return IM_COL32(101, 204, 190, alpha);
    if (entity.skyAtmosphere.has_value()) return IM_COL32(114, 169, 255, alpha);
    if (entity.heightFog.has_value()) return IM_COL32(166, 196, 210, alpha);
    if (entity.volumetricCloud.has_value()) return IM_COL32(190, 205, 225, alpha);
    if (entity.postProcessVolume.has_value() || entity.cameraPostProcess.has_value()) return IM_COL32(206, 158, 255, alpha);
    return IM_COL32(185, 195, 210, alpha);
}

std::optional<ImVec2> entityScreenCenter(
    const EditorRuntimeState& state,
    const glm::mat4& view,
    const glm::mat4& projection,
    const Entity& entity) {
    constexpr float nearPlane = 0.01f;
    const glm::mat4 world = entityWorldMatrix(state.sceneDocument->registry(), entity);
    return projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(glm::vec3(world[3]), 1.0f)), nearPlane);
}

bool screenPointInsideViewport(const EditorRuntimeState& state, ImVec2 point, float padding = 18.0f) {
    return point.x >= state.viewport.imageOrigin.x - padding &&
        point.y >= state.viewport.imageOrigin.y - padding &&
        point.x <= state.viewport.imageOrigin.x + state.viewport.imageSize.x + padding &&
        point.y <= state.viewport.imageOrigin.y + state.viewport.imageSize.y + padding;
}

bool rayTriangleIntersection(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const glm::vec3& a,
    const glm::vec3& b,
    const glm::vec3& c,
    float& t) {
    constexpr float epsilon = 0.000001f;
    const glm::vec3 edge1 = b - a;
    const glm::vec3 edge2 = c - a;
    const glm::vec3 p = glm::cross(direction, edge2);
    const float det = glm::dot(edge1, p);
    if (std::abs(det) <= epsilon) {
        return false;
    }
    const float invDet = 1.0f / det;
    const glm::vec3 s = origin - a;
    const float u = invDet * glm::dot(s, p);
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::vec3 q = glm::cross(s, edge1);
    const float v = invDet * glm::dot(direction, q);
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    t = invDet * glm::dot(edge2, q);
    return t > epsilon && std::isfinite(t);
}

glm::vec3 triangleNormalFacingRay(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& rayDir) {
    glm::vec3 normal = glm::cross(b - a, c - a);
    const float len2 = glm::dot(normal, normal);
    if (len2 <= 0.0000000001f || !std::isfinite(len2)) {
        return {0.0f, 1.0f, 0.0f};
    }
    normal /= std::sqrt(len2);
    return glm::dot(normal, rayDir) > 0.0f ? -normal : normal;
}

glm::vec3 rotationEulerAligningUpToNormal(glm::vec3 normal) {
    const float len2 = glm::dot(normal, normal);
    if (len2 <= 0.0000000001f || !std::isfinite(len2)) {
        return glm::vec3(0.0f);
    }
    normal /= std::sqrt(len2);

    constexpr float pi = 3.14159265358979323846f;
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const float c = std::clamp(glm::dot(up, normal), -1.0f, 1.0f);
    if (c > 0.9999f) {
        return glm::vec3(0.0f);
    }
    if (c < -0.9999f) {
        return glm::eulerAngles(glm::angleAxis(pi, glm::vec3{1.0f, 0.0f, 0.0f}));
    }
    const glm::vec3 axis = glm::normalize(glm::cross(up, normal));
    return glm::eulerAngles(glm::normalize(glm::angleAxis(std::acos(c), axis)));
}

std::optional<ViewportSceneRayHit> viewportSceneRaycast(
    const EditorRuntimeState& state,
    const glm::vec3& origin,
    const glm::vec3& rayDir,
    const std::vector<EntityId>& excludedEntities) {
    if (state.sceneDocument == nullptr || state.assets == nullptr) {
        return std::nullopt;
    }

    float nearestT = std::numeric_limits<float>::max();
    ViewportSceneRayHit nearestHit{};
    const SceneRegistry& registry = state.sceneDocument->registry();
    for (const Entity* entity : registry.entities()) {
        if (entity == nullptr || !entity->visible || !entity->meshRenderer.has_value()) {
            continue;
        }
        if (std::find(excludedEntities.begin(), excludedEntities.end(), entity->id) != excludedEntities.end()) {
            continue;
        }
        const MeshRenderer& renderer = *entity->meshRenderer;
        if (!renderer.visible || !renderer.visibleToCamera || !renderer.mesh.valid()) {
            continue;
        }
        const MeshAsset* mesh = state.assets->mesh(renderer.mesh);
        if (mesh == nullptr || mesh->vertices.empty() || mesh->primitives.empty()) {
            continue;
        }

        const glm::mat4 world = entityWorldMatrix(registry, *entity);
        for (uint32_t primitiveIndex = 0; primitiveIndex < mesh->primitives.size(); ++primitiveIndex) {
            const MeshPrimitiveAsset& primitive = mesh->primitives[primitiveIndex];
            const uint32_t indexEnd = primitive.firstIndex + primitive.indexCount;
            if (primitive.indexCount >= 3u && indexEnd <= mesh->indices.size()) {
                for (uint32_t i = primitive.firstIndex; i + 2u < indexEnd; i += 3u) {
                    const uint32_t i0 = mesh->indices[i];
                    const uint32_t i1 = mesh->indices[i + 1u];
                    const uint32_t i2 = mesh->indices[i + 2u];
                    if (i0 >= mesh->vertices.size() || i1 >= mesh->vertices.size() || i2 >= mesh->vertices.size()) {
                        continue;
                    }
                    const glm::vec3 a = glm::vec3(world * glm::vec4(mesh->vertices[i0].position, 1.0f));
                    const glm::vec3 b = glm::vec3(world * glm::vec4(mesh->vertices[i1].position, 1.0f));
                    const glm::vec3 c = glm::vec3(world * glm::vec4(mesh->vertices[i2].position, 1.0f));
                    float t = 0.0f;
                    if (rayTriangleIntersection(origin, rayDir, a, b, c, t) && t < nearestT) {
                        nearestT = t;
                        nearestHit.position = origin + rayDir * t;
                        nearestHit.normal = triangleNormalFacingRay(a, b, c, rayDir);
                        nearestHit.entity = entity->id;
                        nearestHit.primitiveIndex = primitiveIndex;
                    }
                }
            } else if (primitive.vertexCount >= 3u && primitive.firstVertex + primitive.vertexCount <= mesh->vertices.size()) {
                const uint32_t vertexEnd = primitive.firstVertex + primitive.vertexCount;
                for (uint32_t i = primitive.firstVertex; i + 2u < vertexEnd; i += 3u) {
                    const glm::vec3 a = glm::vec3(world * glm::vec4(mesh->vertices[i].position, 1.0f));
                    const glm::vec3 b = glm::vec3(world * glm::vec4(mesh->vertices[i + 1u].position, 1.0f));
                    const glm::vec3 c = glm::vec3(world * glm::vec4(mesh->vertices[i + 2u].position, 1.0f));
                    float t = 0.0f;
                    if (rayTriangleIntersection(origin, rayDir, a, b, c, t) && t < nearestT) {
                        nearestT = t;
                        nearestHit.position = origin + rayDir * t;
                        nearestHit.normal = triangleNormalFacingRay(a, b, c, rayDir);
                        nearestHit.entity = entity->id;
                        nearestHit.primitiveIndex = primitiveIndex;
                    }
                }
            }
        }
    }

    return nearestHit.entity.valid() ? std::optional<ViewportSceneRayHit>(nearestHit) : std::nullopt;
}

bool viewportRayFromMouse(const EditorRuntimeState& state, glm::vec3& origin, glm::vec3& rayDir) {
    if (state.camera == nullptr || state.viewport.imageSize.x <= 0.0f || state.viewport.imageSize.y <= 0.0f) {
        return false;
    }

    glm::vec3 forward{};
    glm::vec3 right{};
    glm::vec3 up{};
    if (!cameraBasis(*state.camera, forward, right, up)) {
        return false;
    }

    const float fovY = state.sceneDocument != nullptr ? activeCameraFov(*state.sceneDocument) : glm::radians(60.0f);
    const float tanHalfFov = std::tan(fovY * 0.5f);
    const float ndcX = state.viewport.mouseUv.x * 2.0f - 1.0f;
    const float ndcY = 1.0f - state.viewport.mouseUv.y * 2.0f;
    rayDir = glm::normalize(forward + right * (ndcX * tanHalfFov * viewportAspect(state)) + up * (ndcY * tanHalfFov));
    origin = state.camera->position();
    return std::isfinite(rayDir.x) && std::isfinite(rayDir.y) && std::isfinite(rayDir.z);
}

std::optional<ViewportSceneRayHit> viewportSceneRaycastUnderCursor(const EditorRuntimeState& state) {
    glm::vec3 origin{};
    glm::vec3 rayDir{};
    if (!viewportRayFromMouse(state, origin, rayDir)) {
        return std::nullopt;
    }
    return viewportSceneRaycast(state, origin, rayDir);
}

float snappedPlacementCoordinate(float value, float step) {
    if (!std::isfinite(value) || !std::isfinite(step) || step <= 0.0001f) {
        return value;
    }
    return std::round(value / step) * step;
}

float wrappedPlacementYaw(float yawRadians) {
    constexpr float pi = 3.14159265358979323846f;
    constexpr float twoPi = pi * 2.0f;
    while (yawRadians > pi) {
        yawRadians -= twoPi;
    }
    while (yawRadians < -pi) {
        yawRadians += twoPi;
    }
    return yawRadians;
}

glm::vec3 normalizedPlacementNormal(glm::vec3 normal) {
    const float len2 = glm::dot(normal, normal);
    if (len2 <= 0.0000000001f || !std::isfinite(len2)) {
        return {0.0f, 1.0f, 0.0f};
    }
    return normal / std::sqrt(len2);
}

struct EditorSurfaceSnapSettings {
    bool enabled = false;
    bool alignToNormal = true;
    bool preserveYaw = true;
    float offset = 0.0f;
    bool snapBoundsBottom = true;
    int axisConstraint = 0;
};

struct EditorSceneQueryHit {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    bool usedSceneSurface = false;
    bool usedGridFallback = false;
};

struct EditorSceneQuery {
    const EditorRuntimeState& state;
    std::vector<EntityId> excludedEntities;

    std::optional<EditorSceneQueryHit> surfaceOrGridUnderCursor(bool forceGrid, bool allowSurface) const {
        glm::vec3 origin{};
        glm::vec3 rayDir{};
        if (!viewportRayFromMouse(state, origin, rayDir)) {
            return std::nullopt;
        }

        if (!forceGrid && allowSurface) {
            if (const std::optional<ViewportSceneRayHit> sceneHit = viewportSceneRaycast(state, origin, rayDir, excludedEntities)) {
                return EditorSceneQueryHit{
                    .position = sceneHit->position,
                    .normal = normalizedPlacementNormal(sceneHit->normal),
                    .usedSceneSurface = true,
                    .usedGridFallback = false,
                };
            }
        }

        glm::vec3 position = origin + rayDir * 5.0f;
        if (std::abs(rayDir.y) > 0.0001f) {
            const float t = -origin.y / rayDir.y;
            if (t > 0.0f && std::isfinite(t)) {
                position = origin + rayDir * t;
            }
            position.y = 0.0f;
        }
        return EditorSceneQueryHit{
            .position = position,
            .normal = glm::vec3{0.0f, 1.0f, 0.0f},
            .usedSceneSurface = false,
            .usedGridFallback = true,
        };
    }
};

EditorSurfaceSnapSettings surfaceSnapSettingsFromPreferences(const EditorPreferences* preferences) {
    EditorSurfaceSnapSettings settings;
    if (preferences == nullptr) {
        return settings;
    }
    settings.enabled = preferences->viewportSurfaceSnappingEnabled;
    settings.alignToNormal = preferences->viewportSurfaceSnapAlignToNormal;
    settings.preserveYaw = preferences->viewportSurfaceSnapPreserveYaw;
    settings.offset = std::clamp(preferences->viewportSurfaceSnapOffset, -100.0f, 100.0f);
    settings.snapBoundsBottom = preferences->viewportSurfaceSnapBoundsBottom;
    settings.axisConstraint = std::clamp(preferences->viewportSurfaceSnapAxisConstraint, 0, 3);
    return settings;
}

const char* surfaceSnapAxisConstraintLabel(int axisConstraint) {
    switch (axisConstraint) {
    case 1: return "X";
    case 2: return "Y";
    case 3: return "Z";
    default: return "None";
    }
}

glm::vec3 constrainedSurfaceSnapPosition(glm::vec3 currentPosition, glm::vec3 snappedPosition, int axisConstraint) {
    switch (axisConstraint) {
    case 1:
        return {snappedPosition.x, currentPosition.y, currentPosition.z};
    case 2:
        return {currentPosition.x, snappedPosition.y, currentPosition.z};
    case 3:
        return {currentPosition.x, currentPosition.y, snappedPosition.z};
    default:
        return snappedPosition;
    }
}

glm::quat surfaceSnapRotation(glm::quat currentRotation, glm::vec3 normal, const EditorSurfaceSnapSettings& settings) {
    if (!settings.alignToNormal) {
        return glm::normalize(currentRotation);
    }
    glm::quat aligned = glm::quat(rotationEulerAligningUpToNormal(normalizedPlacementNormal(normal)));
    if (settings.preserveYaw) {
        const float yaw = glm::eulerAngles(glm::normalize(currentRotation)).y;
        aligned = aligned * glm::angleAxis(yaw, glm::vec3{0.0f, 1.0f, 0.0f});
    }
    return glm::normalize(aligned);
}

void includeEntityBoundsForWorldMatrix(
    const EditorRuntimeState& state,
    const Entity& entity,
    const glm::mat4& world,
    ViewportPlacementBounds& outBounds,
    bool& hasPoint) {
    if (state.assets != nullptr && entity.meshRenderer.has_value()) {
        const std::optional<ViewportPlacementBounds> localBounds = meshPlacementBounds(state.assets, entity.meshRenderer->mesh);
        if (localBounds.has_value()) {
            includeTransformedBoundsCorners(outBounds, *localBounds, world, hasPoint);
            return;
        }
    }
    const glm::vec3 position = glm::vec3(world[3]);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        return;
    }
    if (!hasPoint) {
        outBounds.min = position;
        outBounds.max = position;
        hasPoint = true;
    } else {
        outBounds.min = glm::min(outBounds.min, position);
        outBounds.max = glm::max(outBounds.max, position);
    }
}

std::optional<ViewportPlacementBounds> currentGizmoSelectionBounds(
    const EditorRuntimeState& state,
    const Entity& activeEntity,
    const glm::mat4& currentWorld,
    const glm::mat4& originalGroupWorld,
    const std::vector<ViewportPanel::GroupGizmoOriginal>& originals) {
    if (state.sceneDocument == nullptr) {
        return std::nullopt;
    }

    ViewportPlacementBounds bounds;
    bool hasPoint = false;
    if (!originals.empty()) {
        const glm::mat4 delta = currentWorld * glm::inverse(originalGroupWorld);
        for (const ViewportPanel::GroupGizmoOriginal& original : originals) {
            if (const Entity* entity = state.sceneDocument->registry().entity(original.entity)) {
                includeEntityBoundsForWorldMatrix(state, *entity, delta * original.world, bounds, hasPoint);
            }
        }
    } else {
        includeEntityBoundsForWorldMatrix(state, activeEntity, currentWorld, bounds, hasPoint);
    }
    return hasPoint ? std::optional<ViewportPlacementBounds>{bounds} : std::nullopt;
}

glm::vec3 boundsBottomSurfaceSnapPivot(
    glm::vec3 currentPivot,
    glm::vec3 snappedSurfacePoint,
    glm::vec3 normal,
    const ViewportPlacementBounds& bounds) {
    normal = normalizedPlacementNormal(normal);
    const std::array<glm::vec3, 8> corners = {
        glm::vec3{bounds.min.x, bounds.min.y, bounds.min.z},
        glm::vec3{bounds.max.x, bounds.min.y, bounds.min.z},
        glm::vec3{bounds.min.x, bounds.max.y, bounds.min.z},
        glm::vec3{bounds.max.x, bounds.max.y, bounds.min.z},
        glm::vec3{bounds.min.x, bounds.min.y, bounds.max.z},
        glm::vec3{bounds.max.x, bounds.min.y, bounds.max.z},
        glm::vec3{bounds.min.x, bounds.max.y, bounds.max.z},
        glm::vec3{bounds.max.x, bounds.max.y, bounds.max.z},
    };
    float minProjection = std::numeric_limits<float>::max();
    for (const glm::vec3& corner : corners) {
        const float projection = glm::dot(corner, normal);
        if (std::isfinite(projection)) {
            minProjection = std::min(minProjection, projection);
        }
    }
    if (!std::isfinite(minProjection)) {
        return snappedSurfacePoint;
    }
    const float pivotAboveSupport = glm::dot(currentPivot, normal) - minProjection;
    return snappedSurfacePoint + normal * pivotAboveSupport;
}

std::optional<glm::mat4> surfaceSnappedGizmoWorldMatrix(
    const EditorRuntimeState& state,
    const Entity& activeEntity,
    const glm::mat4& currentWorld,
    const glm::mat4& originalGroupWorld,
    const std::vector<ViewportPanel::GroupGizmoOriginal>& originals,
    const std::vector<EntityId>& excludedEntities,
    const EditorSurfaceSnapSettings& settings) {
    if (!settings.enabled) {
        return std::nullopt;
    }
    const std::optional<EditorSceneQueryHit> hit = EditorSceneQuery{state, excludedEntities}.surfaceOrGridUnderCursor(false, true);
    if (!hit.has_value()) {
        return std::nullopt;
    }

    glm::vec3 skew{};
    glm::vec4 perspective{};
    glm::quat currentRotation{};
    glm::vec3 currentPosition{};
    glm::vec3 currentScale{1.0f};
    if (!glm::decompose(currentWorld, currentScale, currentRotation, currentPosition, skew, perspective)) {
        return std::nullopt;
    }

    glm::vec3 snappedPosition = hit->position + normalizedPlacementNormal(hit->normal) * settings.offset;
    if (settings.snapBoundsBottom) {
        if (const std::optional<ViewportPlacementBounds> bounds = currentGizmoSelectionBounds(state, activeEntity, currentWorld, originalGroupWorld, originals)) {
            snappedPosition = boundsBottomSurfaceSnapPivot(currentPosition, snappedPosition, hit->normal, *bounds);
        }
    }
    snappedPosition = constrainedSurfaceSnapPosition(currentPosition, snappedPosition, settings.axisConstraint);

    const glm::quat snappedRotation = surfaceSnapRotation(currentRotation, hit->normal, settings);
    return glm::translate(glm::mat4{1.0f}, snappedPosition) *
        glm::mat4_cast(snappedRotation) *
        glm::scale(glm::mat4{1.0f}, currentScale);
}

glm::vec3 placementBoundsSupportOffset(
    const ViewportPlacementBounds& bounds,
    const glm::vec3& rotationEuler,
    glm::vec3 placementNormal) {
    placementNormal = normalizedPlacementNormal(placementNormal);
    const glm::mat3 rotation = glm::mat3_cast(glm::quat(rotationEuler));
    const std::array<glm::vec3, 8> corners = {
        glm::vec3{bounds.min.x, bounds.min.y, bounds.min.z},
        glm::vec3{bounds.max.x, bounds.min.y, bounds.min.z},
        glm::vec3{bounds.min.x, bounds.max.y, bounds.min.z},
        glm::vec3{bounds.max.x, bounds.max.y, bounds.min.z},
        glm::vec3{bounds.min.x, bounds.min.y, bounds.max.z},
        glm::vec3{bounds.max.x, bounds.min.y, bounds.max.z},
        glm::vec3{bounds.min.x, bounds.max.y, bounds.max.z},
        glm::vec3{bounds.max.x, bounds.max.y, bounds.max.z},
    };

    float minProjection = std::numeric_limits<float>::max();
    for (const glm::vec3& corner : corners) {
        const float projection = glm::dot(rotation * corner, placementNormal);
        if (std::isfinite(projection)) {
            minProjection = std::min(minProjection, projection);
        }
    }
    if (!std::isfinite(minProjection)) {
        return glm::vec3(0.0f);
    }
    return placementNormal * -minProjection;
}

bool viewportScatterPaletteActive(const EditorRuntimeState& state) {
    return state.editorPrefs != nullptr && state.editorPrefs->viewportScatterPaletteByDefault;
}

ScatterPaletteSettings scatterPaletteSettingsFromPreferences(const EditorPreferences* preferences) {
    ScatterPaletteSettings settings;
    if (preferences == nullptr) {
        return settings;
    }
    settings.density = std::clamp(preferences->viewportScatterPaletteDensity, 0.0f, 10000.0f);
    settings.slopeMinDegrees = std::clamp(preferences->viewportScatterPaletteSlopeMinDegrees, 0.0f, 180.0f);
    settings.slopeMaxDegrees = std::clamp(preferences->viewportScatterPaletteSlopeMaxDegrees, 0.0f, 180.0f);
    if (settings.slopeMaxDegrees < settings.slopeMinDegrees) {
        std::swap(settings.slopeMinDegrees, settings.slopeMaxDegrees);
    }
    settings.heightMin = preferences->viewportScatterPaletteHeightMin;
    settings.heightMax = preferences->viewportScatterPaletteHeightMax;
    if (settings.heightMax < settings.heightMin) {
        std::swap(settings.heightMin, settings.heightMax);
    }
    settings.scaleMin = std::clamp(preferences->viewportScatterPaletteScaleMin, 0.001f, 1000.0f);
    settings.scaleMax = std::clamp(preferences->viewportScatterPaletteScaleMax, 0.001f, 1000.0f);
    if (settings.scaleMax < settings.scaleMin) {
        std::swap(settings.scaleMin, settings.scaleMax);
    }
    settings.yawRandomDegrees = std::clamp(preferences->viewportScatterPaletteYawRandomDegrees, 0.0f, 360.0f);
    settings.seed = preferences->viewportScatterPaletteSeed;
    settings.spacing = std::clamp(preferences->viewportScatterPaletteSpacing, 0.001f, 10000.0f);
    settings.collisionRadius = std::clamp(preferences->viewportScatterPaletteCollisionRadius, 0.0f, 10000.0f);
    settings.surfaceAlignment = preferences->viewportScatterPaletteSurfaceAlignment;
    return settings;
}

uint32_t scatterNextRandom(uint32_t& state) {
    state += 0x9e3779b9u;
    uint32_t value = state;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float scatterRandom01(uint32_t& state) {
    return static_cast<float>(scatterNextRandom(state) >> 8u) * (1.0f / 16777216.0f);
}

uint32_t scatterSurfaceSeed(uint32_t seed, const EditorSceneQueryHit& hit) {
    const glm::ivec3 quantized = glm::ivec3(glm::floor(hit.position * 1000.0f));
    uint32_t state = seed ^ 0xa511e9b3u;
    state ^= static_cast<uint32_t>(quantized.x) * 0x85ebca6bu;
    state ^= static_cast<uint32_t>(quantized.y) * 0xc2b2ae35u;
    state ^= static_cast<uint32_t>(quantized.z) * 0x27d4eb2fu;
    return state;
}

void scatterSurfaceBasis(glm::vec3 normal, glm::vec3& tangent, glm::vec3& bitangent) {
    normal = normalizedPlacementNormal(normal);
    const glm::vec3 reference = std::abs(normal.y) < 0.95f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
    tangent = glm::normalize(glm::cross(reference, normal));
    bitangent = glm::normalize(glm::cross(normal, tangent));
}

bool scatterCandidatePassesFilters(const ScatterPaletteSettings& settings, const glm::vec3& position, glm::vec3 normal) {
    normal = normalizedPlacementNormal(normal);
    const float slopeDegrees = glm::degrees(std::acos(std::clamp(glm::dot(normal, glm::vec3{0.0f, 1.0f, 0.0f}), -1.0f, 1.0f)));
    return slopeDegrees >= settings.slopeMinDegrees &&
        slopeDegrees <= settings.slopeMaxDegrees &&
        position.y >= settings.heightMin &&
        position.y <= settings.heightMax;
}

std::vector<EditorMeshScatterInstancePlacement> buildScatterMeshPlacements(
    const EditorRuntimeState& state,
    const AssetGuid& meshGuid,
    float yawRadians,
    const ScatterPaletteSettings& settings,
    std::optional<ViewportPlacementBounds> meshBounds,
    size_t maxInstances) {
    if (meshGuid.empty()) {
        return {};
    }
    const std::optional<EditorSceneQueryHit> anchor = EditorSceneQuery{state, {}}.surfaceOrGridUnderCursor(false, true);
    if (!anchor.has_value() || !scatterCandidatePassesFilters(settings, anchor->position, anchor->normal)) {
        return {};
    }

    const size_t targetCount = std::max<size_t>(1u, std::min<size_t>(96u, static_cast<size_t>(std::round(std::max(0.1f, settings.density) * 8.0f))));
    const size_t requestedCount = maxInstances > 0 ? std::min(targetCount, maxInstances) : targetCount;
    const float scatterRadius = std::max(settings.spacing, settings.collisionRadius * 2.0f) * std::sqrt(static_cast<float>(targetCount)) * 0.5f;
    const float minDistance = settings.collisionRadius * 2.0f;
    glm::vec3 tangent{};
    glm::vec3 bitangent{};
    scatterSurfaceBasis(anchor->normal, tangent, bitangent);

    uint32_t rng = scatterSurfaceSeed(settings.seed, *anchor);
    std::vector<glm::vec3> acceptedPositions;
    std::vector<EditorMeshScatterInstancePlacement> instances;
    acceptedPositions.reserve(requestedCount);
    instances.reserve(requestedCount);
    constexpr float twoPi = 6.28318530717958647692f;
    const size_t attemptLimit = std::max<size_t>(requestedCount * 8u, requestedCount + 4u);
    for (size_t attempt = 0; attempt < attemptLimit && instances.size() < requestedCount; ++attempt) {
        const float angle = scatterRandom01(rng) * twoPi;
        const float radius = std::sqrt(scatterRandom01(rng)) * scatterRadius;
        const glm::vec3 candidate = anchor->position + tangent * (std::cos(angle) * radius) + bitangent * (std::sin(angle) * radius);
        glm::vec3 position = candidate;
        glm::vec3 normal = anchor->normal;
        if (settings.surfaceAlignment && anchor->usedSceneSurface) {
            const glm::vec3 rayOrigin = candidate + normalizedPlacementNormal(anchor->normal) * 500.0f;
            const glm::vec3 rayDir = -normalizedPlacementNormal(anchor->normal);
            if (const std::optional<ViewportSceneRayHit> hit = viewportSceneRaycast(state, rayOrigin, rayDir)) {
                position = hit->position;
                normal = hit->normal;
            }
        }
        if (!scatterCandidatePassesFilters(settings, position, normal)) {
            continue;
        }
        bool collides = false;
        if (minDistance > 0.0f) {
            for (const glm::vec3& existing : acceptedPositions) {
                if (glm::length(existing - position) < minDistance) {
                    collides = true;
                    break;
                }
            }
        }
        if (collides) {
            continue;
        }

        const float randomYaw = glm::radians((scatterRandom01(rng) * 2.0f - 1.0f) * settings.yawRandomDegrees);
        const float randomScale = settings.scaleMin + (settings.scaleMax - settings.scaleMin) * scatterRandom01(rng);
        glm::quat rotation = settings.surfaceAlignment
            ? glm::angleAxis(yawRadians + randomYaw, normalizedPlacementNormal(normal)) * glm::quat(rotationEulerAligningUpToNormal(normal))
            : glm::angleAxis(yawRadians + randomYaw, glm::vec3{0.0f, 1.0f, 0.0f});
        const glm::vec3 rotationEuler = glm::eulerAngles(glm::normalize(rotation));
        if (meshBounds.has_value()) {
            ViewportPlacementBounds scaledBounds = *meshBounds;
            scaledBounds.min *= randomScale;
            scaledBounds.max *= randomScale;
            position += placementBoundsSupportOffset(scaledBounds, rotationEuler, normal);
        }

        Transform transform;
        transform.position = position;
        transform.rotationEuler = rotationEuler;
        transform.scale = glm::vec3(randomScale);
        transform.dirty = true;
        instances.push_back(EditorMeshScatterInstancePlacement{
            .meshGuid = meshGuid,
            .materialGuid = {},
            .transform = transform,
        });
        acceptedPositions.push_back(position);
    }
    return instances;
}

void drawScatterMeshPlacementPreview(
    const EditorRuntimeState& state,
    const std::vector<EditorMeshScatterInstancePlacement>& instances) {
    if (state.camera == nullptr || state.sceneDocument == nullptr || instances.empty()) {
        return;
    }
    const glm::mat4 view = editorViewMatrix(*state.camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (const EditorMeshScatterInstancePlacement& instance : instances) {
        const std::optional<ImVec2> center = projectWorldToScreen(state, view, projection, instance.transform.position);
        if (!center.has_value() || !screenPointInsideViewport(state, *center, 40.0f)) {
            continue;
        }
        drawList->AddCircle(*center, 5.5f, IM_COL32(145, 220, 120, 230), 18, 1.5f);
        drawList->AddCircleFilled(*center, 2.0f, IM_COL32(145, 220, 120, 230), 12);
    }
}

std::optional<Transform> viewportDropPlacementTransform(
    const EditorRuntimeState& state,
    bool snapEnabled,
    float translationSnap,
    bool forceGrid,
    bool surfaceAlign,
    float yawRadians,
    std::optional<ViewportPlacementBounds> bounds) {
    const std::optional<EditorSceneQueryHit> queryHit = EditorSceneQuery{state, {}}.surfaceOrGridUnderCursor(forceGrid, true);
    if (!queryHit.has_value()) {
        return std::nullopt;
    }

    glm::vec3 position = queryHit->position;
    glm::vec3 rotationEuler{0.0f};
    const glm::vec3 placementNormal = normalizedPlacementNormal(queryHit->normal);
    if (surfaceAlign && queryHit->usedSceneSurface) {
        rotationEuler = rotationEulerAligningUpToNormal(placementNormal);
    }
    if (snapEnabled) {
        position.x = snappedPlacementCoordinate(position.x, translationSnap);
        position.y = snappedPlacementCoordinate(position.y, translationSnap);
        position.z = snappedPlacementCoordinate(position.z, translationSnap);
    }

    if (std::isfinite(yawRadians) && std::abs(yawRadians) > 0.0001f) {
        const glm::quat placementRotation = glm::quat(rotationEuler) * glm::angleAxis(yawRadians, glm::vec3{0.0f, 1.0f, 0.0f});
        rotationEuler = glm::eulerAngles(glm::normalize(placementRotation));
    }

    if (bounds.has_value()) {
        const glm::vec3 localCenter = (bounds->min + bounds->max) * 0.5f;
        const glm::quat rotation = glm::quat(rotationEuler);
        const glm::vec3 planarOffset = rotation * glm::vec3(localCenter.x, 0.0f, localCenter.z);
        position -= planarOffset - placementNormal * glm::dot(planarOffset, placementNormal);
        position += placementBoundsSupportOffset(*bounds, rotationEuler, placementNormal);
    }

    Transform transform;
    transform.position = position;
    transform.rotationEuler = rotationEuler;
    transform.scale = glm::vec3(1.0f);
    transform.dirty = true;
    return transform;
}

void drawActorIcon(ImDrawList* drawList, EditorGlyphIcon icon, ImVec2 center, ImU32 color, bool selected) {
    const float size = selected ? 25.0f : 21.0f;
    const ImVec2 min(center.x - size * 0.5f, center.y - size * 0.5f);
    const ImVec2 max(center.x + size * 0.5f, center.y + size * 0.5f);
    drawList->AddRectFilled(min, max, selected ? IM_COL32(10, 15, 22, 235) : IM_COL32(10, 14, 20, 190), 4.0f);
    drawList->AddRect(min, max, selected ? color : IM_COL32(75, 88, 105, 180), 4.0f, 0, selected ? 2.0f : 1.0f);
    editorDrawIconGlyph(icon, ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), color);
}

void drawActorIconsOverlay(const EditorRuntimeState& state, const EditorSelection& selection) {
    if (state.sceneDocument == nullptr || state.camera == nullptr) {
        return;
    }
    const glm::mat4 view = editorViewMatrix(*state.camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (const Entity* entity : state.sceneDocument->registry().entities()) {
        if (entity == nullptr || !entity->visible || !isNonMeshActor(*entity)) {
            continue;
        }
        const std::optional<ImVec2> center = entityScreenCenter(state, view, projection, *entity);
        if (!center.has_value() || !screenPointInsideViewport(state, *center)) {
            continue;
        }
        const bool selected = selection.entityId() == entity->id;
        drawActorIcon(drawList, editorGlyphForEntity(*entity), *center, actorOverlayColor(*entity, selected), selected);
    }
}

std::optional<EntityId> actorIconUnderCursor(const EditorRuntimeState& state) {
    if (state.sceneDocument == nullptr || state.camera == nullptr) {
        return std::nullopt;
    }
    const glm::mat4 view = editorViewMatrix(*state.camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));
    const ImVec2 mouse(state.viewport.mousePosition.x, state.viewport.mousePosition.y);
    constexpr float pickRadius = 14.0f;
    constexpr float pickRadiusSq = pickRadius * pickRadius;
    float bestDistanceSq = pickRadiusSq;
    EntityId best{};
    for (const Entity* entity : state.sceneDocument->registry().entities()) {
        if (entity == nullptr || !entity->visible || entity->locked || !isNonMeshActor(*entity)) {
            continue;
        }
        const std::optional<ImVec2> center = entityScreenCenter(state, view, projection, *entity);
        if (!center.has_value() || !screenPointInsideViewport(state, *center)) {
            continue;
        }
        const float dx = center->x - mouse.x;
        const float dy = center->y - mouse.y;
        const float distanceSq = dx * dx + dy * dy;
        if (distanceSq <= bestDistanceSq) {
            bestDistanceSq = distanceSq;
            best = entity->id;
        }
    }
    return best.valid() ? std::optional<EntityId>(best) : std::nullopt;
}

void drawActorLabel(ImDrawList* drawList, const Entity& entity, ImVec2 center, ImU32 color) {
    const std::string label = std::string(actorOverlayLabel(entity)) + (entity.name.empty() ? std::string{} : (": " + entity.name));
    const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    const ImVec2 min(center.x + 16.0f, center.y - textSize.y * 0.5f - 4.0f);
    const ImVec2 max(min.x + textSize.x + 10.0f, min.y + textSize.y + 8.0f);
    drawList->AddRectFilled(min, max, IM_COL32(10, 14, 20, 220), 4.0f);
    drawList->AddRect(min, max, color, 4.0f, 0, 1.0f);
    drawList->AddText(ImVec2(min.x + 5.0f, min.y + 4.0f), IM_COL32(220, 228, 238, 255), label.c_str());
}

void drawSelectedLightOverlay(
    const EditorRuntimeState& state,
    const glm::mat4& view,
    const glm::mat4& projection,
    const Entity& entity) {
    if (!entity.light.has_value()) {
        return;
    }

    constexpr float nearPlane = 0.01f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const glm::mat4 world = entityWorldMatrix(state.sceneDocument->registry(), entity);
    glm::vec3 center = glm::vec3(world[3]);
    const glm::vec3 viewCenter = glm::vec3(view * glm::vec4(center, 1.0f));
    const std::optional<ImVec2> screenCenter = projectViewToScreen(state, projection, viewCenter, nearPlane);
    if (!screenCenter.has_value()) {
        return;
    }

    drawList->AddCircle(*screenCenter, 8.0f, IM_COL32(255, 214, 80, 255), 24, 2.0f);
    drawList->AddLine(ImVec2(screenCenter->x - 10.0f, screenCenter->y), ImVec2(screenCenter->x + 10.0f, screenCenter->y), IM_COL32(255, 214, 80, 220), 2.0f);
    drawList->AddLine(ImVec2(screenCenter->x, screenCenter->y - 10.0f), ImVec2(screenCenter->x, screenCenter->y + 10.0f), IM_COL32(255, 214, 80, 220), 2.0f);

    const Light& light = *entity.light;
    if (light.type == LightType::Point) {
        glm::vec3 forward{};
        glm::vec3 right{};
        glm::vec3 up{};
        cameraBasis(*state.camera, forward, right, up);
        const glm::vec3 radiusPoint = center + right * std::max(light.sizeOrRadius, 0.05f);
        const std::optional<ImVec2> screenRadius = projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(radiusPoint, 1.0f)), nearPlane);
        if (screenRadius.has_value()) {
            const float radius = std::max(8.0f, std::abs(screenRadius->x - screenCenter->x));
            drawList->AddCircle(*screenCenter, radius, IM_COL32(255, 214, 80, 120), 32, 1.5f);
        }
    } else if (light.type == LightType::Area) {
        const glm::vec3 axisX = glm::normalize(glm::mat3(world) * glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 axisY = glm::normalize(glm::mat3(world) * glm::vec3(0.0f, 1.0f, 0.0f));
        const float halfSize = std::max(light.sizeOrRadius * 0.5f, 0.05f);
        std::array<glm::vec3, 4> corners{
            center + axisX * halfSize + axisY * halfSize,
            center - axisX * halfSize + axisY * halfSize,
            center - axisX * halfSize - axisY * halfSize,
            center + axisX * halfSize - axisY * halfSize,
        };
        std::array<ImVec2, 4> screenCorners{};
        bool valid = true;
        for (size_t i = 0; i < corners.size(); ++i) {
            const std::optional<ImVec2> screen = projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(corners[i], 1.0f)), nearPlane);
            if (!screen.has_value()) {
                valid = false;
                break;
            }
            screenCorners[i] = *screen;
        }
        if (valid) {
            drawList->AddPolyline(screenCorners.data(), static_cast<int>(screenCorners.size()), IM_COL32(255, 214, 80, 180), ImDrawFlags_Closed, 2.0f);
        }
    }
}

void drawSelectedCameraOverlay(
    const EditorRuntimeState& state,
    const glm::mat4& view,
    const glm::mat4& projection,
    const Entity& entity) {
    if (!entity.camera.has_value()) {
        return;
    }
    constexpr float nearPlane = 0.01f;
    const glm::mat4 world = entityWorldMatrix(state.sceneDocument->registry(), entity);
    const glm::vec3 center = glm::vec3(world[3]);
    const glm::mat3 basis(world);
    const glm::vec3 forward = glm::normalize(basis * glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::vec3 right = glm::normalize(basis * glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 up = glm::normalize(basis * glm::vec3(0.0f, 1.0f, 0.0f));
    const float distance = 1.0f;
    const float halfHeight = std::tan(entity.camera->verticalFovRadians * 0.5f) * distance;
    const float halfWidth = halfHeight * viewportAspect(state);
    const glm::vec3 farCenter = center + forward * distance;
    const std::array<glm::vec3, 4> corners{
        farCenter + right * halfWidth + up * halfHeight,
        farCenter - right * halfWidth + up * halfHeight,
        farCenter - right * halfWidth - up * halfHeight,
        farCenter + right * halfWidth - up * halfHeight,
    };
    const std::optional<ImVec2> screenCenter = projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(center, 1.0f)), nearPlane);
    if (!screenCenter.has_value()) {
        return;
    }
    std::array<ImVec2, 4> screenCorners{};
    bool valid = true;
    for (size_t i = 0; i < corners.size(); ++i) {
        const std::optional<ImVec2> screen = projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(corners[i], 1.0f)), nearPlane);
        if (!screen.has_value()) {
            valid = false;
            break;
        }
        screenCorners[i] = *screen;
    }
    if (!valid) {
        return;
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = actorOverlayColor(entity, true);
    drawList->AddPolyline(screenCorners.data(), static_cast<int>(screenCorners.size()), color, ImDrawFlags_Closed, 2.0f);
    for (const ImVec2& corner : screenCorners) {
        drawList->AddLine(*screenCenter, corner, IM_COL32(105, 180, 255, 150), 1.5f);
    }
    drawActorLabel(drawList, entity, *screenCenter, color);
}

void drawSelectedDirectionalOverlay(
    const EditorRuntimeState& state,
    const glm::mat4& view,
    const glm::mat4& projection,
    const Entity& entity) {
    if (!entity.sun.has_value()) {
        return;
    }
    constexpr float nearPlane = 0.01f;
    const glm::mat4 world = entityWorldMatrix(state.sceneDocument->registry(), entity);
    const glm::vec3 center = glm::vec3(world[3]);
    const glm::vec3 direction = glm::normalize(glm::mat3(world) * glm::vec3(0.0f, 0.0f, -1.0f));
    const std::optional<ImVec2> start = projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(center, 1.0f)), nearPlane);
    const std::optional<ImVec2> end = projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(center + direction * 2.0f, 1.0f)), nearPlane);
    if (!start.has_value()) {
        return;
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = actorOverlayColor(entity, true);
    drawList->AddCircle(*start, 12.0f, color, 28, 2.0f);
    if (end.has_value()) {
        drawList->AddLine(*start, *end, color, 2.0f);
        drawList->AddCircleFilled(*end, 4.0f, color, 16);
    }
    drawActorLabel(drawList, entity, *start, color);
}

void drawSelectedWorldEffectOverlay(
    const EditorRuntimeState& state,
    const glm::mat4& view,
    const glm::mat4& projection,
    const Entity& entity) {
    if (!entity.environmentLight.has_value() && !entity.skyAtmosphere.has_value() && !entity.heightFog.has_value() &&
        !entity.volumetricCloud.has_value() && !entity.postProcessVolume.has_value() && !entity.cameraPostProcess.has_value()) {
        return;
    }
    const std::optional<ImVec2> center = entityScreenCenter(state, view, projection, entity);
    if (!center.has_value()) {
        return;
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = actorOverlayColor(entity, true);
    if (entity.environmentLight.has_value() || entity.skyAtmosphere.has_value()) {
        drawList->AddCircle(*center, 19.0f, color, 36, 2.0f);
        drawList->AddCircle(*center, 27.0f, IM_COL32(114, 169, 255, 90), 40, 1.0f);
    }
    if (entity.heightFog.has_value()) {
        for (int i = -1; i <= 1; ++i) {
            drawList->AddLine(ImVec2(center->x - 28.0f, center->y + static_cast<float>(i) * 7.0f), ImVec2(center->x + 28.0f, center->y + static_cast<float>(i) * 7.0f), color, 1.5f);
        }
    }
    if (entity.volumetricCloud.has_value()) {
        drawList->AddCircle(ImVec2(center->x - 9.0f, center->y + 2.0f), 11.0f, color, 24, 2.0f);
        drawList->AddCircle(ImVec2(center->x + 4.0f, center->y - 4.0f), 14.0f, color, 24, 2.0f);
        drawList->AddCircle(ImVec2(center->x + 17.0f, center->y + 3.0f), 9.0f, color, 24, 2.0f);
    }
    if (entity.postProcessVolume.has_value() || entity.cameraPostProcess.has_value()) {
        drawList->AddRect(ImVec2(center->x - 24.0f, center->y - 18.0f), ImVec2(center->x + 24.0f, center->y + 18.0f), color, 5.0f, 0, 2.0f);
        drawList->AddRect(ImVec2(center->x - 30.0f, center->y - 24.0f), ImVec2(center->x + 30.0f, center->y + 24.0f), IM_COL32(206, 158, 255, 95), 5.0f, 0, 1.0f);
    }
    drawActorLabel(drawList, entity, *center, color);
}

void drawSelectionOverlay(const EditorRuntimeState& state, const EditorSelection& selection) {
    if (state.sceneDocument == nullptr || state.camera == nullptr || !selection.entityId().valid()) {
        return;
    }
    const Entity* entity = state.sceneDocument->registry().entity(selection.entityId());
    if (entity == nullptr) {
        return;
    }
    const glm::mat4 view = editorViewMatrix(*state.camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));
    if (entity->light.has_value()) {
        drawSelectedLightOverlay(state, view, projection, *entity);
    }
    if (entity->camera.has_value()) {
        drawSelectedCameraOverlay(state, view, projection, *entity);
    }
    if (entity->sun.has_value()) {
        drawSelectedDirectionalOverlay(state, view, projection, *entity);
    }
    drawSelectedWorldEffectOverlay(state, view, projection, *entity);
}

void drawGridOverlay(const EditorRuntimeState& state, const CameraController& camera) {
    if (state.sceneDocument == nullptr) {
        return;
    }
    const glm::mat4 view = editorViewMatrix(camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));

    const glm::vec3 camPos = camera.position();
    const float ox = state.viewport.imageOrigin.x;
    const float oy = state.viewport.imageOrigin.y;
    const float iw = static_cast<float>(state.viewport.imageSize.x);
    const float ih = static_cast<float>(state.viewport.imageSize.y);

    auto clip = [&](glm::vec3 wp) -> glm::vec4 {
        return projection * view * glm::vec4(wp, 1.0f);
    };
    auto screen = [&](glm::vec4 c) -> ImVec2 {
        glm::vec3 ndc = glm::vec3(c) / c.w;
        return ImVec2(ox + (ndc.x * 0.5f + 0.5f) * iw, oy + (1.0f - (ndc.y * 0.5f + 0.5f)) * ih);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto drawClippedLine = [&](glm::vec3 a, glm::vec3 b, ImU32 color, float thk) {
        glm::vec4 ca = clip(a);
        glm::vec4 cb = clip(b);

        auto visibleNear = [](const glm::vec4& c) {
            return c.w > 0.0001f && c.z >= -c.w;
        };

        bool va = visibleNear(ca);
        bool vb = visibleNear(cb);

        if (!va && !vb) {
            return;
        }

        if (va && vb) {
            dl->AddLine(screen(ca), screen(cb), color, thk);
            return;
        }

        float da = ca.z + ca.w;
        float db = cb.z + cb.w;
        float t = da / (da - db);
        t = std::clamp(t, 0.0f, 1.0f);

        glm::vec4 ci = ca + (cb - ca) * t;

        if (!va) ca = ci;
        else     cb = ci;

        dl->AddLine(screen(ca), screen(cb), color, thk);
    };

    constexpr float halfExtent = 20.0f;
    constexpr float stepSz     = 1.0f;
    constexpr int   halfSteps  = static_cast<int>(halfExtent / stepSz);

    const float cx = std::floor(camPos.x / stepSz) * stepSz;
    const float cz = std::floor(camPos.z / stepSz) * stepSz;

    for (int i = -halfSteps; i <= halfSteps; ++i) {
        const float pos  = static_cast<float>(i) * stepSz;
        const bool major = (i % 5) == 0;
        const ImU32 color = major ? IM_COL32(140, 140, 140, 160) : IM_COL32(80, 80, 80, 70);
        const float thk   = major ? 1.5f : 0.7f;

        drawClippedLine(glm::vec3(cx + pos, 0.0f, cz - halfExtent),
                        glm::vec3(cx + pos, 0.0f, cz + halfExtent), color, thk);
        drawClippedLine(glm::vec3(cx - halfExtent, 0.0f, cz + pos),
                        glm::vec3(cx + halfExtent, 0.0f, cz + pos), color, thk);
    }
}

void drawAxesIndicator(const EditorRuntimeState& state, const CameraController& camera) {
    const float size = 48.0f, margin = 14.0f;
    const float ox = state.viewport.imageOrigin.x;
    const float oy = state.viewport.imageOrigin.y;
    const float iw = static_cast<float>(state.viewport.imageSize.x);
    const ImVec2 origin(ox + iw - margin - size, oy + margin + size);

    const glm::mat3 rot = glm::mat3(editorViewMatrix(camera));
    const glm::vec3 xDir = rot * glm::vec3(1,0,0);
    const glm::vec3 yDir = rot * glm::vec3(0,1,0);
    const glm::vec3 zDir = rot * glm::vec3(0,0,1);

    struct AxisItem { const char* label; ImU32 color; float depth; ImVec2 tip; };
    auto makeItem = [&](glm::vec3 d, const char* lbl, ImU32 clr) -> AxisItem {
        ImVec2 t(origin.x + d.x * size * 0.7f, origin.y - d.y * size * 0.7f);
        return {lbl, clr, d.z, t};
    };

    AxisItem axes[3] = {
        makeItem(xDir, "X", IM_COL32(255,80,80,255)),
        makeItem(yDir, "Y", IM_COL32(80,255,80,255)),
        makeItem(zDir, "Z", IM_COL32(80,80,255,255)),
    };
    std::sort(std::begin(axes), std::end(axes),
              [](const AxisItem& a, const AxisItem& b) { return a.depth < b.depth; });

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(origin, 2.5f, IM_COL32(200,200,200,255));
    for (const auto& ax : axes) {
        dl->AddLine(origin, ax.tip, ax.color, 2.0f);
        dl->AddText(nullptr, 13.0f, {ax.tip.x+3,ax.tip.y-7}, ax.color, ax.label);
    }
}

void drawViewportOverlayBackdrop(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    min.x -= EditorUiMetric::viewportOverlayPaddingX;
    min.y -= EditorUiMetric::viewportOverlayPaddingY;
    max.x += EditorUiMetric::viewportOverlayPaddingX;
    max.y += EditorUiMetric::viewportOverlayPaddingY;
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(editorViewportOverlayBgColor()), EditorUiMetric::viewportOverlayRounding);
    drawList->AddRect(min, max, ImGui::GetColorU32(editorViewportOverlayBorderColor()), EditorUiMetric::viewportOverlayRounding);
}

void drawViewportTopRail(ImDrawList* drawList, ImVec2 imagePos, ImVec2 avail) {
    const ImVec2 min(imagePos.x, imagePos.y);
    const ImVec2 max(imagePos.x + avail.x, imagePos.y + editorIconButtonSize().y + 3.0f);
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(editorViewportOverlayBgColor()), 0.0f);
    drawList->AddLine(ImVec2(min.x, max.y), max, ImGui::GetColorU32(editorViewportOverlayBorderColor()), 1.0f);
}

} // namespace

void ViewportPanel::draw(EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    if (state.editorPrefs != nullptr && !viewportPreferencesLoaded_) {
        reloadViewportPreferences(*state.editorPrefs);
    }
    const SnapSettings snapAtFrameStart = snap_;
    const bool showGridAtFrameStart = showGrid_;
    const bool showAxesAtFrameStart = showAxes_;
    const bool localGizmoAtFrameStart = localGizmoMode_;
    const bool pickMeshEntitiesAtFrameStart = pickMeshEntities_;
    const bool pickActorIconsAtFrameStart = pickActorIcons_;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin(EditorDockWindowTitle::Scene)) {
        focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        hovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        state.viewport.focused = focused_;
        state.viewport.hovered = hovered_;

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 imagePos = ImGui::GetCursorScreenPos();
        lastContentExtent_.width = static_cast<uint32_t>(std::max(1.0f, std::floor(avail.x)));
        lastContentExtent_.height = static_cast<uint32_t>(std::max(1.0f, std::floor(avail.y)));
        state.viewport.imageOrigin = {imagePos.x, imagePos.y};
        state.viewport.imageSize = {avail.x, avail.y};
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        state.viewport.mousePosition = {mousePos.x, mousePos.y};
        state.viewport.mouseUv = {
            avail.x > 0.0f ? std::clamp((mousePos.x - imagePos.x) / avail.x, 0.0f, 1.0f) : 0.0f,
            avail.y > 0.0f ? std::clamp((mousePos.y - imagePos.y) / avail.y, 0.0f, 1.0f) : 0.0f,
        };
        state.viewport.leftClicked = hovered_ && !state.viewport.mouseCaptureActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        const bool imageMatchesPanel =
            lastContentExtent_.width == state.viewport.displayExtent.width &&
            lastContentExtent_.height == state.viewport.displayExtent.height;

        if (imageMatchesPanel && state.viewport.textureReady && state.viewport.texture != VK_NULL_HANDLE) {
            ImGui::Image(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(state.viewport.texture)),
                avail,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f));
        } else {
            ImGui::Dummy(avail);
            ImGui::GetWindowDrawList()->AddRectFilled(
                imagePos,
                ImVec2(imagePos.x + avail.x, imagePos.y + avail.y),
                IM_COL32(18, 20, 23, 255));
        }
        const bool viewportContentHovered = ImGui::IsItemHovered();
        const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
        const std::string placementKey = activePayload != nullptr ? placementPayloadKey(*activePayload) : std::string{};
        if (placementKey.empty()) {
            activePlacementPayloadKey_.clear();
            if (!placementBrush_.active()) {
                placementPreviewYawRadians_ = 0.0f;
            }
        } else if (placementKey != activePlacementPayloadKey_) {
            activePlacementPayloadKey_ = placementKey;
            placementBrush_ = {};
            placementPreviewYawRadians_ = 0.0f;
        } else if (!viewportDropWheelRotationEnabled(state)) {
            placementPreviewYawRadians_ = 0.0f;
        } else if (viewportContentHovered && std::abs(ImGui::GetIO().MouseWheel) > 0.0f) {
            const float stepDegrees = snap_.enabled ? std::max(1.0f, snap_.rotation) : 5.0f;
            placementPreviewYawRadians_ = wrappedPlacementYaw(
                placementPreviewYawRadians_ + glm::radians(stepDegrees) * ImGui::GetIO().MouseWheel);
        }
        if (placementBrush_.active() && placementKey.empty()) {
            if (!viewportDropWheelRotationEnabled(state)) {
                placementPreviewYawRadians_ = 0.0f;
            } else if (viewportContentHovered && std::abs(ImGui::GetIO().MouseWheel) > 0.0f) {
                const float stepDegrees = snap_.enabled ? std::max(1.0f, snap_.rotation) : 5.0f;
                placementPreviewYawRadians_ = wrappedPlacementYaw(
                    placementPreviewYawRadians_ + glm::radians(stepDegrees) * ImGui::GetIO().MouseWheel);
            }
            placementBrush_.yawRadians = placementPreviewYawRadians_;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                placementBrush_ = {};
                placementPreviewYawRadians_ = 0.0f;
            }
        }
        if (viewportContentHovered) {
            if (activePayload != nullptr) {
                drawViewportDropPreview(state, selection, snap_.enabled, snap_.translation, placementPreviewYawRadians_);
            } else if (placementBrush_.active()) {
                drawViewportPlacementBrushPreview(state, placementBrush_, snap_.enabled, snap_.translation);
            }
        }
        bool placementBrushClicked = false;
        bool placementBrushCancelledClick = false;
        auto armPlacementBrush = [&](ViewportPlacementBrushKind kind, AssetGuid guid) {
            const bool multiPlace = viewportDropMultiPlaceActive(state);
            const bool duplicatePlacement = viewportDropDuplicatePlacementActive(state);
            if (guid.empty() || (!multiPlace && !duplicatePlacement)) {
                return;
            }
            placementBrush_ = ViewportPlacementBrushState{
                .kind = kind,
                .guid = std::move(guid),
                .yawRadians = placementPreviewYawRadians_,
                .remainingPlacements = duplicatePlacement && !multiPlace ? 1 : 0,
                .multiPlace = multiPlace,
            };
        };
        auto requestScatterMeshPlacement = [&](const AssetGuid& guid, float yawRadians, std::optional<ViewportPlacementBounds> bounds) {
            if (!viewportScatterPaletteActive(state)) {
                return false;
            }
            std::vector<EditorMeshScatterInstancePlacement> instances = buildScatterMeshPlacements(
                state,
                guid,
                yawRadians,
                scatterPaletteSettingsFromPreferences(state.editorPrefs),
                bounds);
            if (instances.empty()) {
                return false;
            }
            requests.meshScatterPlacement = EditorMeshScatterPlacement{
                .instances = std::move(instances),
                .seed = state.editorPrefs != nullptr ? state.editorPrefs->viewportScatterPaletteSeed : 1u,
                .label = "Scatter Mesh Palette",
            };
            return true;
        };
        if (ImGui::BeginDragDropTarget()) {
            const bool forceGridDrop = viewportDropForceGridActive(state);
            const bool surfaceAlignDrop = viewportDropSurfaceAlignActive(state, forceGridDrop);
            if (const auto* payload = ImGui::AcceptDragDropPayload("PREFAB_ASSET")) {
                const AssetGuid guid = payloadString(*payload);
                requests.placeAsset = guid;
                requests.placeAssetTransform = viewportDropPlacementTransform(state, snap_.enabled, snap_.translation, forceGridDrop, surfaceAlignDrop, placementPreviewYawRadians_);
                armPlacementBrush(ViewportPlacementBrushKind::Prefab, guid);
            }
            if (const auto* payload = ImGui::AcceptDragDropPayload("MESH_ASSET")) {
                const AssetGuid guid = payloadString(*payload);
                const std::optional<ViewportPlacementBounds> bounds = meshPlacementBoundsForPayload(state, *payload);
                EntityId replaceEntity{};
                if (ImGui::GetIO().KeyShift) {
                    if (const std::optional<ViewportSceneRayHit> hit = viewportSceneRaycastUnderCursor(state)) {
                        replaceEntity = hit->entity;
                    } else if (state.sceneDocument != nullptr) {
                        if (const Entity* selectedEntity = state.sceneDocument->registry().entity(selection.entityId());
                            selectedEntity != nullptr && selectedEntity->meshRenderer.has_value()) {
                            replaceEntity = selectedEntity->id;
                        }
                    }
                }
                if (!replaceEntity.valid() && requestScatterMeshPlacement(guid, placementPreviewYawRadians_, bounds)) {
                    // Scatter placement is routed as one undoable batch request.
                } else {
                    requests.meshAssetPlacement = EditorMeshAssetPlacement{
                        .meshGuid = guid,
                        .placementTransform = viewportDropPlacementTransform(state, snap_.enabled, snap_.translation, forceGridDrop, surfaceAlignDrop, placementPreviewYawRadians_, bounds),
                        .replaceEntity = replaceEntity,
                    };
                }
                if (!replaceEntity.valid()) {
                    armPlacementBrush(ViewportPlacementBrushKind::Mesh, guid);
                }
            }
            if (const auto* payload = ImGui::AcceptDragDropPayload("MATERIAL_ASSET")) {
                const AssetGuid materialGuid = payloadString(*payload);
                if (!materialGuid.empty()) {
                    if (const std::optional<ViewportSceneRayHit> hit = viewportSceneRaycastUnderCursor(state)) {
                        requests.materialAssetAssignment = EditorMaterialAssetAssignment{
                            .materialGuid = materialGuid,
                            .entity = hit->entity,
                            .primitiveIndex = hit->primitiveIndex,
                        };
                        selection.selectEntity(hit->entity);
                        selection.setLastClickedId(hit->entity);
                    } else {
                        pendingMaterialDropGuid_ = materialGuid;
                        pendingMaterialDropEntity_ = selection.entityId();
                        materialDropPopupOpen_ = true;
                    }
                }
            }
            if (const auto* payload = ImGui::AcceptDragDropPayload("ENVIRONMENT_ASSET")) {
                requests.environmentAssetAssignment = std::string(static_cast<const char*>(payload->Data));
            }
            if (const auto* payload = ImGui::AcceptDragDropPayload("LEVEL_PATH")) {
                pendingLevelDropPath_ = std::filesystem::path(payloadString(*payload));
                pendingLevelDropLabel_ = pendingLevelDropPath_.filename().string();
                levelDropPopupOpen_ = !pendingLevelDropPath_.empty();
            }
            ImGui::EndDragDropTarget();
        }

        if (placementBrush_.active() && placementKey.empty() && viewportContentHovered) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                placementBrush_ = {};
                placementPreviewYawRadians_ = 0.0f;
                rightMouseContextCandidate_ = false;
                rightMouseContextSuppressed_ = true;
                placementBrushCancelledClick = true;
            } else if (state.viewport.leftClicked && !state.viewport.mouseCaptureActive && !ImGui::GetIO().WantTextInput) {
                const bool forceGridDrop = viewportDropForceGridActive(state);
                const bool surfaceAlignDrop = viewportDropSurfaceAlignActive(state, forceGridDrop);
                if (placementBrush_.kind == ViewportPlacementBrushKind::Prefab) {
                    requests.placeAsset = placementBrush_.guid;
                    requests.placeAssetTransform = viewportDropPlacementTransform(
                        state,
                        snap_.enabled,
                        snap_.translation,
                        forceGridDrop,
                        surfaceAlignDrop,
                        placementBrush_.yawRadians);
                    placementBrushClicked = true;
                } else if (placementBrush_.kind == ViewportPlacementBrushKind::Mesh) {
                    std::optional<ViewportPlacementBounds> bounds;
                    if (const std::optional<MeshAssetHandle> handle = loadedMeshHandleForGuid(state, placementBrush_.guid)) {
                        bounds = meshPlacementBounds(state.assets, *handle);
                    }
                    if (requestScatterMeshPlacement(placementBrush_.guid, placementBrush_.yawRadians, bounds)) {
                        placementBrushClicked = true;
                    } else if (!viewportScatterPaletteActive(state)) {
                        requests.meshAssetPlacement = EditorMeshAssetPlacement{
                            .meshGuid = placementBrush_.guid,
                            .placementTransform = viewportDropPlacementTransform(
                                state,
                                snap_.enabled,
                                snap_.translation,
                                forceGridDrop,
                                surfaceAlignDrop,
                                placementBrush_.yawRadians,
                                bounds),
                        };
                        placementBrushClicked = true;
                    }
                }
                if (placementBrushClicked && !placementBrush_.multiPlace) {
                    --placementBrush_.remainingPlacements;
                    if (placementBrush_.remainingPlacements <= 0) {
                        placementBrush_ = {};
                        placementPreviewYawRadians_ = 0.0f;
                    }
                }
            }
        }

        if (levelDropPopupOpen_) {
            ImGui::OpenPopup("LevelDropActionPopup");
            levelDropPopupOpen_ = false;
        }
        if (ImGui::BeginPopupModal("LevelDropActionPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", pendingLevelDropLabel_.empty() ? "Level" : pendingLevelDropLabel_.c_str());
            ImGui::TextDisabled("%s", pendingLevelDropPath_.string().c_str());
            ImGui::Spacing();
            if (editorGlyphMenuItem(EditorGlyphIcon::SceneFile, "Open Level", !pendingLevelDropPath_.empty())) {
                requests.openScene = pendingLevelDropPath_;
                pendingLevelDropPath_.clear();
                pendingLevelDropLabel_.clear();
                ImGui::CloseCurrentPopup();
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Merge Into Current Level", !pendingLevelDropPath_.empty())) {
                requests.mergeScene = pendingLevelDropPath_;
                pendingLevelDropPath_.clear();
                pendingLevelDropLabel_.clear();
                ImGui::CloseCurrentPopup();
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Group, "Add As Sublevel", !pendingLevelDropPath_.empty())) {
                requests.mergeScene = pendingLevelDropPath_;
                pendingLevelDropPath_.clear();
                pendingLevelDropLabel_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (editorGlyphMenuItem(EditorGlyphIcon::Exit, "Cancel")) {
                pendingLevelDropPath_.clear();
                pendingLevelDropLabel_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (materialDropPopupOpen_) {
            ImGui::OpenPopup("MaterialDropActionPopup");
            materialDropPopupOpen_ = false;
        }
        if (ImGui::BeginPopupModal("MaterialDropActionPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            Entity* targetEntity = state.sceneDocument != nullptr ? state.sceneDocument->registry().entity(pendingMaterialDropEntity_) : nullptr;
            const bool hasMeshTarget = targetEntity != nullptr && targetEntity->meshRenderer.has_value();
            ImGui::Text("Material asset");
            ImGui::TextDisabled("%s", pendingMaterialDropGuid_.empty() ? "(none)" : pendingMaterialDropGuid_.c_str());
            if (!hasMeshTarget) {
                ImGui::Spacing();
                ImGui::TextDisabled("Select a mesh entity before assigning a dropped material.");
            }
            ImGui::Spacing();
            if (editorGlyphMenuItem(EditorGlyphIcon::Material, "Assign to Slot 0", hasMeshTarget)) {
                requests.materialAssetAssignment = EditorMaterialAssetAssignment{
                    .materialGuid = pendingMaterialDropGuid_,
                    .entity = pendingMaterialDropEntity_,
                    .primitiveIndex = 0u,
                };
                pendingMaterialDropGuid_.clear();
                pendingMaterialDropEntity_ = {};
                ImGui::CloseCurrentPopup();
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Material, "Assign to All Slots", hasMeshTarget)) {
                requests.materialAssetAssignment = EditorMaterialAssetAssignment{
                    .materialGuid = pendingMaterialDropGuid_,
                    .entity = pendingMaterialDropEntity_,
                    .primitiveIndex = UINT32_MAX,
                };
                pendingMaterialDropGuid_.clear();
                pendingMaterialDropEntity_ = {};
                ImGui::CloseCurrentPopup();
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Details, "Open Material Slots", hasMeshTarget)) {
                selection.selectEntity(pendingMaterialDropEntity_);
                selection.setLastClickedId(pendingMaterialDropEntity_);
                requests.showInspector = true;
                requests.focusOnEntity = pendingMaterialDropEntity_;
                pendingMaterialDropGuid_.clear();
                pendingMaterialDropEntity_ = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (editorGlyphMenuItem(EditorGlyphIcon::Exit, "Cancel")) {
                pendingMaterialDropGuid_.clear();
                pendingMaterialDropEntity_ = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        constexpr float viewportContextTapMaxSeconds = 0.18f;
        constexpr float viewportContextTapMaxDragSq = 16.0f;
        const ImGuiIO& io = ImGui::GetIO();
        if (viewportContentHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !placementBrushCancelledClick) {
            rightMouseContextCandidate_ = true;
            rightMouseContextSuppressed_ = false;
            rightMouseContextHoldSeconds_ = 0.0f;
        }
        if (rightMouseContextCandidate_ && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            rightMouseContextHoldSeconds_ += io.DeltaTime;
            const ImVec2 rightDrag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
            const bool rightDragExceeded = (rightDrag.x * rightDrag.x + rightDrag.y * rightDrag.y) >= viewportContextTapMaxDragSq;
            const bool cameraMovedDuringCapture = state.camera != nullptr && state.camera->mouseCaptureMoved();
            if (rightMouseContextHoldSeconds_ >= viewportContextTapMaxSeconds || rightDragExceeded || cameraMovedDuringCapture) {
                rightMouseContextSuppressed_ = true;
            }
        }
        const float releasedNavigationDuration = state.camera != nullptr ? state.camera->releasedMouseCaptureDurationSeconds() : -1.0f;
        const bool releasedNavigationGesture = rightMouseContextSuppressed_ ||
            releasedNavigationDuration >= viewportContextTapMaxSeconds ||
            (state.camera != nullptr && state.camera->releasedMouseCaptureMoved());
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            if (rightMouseContextCandidate_ &&
                viewportContentHovered &&
                !state.viewport.mouseCaptureActive &&
                !releasedNavigationGesture) {
                ImGui::OpenPopup("ViewportContextMenu");
            }
            rightMouseContextCandidate_ = false;
            rightMouseContextSuppressed_ = false;
            rightMouseContextHoldSeconds_ = 0.0f;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            rightMouseContextCandidate_ = false;
            rightMouseContextSuppressed_ = false;
            rightMouseContextHoldSeconds_ = 0.0f;
        }
        if (ImGui::BeginPopup("ViewportContextMenu")) {
            Entity* selectedEntity = state.sceneDocument != nullptr ? state.sceneDocument->registry().entity(selection.entityId()) : nullptr;
            const bool hasSelection = selectedEntity != nullptr;
            const bool editableSelection = hasSelection && !selectedEntity->locked;
            if (editorGlyphMenuItem(EditorGlyphIcon::Frame, "Focus Selected", hasSelection)) {
                requests.focusOnEntity = selection.entityId();
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Duplicate", editableSelection)) {
                requests.duplicateEntity = selection.entityId();
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete", editableSelection)) {
                const std::vector<EntityId> selectedEntities = selection.selectedEntitiesOr(selection.entityId());
                if (selectedEntities.size() == 1) {
                    requests.deleteEntity = selectedEntities.front();
                } else {
                    requests.deleteEntities = selectedEntities;
                }
                selection.clear();
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Reset, "Reset Transform", editableSelection)) {
                Transform reset = selectedEntity->defaultTransform;
                reset.dirty = true;
                const SceneUpdateKind updateKind = transformUpdateKind(*state.sceneDocument, *selectedEntity);
                requests.setEntityTransform = EditorEntityTransformChange{
                    .entity = selectedEntity->id,
                    .oldTransform = selectedEntity->transform,
                    .newTransform = reset,
                };
                requests.sceneUpdate = updateKind;
            }
            ImGui::Separator();
            if (editorGlyphMenuItem(EditorGlyphIcon::Entity, "Create Empty Here")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Empty};
                requests.sceneUpdate = SceneUpdateKind::None;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Camera, "Create Camera Here")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Camera};
                requests.sceneUpdate = SceneUpdateKind::CameraOnly;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Create Light Here")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Light};
                requests.sceneUpdate = SceneUpdateKind::LightOnly;
            }
            editorGlyphMenuItem(EditorGlyphIcon::Add, "Drop prefab here", false);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Drag a prefab from Content, or drag a mesh, material, HDRI, or level asset onto the viewport");
            }
            ImGui::EndPopup();
        }

        const RendererSettings& settings = state.renderer.settings();
        const GpuFrameTimings& timings = state.renderer.timings();
        bool gizmoHoveredOrUsing = false;

        if (focused_ && !state.viewport.mouseCaptureActive && !ImGui::GetIO().WantTextInput) {
            auto commandPressed = [&](EditorCommandId id) {
                const EditorKeybinding binding = editorCommandKeybinding(id, state.editorPrefs);
                if (binding.imguiKey < 0) {
                    return false;
                }
                const ImGuiIO& io = ImGui::GetIO();
                if (binding.ctrl != io.KeyCtrl || binding.shift != io.KeyShift || binding.alt != io.KeyAlt) {
                    return false;
                }
                return ImGui::IsKeyPressed(static_cast<ImGuiKey>(binding.imguiKey));
            };
            if (commandPressed(EditorCommandId::ViewportSelect)) { executeCommand(EditorCommandId::ViewportSelect); }
            if (commandPressed(EditorCommandId::ViewportMove) || ImGui::IsKeyPressed(ImGuiKey_T)) { executeCommand(EditorCommandId::ViewportMove); }
            if (commandPressed(EditorCommandId::ViewportRotate)) { executeCommand(EditorCommandId::ViewportRotate); }
            if (commandPressed(EditorCommandId::ViewportScale) || ImGui::IsKeyPressed(ImGuiKey_S)) { executeCommand(EditorCommandId::ViewportScale); }
            if (commandPressed(EditorCommandId::ViewportToggleLocal)) { executeCommand(EditorCommandId::ViewportToggleLocal); }
            if (commandPressed(EditorCommandId::ViewportFrameSelected) && selection.entityId().valid()) {
                requests.focusOnEntity = selection.entityId();
            }
            if (commandPressed(EditorCommandId::ViewportToggleGrid)) { executeCommand(EditorCommandId::ViewportToggleGrid); }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        bool viewportUiHovered = false;
        auto commandTooltip = [&](EditorCommandId command) {
            const std::string shortcut = editorCommandShortcutDisplay(command, state.editorPrefs);
            if (shortcut.empty()) {
                ImGui::SetTooltip("%s", editorCommandName(command));
            } else {
                ImGui::SetTooltip("%s (%s)", editorCommandName(command), shortcut.c_str());
            }
        };
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(0);
        drawViewportTopRail(dl, imagePos, avail);
        dl->ChannelsSetCurrent(1);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(EditorUiMetric::rowPaddingX, EditorUiMetric::rowPaddingY));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, EditorUiMetric::compactButtonRounding);
        ImGui::SetCursorScreenPos(ImVec2(imagePos.x + 4.0f, imagePos.y + 2.0f));
        ImGui::BeginGroup();
        auto toolButton = [&](EditorGlyphIcon icon, const char* id, EditorCommandId command, bool active) {
            const bool pressed = editorIconButton(id, icon, active);
            viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
            if (ImGui::IsItemHovered()) {
                commandTooltip(command);
            }
            if (pressed) {
                executeCommand(command);
            }
            ImGui::SameLine();
        };
        toolButton(EditorGlyphIcon::Select, "ViewportSelect", EditorCommandId::ViewportSelect, transformGizmoMode_ < 0);
        toolButton(EditorGlyphIcon::Move, "ViewportMove", EditorCommandId::ViewportMove, transformGizmoMode_ == 0);
        toolButton(EditorGlyphIcon::Rotate, "ViewportRotate", EditorCommandId::ViewportRotate, transformGizmoMode_ == 1);
        toolButton(EditorGlyphIcon::Scale, "ViewportScale", EditorCommandId::ViewportScale, transformGizmoMode_ == 2);
        toolButton(localGizmoMode_ ? EditorGlyphIcon::LocalSpace : EditorGlyphIcon::WorldSpace, "ViewportSpace", EditorCommandId::ViewportToggleLocal, localGizmoMode_);
        if (state.editorPrefs != nullptr && selection.entityId().valid() && transformGizmoMode_ == 2) {
            const bool linkedScale = state.editorPrefs->linkedScale;
            const bool pressed = editorIconButton(
                "ViewportLinkedScale",
                linkedScale ? EditorGlyphIcon::Lock : EditorGlyphIcon::Unlock,
                linkedScale);
            viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Linked Scale");
            }
            if (pressed) {
                state.editorPrefs->linkedScale = !linkedScale;
            }
            ImGui::SameLine();
        }
        toolButton(EditorGlyphIcon::Snap, "ViewportSnap", EditorCommandId::ViewportToggleSnap, snap_.enabled);
        toolButton(EditorGlyphIcon::Grid, "ViewportGrid", EditorCommandId::ViewportToggleGrid, showGrid_);
        toolButton(EditorGlyphIcon::Axes, "ViewportAxes", EditorCommandId::ViewportToggleAxes, showAxes_);
        if (selection.entityId().valid()) {
            if (editorIconButton("ViewportFrameSelected", EditorGlyphIcon::Frame, false)) { requests.focusOnEntity = selection.entityId(); }
            viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
            if (ImGui::IsItemHovered()) {
                commandTooltip(EditorCommandId::ViewportFrameSelected);
            }
            if (snap_.enabled) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(58.0f);
                if (transformGizmoMode_ == 0) {
                    if (ImGui::DragFloat("##snapTranslate", &snap_.translation, 0.01f, 0.001f, 100.0f, "%.2f")) {
                        snap_.translation = std::clamp(snap_.translation, 0.001f, 100.0f);
                    }
                } else if (transformGizmoMode_ == 1) {
                    if (ImGui::DragFloat("##snapRotate", &snap_.rotation, 1.0f, 0.1f, 180.0f, "%.0f")) {
                        snap_.rotation = std::clamp(snap_.rotation, 0.1f, 180.0f);
                    }
                } else {
                    if (ImGui::DragFloat("##snapScale", &snap_.scale, 0.01f, 0.001f, 10.0f, "%.2f")) {
                        snap_.scale = std::clamp(snap_.scale, 0.001f, 10.0f);
                    }
                }
                viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
            }
        }
        ImGui::EndGroup();
        ImGui::PopStyleVar(3);

        const float gpuTotal = timings.totalMs();
        std::ostringstream compactStatus;
        compactStatus << "pt " << state.renderer.sampleCount() << "/" << settings.accumulationLimit
                      << " " << std::fixed << std::setprecision(3) << gpuTotal;
        if (state.viewport.mouseCaptureActive) {
            compactStatus << "  Moving";
        } else if (settings.debugView != RendererDebugView::Beauty) {
            compactStatus << "  " << rendererDebugViewName(settings.debugView);
        }
        const std::string statusText = compactStatus.str();
        const bool hudVisible = state.editorPrefs == nullptr || state.editorPrefs->showHud;
        const float hudScale = state.editorPrefs != nullptr ? std::clamp(state.editorPrefs->hudScale, 0.75f, 1.75f) : 1.0f;
        const ImVec2 baseStatusSize = ImGui::CalcTextSize(statusText.c_str());
        const ImVec2 statusSize(baseStatusSize.x * hudScale, baseStatusSize.y * hudScale);
        const float statusRight = imagePos.x + avail.x - 6.0f;
        const float statusY = imagePos.y + 4.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, EditorUiMetric::rowPaddingY));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, EditorUiMetric::compactButtonRounding);
        std::string cameraSpeedText;
        if (hudVisible && state.camera != nullptr) {
            std::ostringstream cameraSpeed;
            cameraSpeed << std::fixed << std::setprecision(3) << state.camera->moveSpeed();
            cameraSpeedText = cameraSpeed.str();
        }
        const float cameraSpeedWidth = cameraSpeedText.empty() ? 0.0f : editorIconTextButtonWidth(cameraSpeedText.c_str()) + ImGui::GetStyle().ItemSpacing.x;
        const float overlayWidth = editorIconTextButtonWidth("View Settings") +
            (hudVisible ? editorIconTextButtonWidth("Stats") + editorIconTextButtonWidth("Draw Debug") + cameraSpeedWidth + ImGui::GetStyle().ItemSpacing.x * 3.0f : 0.0f);
        const float controlsX = std::max(imagePos.x + 8.0f, statusRight - overlayWidth);
        const float statusX = std::max(imagePos.x + 8.0f, controlsX - statusSize.x - 14.0f);
        if (hudVisible) {
            dl->AddText(nullptr, ImGui::GetFontSize() * hudScale, ImVec2(statusX, statusY), IM_COL32(216, 221, 228, 245), statusText.c_str());
        }

        ImGui::SetCursorScreenPos(ImVec2(controlsX, imagePos.y + 2.0f));
        ImGui::BeginGroup();
        auto overlayButton = [&](EditorGlyphIcon icon, const char* label, const char* popupName, const char* buttonId, const char* tooltip) {
            const bool open = ImGui::IsPopupOpen(popupName);
            if (editorIconTextButton(buttonId, icon, label, open)) {
                ImGui::OpenPopup(popupName);
            }
            viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", tooltip);
            }
        };
        overlayButton(EditorGlyphIcon::ViewSettings, "View Settings", "ViewportViewSettings", "ViewportViewSettingsButton", "Viewport overlays, transform frame, and preview debug view");
        if (hudVisible) {
            ImGui::SameLine();
            overlayButton(EditorGlyphIcon::Stats, "Stats", "ViewportStats", "ViewportStatsButton", "Frame timing and render statistics");
            ImGui::SameLine();
            overlayButton(EditorGlyphIcon::DrawDebug, "Draw Debug", "ViewportDrawDebug", "ViewportDrawDebugButton", "Viewport debug drawing controls");
        }
        if (hudVisible && !cameraSpeedText.empty()) {
            ImGui::SameLine();
            editorIconTextReadout(EditorGlyphIcon::Camera, cameraSpeedText.c_str(), IM_COL32(216, 221, 228, 245));
            viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Viewport camera navigation speed");
            }
        }
        ImGui::EndGroup();
        viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
        dl->ChannelsMerge();

        auto beginOverlayPopup = [&](const char* name, float width) {
            ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Appearing);
            const bool open = ImGui::BeginPopup(name);
            if (open) {
                viewportUiHovered = true;
                viewportUiHovered = viewportUiHovered || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            }
            return open;
        };

        if (beginOverlayPopup("ViewportViewSettings", 300.0f)) {
            RendererSettings popupSettings = settings;
            bool changed = false;
            ImGui::SeparatorText("Viewport");
            if (state.editorPrefs != nullptr) {
                bool saveViewportPrefs = false;
                saveViewportPrefs |= ImGui::Checkbox("HUD", &state.editorPrefs->showHud);
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragFloat("HUD Scale", &state.editorPrefs->hudScale, 0.01f, 0.75f, 1.75f, "%.2f")) {
                    state.editorPrefs->hudScale = std::clamp(state.editorPrefs->hudScale, 0.75f, 1.75f);
                    saveViewportPrefs = true;
                }
                if (saveViewportPrefs) {
                    state.editorPrefs->save(state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath);
                }
            }
            ImGui::Checkbox("Grid", &showGrid_);
            ImGui::Checkbox("Axes", &showAxes_);
            ImGui::Checkbox("Local transform frame", &localGizmoMode_);
            ImGui::Checkbox("Snap", &snap_.enabled);
            if (snap_.enabled) {
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragFloat("Translate", &snap_.translation, 0.01f, 0.001f, 100.0f, "%.2f")) {
                    snap_.translation = std::clamp(snap_.translation, 0.001f, 100.0f);
                }
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragFloat("Rotate", &snap_.rotation, 1.0f, 0.1f, 180.0f, "%.0f")) {
                    snap_.rotation = std::clamp(snap_.rotation, 0.1f, 180.0f);
                }
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragFloat("Scale", &snap_.scale, 0.01f, 0.001f, 10.0f, "%.2f")) {
                    snap_.scale = std::clamp(snap_.scale, 0.001f, 10.0f);
                }
            }
            ImGui::SeparatorText("Selection Filters");
            ImGui::Checkbox("Pick meshes", &pickMeshEntities_);
            ImGui::Checkbox("Pick actor icons", &pickActorIcons_);
            if (state.editorPrefs != nullptr) {
                ImGui::SeparatorText("Placement Drops");
                bool savePrefs = false;
                savePrefs |= ImGui::Checkbox("Force grid by default", &state.editorPrefs->viewportDropForceGridByDefault);
                savePrefs |= ImGui::Checkbox("Surface align by default", &state.editorPrefs->viewportDropSurfaceAlignByDefault);
                savePrefs |= ImGui::Checkbox("Duplicate next by default", &state.editorPrefs->viewportDropDuplicatePlacementByDefault);
                savePrefs |= ImGui::Checkbox("Multi-place by default", &state.editorPrefs->viewportDropMultiPlaceByDefault);
                savePrefs |= ImGui::Checkbox("Wheel rotation", &state.editorPrefs->viewportDropMouseWheelRotationEnabled);
                if (savePrefs) {
                    state.editorPrefs->save(state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath);
                }

                ImGui::SeparatorText("Surface Snapping");
                bool saveSurfaceSnapPrefs = false;
                saveSurfaceSnapPrefs |= ImGui::Checkbox("Enable surface snap", &state.editorPrefs->viewportSurfaceSnappingEnabled);
                saveSurfaceSnapPrefs |= ImGui::Checkbox("Align to normal", &state.editorPrefs->viewportSurfaceSnapAlignToNormal);
                saveSurfaceSnapPrefs |= ImGui::Checkbox("Preserve yaw", &state.editorPrefs->viewportSurfaceSnapPreserveYaw);
                saveSurfaceSnapPrefs |= ImGui::Checkbox("Snap bounds bottom", &state.editorPrefs->viewportSurfaceSnapBoundsBottom);
                ImGui::SetNextItemWidth(140.0f);
                saveSurfaceSnapPrefs |= ImGui::DragFloat(
                    "Surface offset",
                    &state.editorPrefs->viewportSurfaceSnapOffset,
                    0.01f,
                    -100.0f,
                    100.0f,
                    "%.3f");
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("Axis constraint", surfaceSnapAxisConstraintLabel(state.editorPrefs->viewportSurfaceSnapAxisConstraint))) {
                    for (int axis = 0; axis <= 3; ++axis) {
                        const bool selected = state.editorPrefs->viewportSurfaceSnapAxisConstraint == axis;
                        if (ImGui::Selectable(surfaceSnapAxisConstraintLabel(axis), selected)) {
                            state.editorPrefs->viewportSurfaceSnapAxisConstraint = axis;
                            saveSurfaceSnapPrefs = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                if (saveSurfaceSnapPrefs) {
                    state.editorPrefs->viewportSurfaceSnapOffset = std::clamp(state.editorPrefs->viewportSurfaceSnapOffset, -100.0f, 100.0f);
                    state.editorPrefs->viewportSurfaceSnapAxisConstraint = std::clamp(state.editorPrefs->viewportSurfaceSnapAxisConstraint, 0, 3);
                    state.editorPrefs->save(state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath);
                }

                ImGui::SeparatorText("Scatter Palettes");
                bool saveScatterPrefs = false;
                saveScatterPrefs |= ImGui::Checkbox("Scatter mesh placement", &state.editorPrefs->viewportScatterPaletteByDefault);
                saveScatterPrefs |= ImGui::Checkbox("Scatter surface align", &state.editorPrefs->viewportScatterPaletteSurfaceAlignment);
                ImGui::SetNextItemWidth(140.0f);
                saveScatterPrefs |= ImGui::DragFloat("Scatter density", &state.editorPrefs->viewportScatterPaletteDensity, 0.05f, 0.0f, 10000.0f, "%.2f");
                ImGui::SetNextItemWidth(140.0f);
                saveScatterPrefs |= ImGui::DragFloat("Scatter spacing", &state.editorPrefs->viewportScatterPaletteSpacing, 0.05f, 0.001f, 10000.0f, "%.3f");
                ImGui::SetNextItemWidth(140.0f);
                saveScatterPrefs |= ImGui::DragFloat("Collision radius", &state.editorPrefs->viewportScatterPaletteCollisionRadius, 0.01f, 0.0f, 10000.0f, "%.3f");
                ImGui::SetNextItemWidth(140.0f);
                saveScatterPrefs |= ImGui::DragFloatRange2("Slope range", &state.editorPrefs->viewportScatterPaletteSlopeMinDegrees, &state.editorPrefs->viewportScatterPaletteSlopeMaxDegrees, 0.5f, 0.0f, 180.0f, "%.1f");
                ImGui::SetNextItemWidth(140.0f);
                saveScatterPrefs |= ImGui::DragFloatRange2("Height range", &state.editorPrefs->viewportScatterPaletteHeightMin, &state.editorPrefs->viewportScatterPaletteHeightMax, 0.5f, -10000.0f, 10000.0f, "%.1f");
                ImGui::SetNextItemWidth(140.0f);
                saveScatterPrefs |= ImGui::DragFloatRange2("Scale range", &state.editorPrefs->viewportScatterPaletteScaleMin, &state.editorPrefs->viewportScatterPaletteScaleMax, 0.01f, 0.001f, 1000.0f, "%.2f");
                ImGui::SetNextItemWidth(140.0f);
                saveScatterPrefs |= ImGui::DragFloat("Yaw random", &state.editorPrefs->viewportScatterPaletteYawRandomDegrees, 1.0f, 0.0f, 360.0f, "%.0f");
                int scatterSeed = static_cast<int>(std::min<uint32_t>(state.editorPrefs->viewportScatterPaletteSeed, static_cast<uint32_t>(std::numeric_limits<int>::max())));
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragInt("Scatter seed", &scatterSeed, 1.0f, 0, std::numeric_limits<int>::max())) {
                    state.editorPrefs->viewportScatterPaletteSeed = static_cast<uint32_t>(std::max(0, scatterSeed));
                    saveScatterPrefs = true;
                }
                if (saveScatterPrefs) {
                    state.editorPrefs->viewportScatterPaletteDensity = std::clamp(state.editorPrefs->viewportScatterPaletteDensity, 0.0f, 10000.0f);
                    state.editorPrefs->viewportScatterPaletteSlopeMinDegrees = std::clamp(state.editorPrefs->viewportScatterPaletteSlopeMinDegrees, 0.0f, 180.0f);
                    state.editorPrefs->viewportScatterPaletteSlopeMaxDegrees = std::clamp(state.editorPrefs->viewportScatterPaletteSlopeMaxDegrees, 0.0f, 180.0f);
                    if (state.editorPrefs->viewportScatterPaletteSlopeMaxDegrees < state.editorPrefs->viewportScatterPaletteSlopeMinDegrees) {
                        std::swap(state.editorPrefs->viewportScatterPaletteSlopeMinDegrees, state.editorPrefs->viewportScatterPaletteSlopeMaxDegrees);
                    }
                    if (state.editorPrefs->viewportScatterPaletteHeightMax < state.editorPrefs->viewportScatterPaletteHeightMin) {
                        std::swap(state.editorPrefs->viewportScatterPaletteHeightMin, state.editorPrefs->viewportScatterPaletteHeightMax);
                    }
                    state.editorPrefs->viewportScatterPaletteScaleMin = std::clamp(state.editorPrefs->viewportScatterPaletteScaleMin, 0.001f, 1000.0f);
                    state.editorPrefs->viewportScatterPaletteScaleMax = std::clamp(state.editorPrefs->viewportScatterPaletteScaleMax, 0.001f, 1000.0f);
                    if (state.editorPrefs->viewportScatterPaletteScaleMax < state.editorPrefs->viewportScatterPaletteScaleMin) {
                        std::swap(state.editorPrefs->viewportScatterPaletteScaleMin, state.editorPrefs->viewportScatterPaletteScaleMax);
                    }
                    state.editorPrefs->viewportScatterPaletteYawRandomDegrees = std::clamp(state.editorPrefs->viewportScatterPaletteYawRandomDegrees, 0.0f, 360.0f);
                    state.editorPrefs->viewportScatterPaletteSpacing = std::clamp(state.editorPrefs->viewportScatterPaletteSpacing, 0.001f, 10000.0f);
                    state.editorPrefs->viewportScatterPaletteCollisionRadius = std::clamp(state.editorPrefs->viewportScatterPaletteCollisionRadius, 0.0f, 10000.0f);
                    state.editorPrefs->save(state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath);
                }
            }
            ImGui::SeparatorText("Advanced Tools");
            std::vector<EntityId> pivotSelection = editableTransformSelection(state, selection);
            auto setEditorPivot = [&](EditorPivotSettings nextPivot, const char* label) {
                if (state.sceneDocument == nullptr) {
                    return;
                }
                const SceneDocument before = *state.sceneDocument;
                state.sceneDocument->setEditorPivot(nextPivot);
                requests.sceneSnapshot = EditorSceneSnapshotChange{
                    .before = before,
                    .updateKind = SceneUpdateKind::None,
                    .label = label,
                };
            };
            auto setPivotMode = [&](EditorPivotMode mode) {
                if (state.sceneDocument == nullptr) {
                    return;
                }
                EditorPivotSettings pivot = state.sceneDocument->editorPivot();
                pivot.mode = mode;
                setEditorPivot(pivot, "Set Pivot Mode");
            };
            if (state.sceneDocument != nullptr) {
                const EditorPivotSettings currentPivot = state.sceneDocument->editorPivot();
                ImGui::TextUnformatted("Pivot");
                ImGui::SameLine(58.0f);
                if (editorIconTextButton("PivotActive", EditorGlyphIcon::Frame, editorPivotModeLabel(EditorPivotMode::Active), currentPivot.mode == EditorPivotMode::Active)) {
                    setPivotMode(EditorPivotMode::Active);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(pivotSelection.empty());
                if (editorIconTextButton("PivotSelectionCenter", EditorGlyphIcon::Group, editorPivotModeLabel(EditorPivotMode::SelectionCenter), currentPivot.mode == EditorPivotMode::SelectionCenter)) {
                    setPivotMode(EditorPivotMode::SelectionCenter);
                }
                ImGui::SameLine();
                if (editorIconTextButton("PivotBoundsCenter", EditorGlyphIcon::Layout, editorPivotModeLabel(EditorPivotMode::BoundsCenter), currentPivot.mode == EditorPivotMode::BoundsCenter)) {
                    setPivotMode(EditorPivotMode::BoundsCenter);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (editorIconTextButton("PivotCustom", EditorGlyphIcon::Move, editorPivotModeLabel(EditorPivotMode::Custom), currentPivot.mode == EditorPivotMode::Custom)) {
                    setPivotMode(EditorPivotMode::Custom);
                }

                ImGui::BeginDisabled(pivotSelection.empty());
                if (editorIconTextButton("PivotCustomFromSelection", EditorGlyphIcon::Group, "From Sel")) {
                    EditorPivotSettings pivot = currentPivot;
                    pivot.mode = EditorPivotMode::Custom;
                    pivot.customPosition = selectionPositionCenter(state.sceneDocument->registry(), *state.sceneDocument->registry().entity(pivotSelection.front()), pivotSelection);
                    setEditorPivot(pivot, "Set Custom Pivot From Selection");
                }
                ImGui::SameLine();
                if (editorIconTextButton("PivotCustomFromBounds", EditorGlyphIcon::Layout, "From Bnd")) {
                    EditorPivotSettings pivot = currentPivot;
                    pivot.mode = EditorPivotMode::Custom;
                    const Entity* active = state.sceneDocument->registry().entity(pivotSelection.front());
                    if (active != nullptr) {
                        pivot.customPosition = selectionBoundsCenter(state, pivotSelection).value_or(selectionPositionCenter(state.sceneDocument->registry(), *active, pivotSelection));
                        setEditorPivot(pivot, "Set Custom Pivot From Bounds");
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (editorIconTextButton("PivotCustomFromHit", EditorGlyphIcon::Select, "From Hit")) {
                    if (const std::optional<Transform> hit = viewportDropPlacementTransform(state, snap_.enabled, snap_.translation, false, true)) {
                        EditorPivotSettings pivot = currentPivot;
                        pivot.mode = EditorPivotMode::Custom;
                        pivot.customPosition = hit->position;
                        pivot.customRotationEuler = hit->rotationEuler;
                        setEditorPivot(pivot, "Set Custom Pivot From Hit");
                    } else if (const std::optional<Transform> grid = viewportDropPlacementTransform(state, snap_.enabled, snap_.translation, true, false)) {
                        EditorPivotSettings pivot = currentPivot;
                        pivot.mode = EditorPivotMode::Custom;
                        pivot.customPosition = grid->position;
                        pivot.customRotationEuler = grid->rotationEuler;
                        setEditorPivot(pivot, "Set Custom Pivot From Hit");
                    }
                }
                ImGui::SameLine();
                if (editorIconTextButton("PivotCustomWorldOrigin", EditorGlyphIcon::Reset, "Origin")) {
                    EditorPivotSettings pivot = currentPivot;
                    pivot.mode = EditorPivotMode::Custom;
                    pivot.customPosition = glm::vec3{0.0f};
                    pivot.customRotationEuler = glm::vec3{0.0f};
                    setEditorPivot(pivot, "Set Custom Pivot To Origin");
                }
                glm::vec3 typedPosition = currentPivot.customPosition;
                glm::vec3 typedRotationDegrees = glm::degrees(currentPivot.customRotationEuler);
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::InputFloat3("Pivot Pos", glm::value_ptr(typedPosition), "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                    EditorPivotSettings pivot = currentPivot;
                    pivot.mode = EditorPivotMode::Custom;
                    pivot.customPosition = typedPosition;
                    setEditorPivot(pivot, "Set Custom Pivot Position");
                }
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::InputFloat3("Pivot Rot", glm::value_ptr(typedRotationDegrees), "%.1f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                    EditorPivotSettings pivot = currentPivot;
                    pivot.mode = EditorPivotMode::Custom;
                    pivot.customRotationEuler = glm::radians(typedRotationDegrees);
                    setEditorPivot(pivot, "Set Custom Pivot Rotation");
                }
            }
            std::vector<EntityId> alignSelection = selection.selectedEntitiesOr(selection.entityId());
            alignSelection.erase(
                std::remove_if(
                    alignSelection.begin(),
                    alignSelection.end(),
                    [&](EntityId id) {
                        const Entity* entity = state.sceneDocument != nullptr ? state.sceneDocument->registry().entity(id) : nullptr;
                        return entity == nullptr || entity->locked;
                    }),
                alignSelection.end());
            const bool canAlignDistribute = alignSelection.size() >= 2;
            const bool canDistribute = alignSelection.size() >= 3;
            std::vector<EditorAlignDistributeEntityBounds> alignBounds;
            alignBounds.reserve(alignSelection.size());
            for (EntityId id : alignSelection) {
                if (const Entity* entity = state.sceneDocument != nullptr ? state.sceneDocument->registry().entity(id) : nullptr) {
                    alignBounds.push_back(alignDistributeBoundsForEntity(state, *entity));
                }
            }
            auto requestAlignDistribute = [&](EditorAlignDistributeAxis axis, EditorAlignDistributeMode mode) {
                requests.alignDistributeEntities = EditorAlignDistributeRequest{
                    .entities = alignSelection,
                    .bounds = alignBounds,
                    .axis = axis,
                    .mode = mode,
                };
            };
            auto drawAlignDistributeAxis = [&](const char* label, EditorAlignDistributeAxis axis) {
                ImGui::TextUnformatted(label);
                ImGui::SameLine(28.0f);
                ImGui::BeginDisabled(!canAlignDistribute);
                const std::string minId = std::string("Align") + label + "Min";
                if (editorIconTextButton(minId.c_str(), EditorGlyphIcon::Move, "Min")) {
                    requestAlignDistribute(axis, EditorAlignDistributeMode::AlignMin);
                }
                ImGui::SameLine();
                const std::string midId = std::string("Align") + label + "Center";
                if (editorIconTextButton(midId.c_str(), EditorGlyphIcon::Move, "Mid")) {
                    requestAlignDistribute(axis, EditorAlignDistributeMode::AlignCenter);
                }
                ImGui::SameLine();
                const std::string maxId = std::string("Align") + label + "Max";
                if (editorIconTextButton(maxId.c_str(), EditorGlyphIcon::Move, "Max")) {
                    requestAlignDistribute(axis, EditorAlignDistributeMode::AlignMax);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!canDistribute);
                const std::string spaceId = std::string("Distribute") + label + "Spacing";
                if (editorIconTextButton(spaceId.c_str(), EditorGlyphIcon::Move, "Space")) {
                    requestAlignDistribute(axis, EditorAlignDistributeMode::DistributeSpacing);
                }
                ImGui::EndDisabled();
            };
            drawAlignDistributeAxis("X", EditorAlignDistributeAxis::X);
            drawAlignDistributeAxis("Y", EditorAlignDistributeAxis::Y);
            drawAlignDistributeAxis("Z", EditorAlignDistributeAxis::Z);
            if (!canAlignDistribute) {
                ImGui::TextDisabled("Select at least two unlocked entities.");
            } else if (!canDistribute) {
                ImGui::TextDisabled("Spacing distribution requires three unlocked entities.");
            }
            if (editorIconTextButton("ViewportAdvancedToolsReadinessReport", EditorGlyphIcon::Details, "Advanced Tools Readiness")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeViewportAdvancedToolsReadinessReport(
                        state,
                        snap_.enabled,
                        snap_.translation,
                        snap_.rotation,
                        snap_.scale,
                        localGizmoMode_,
                        showGrid_,
                        reportPath,
                        error)) {
                    requests.openFilePath = reportPath;
                } else if (state.log != nullptr) {
                    state.log->add(EditorLogCategory::Warning, error);
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Write and open advanced viewport tool readiness without changing the scene");
            }
            if (state.camera != nullptr) {
                ImGui::SeparatorText("Navigation");
                float cameraSpeed = state.camera->moveSpeed();
                float fastCameraSpeed = state.camera->fastMoveSpeed();
                float cameraSensitivity = state.camera->mouseSensitivity();
                bool invertLookX = state.camera->invertLookX();
                bool invertLookY = state.camera->invertLookY();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragFloat("Camera Speed", &cameraSpeed, 0.05f, 0.05f, 100.0f, "%.3f")) {
                    const float clampedSpeed = std::clamp(cameraSpeed, 0.05f, 100.0f);
                    state.camera->setMoveSpeed(clampedSpeed);
                    requests.cameraMoveSpeed = clampedSpeed;
                }
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragFloat("Fast Camera Speed", &fastCameraSpeed, 0.1f, 0.05f, 250.0f, "%.3f")) {
                    const float clampedFastSpeed = std::clamp(fastCameraSpeed, 0.05f, 250.0f);
                    state.camera->setFastMoveSpeed(clampedFastSpeed);
                    requests.cameraFastMoveSpeed = clampedFastSpeed;
                }
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragFloat("Look Sensitivity", &cameraSensitivity, 0.0001f, 0.0001f, 0.02f, "%.4f")) {
                    const float clampedSensitivity = std::clamp(cameraSensitivity, 0.0001f, 0.02f);
                    state.camera->setMouseSensitivity(clampedSensitivity);
                    requests.cameraMouseSensitivity = clampedSensitivity;
                }
                if (ImGui::Checkbox("Invert Look X", &invertLookX)) {
                    state.camera->setInvertLookX(invertLookX);
                    requests.cameraInvertLookX = invertLookX;
                }
                if (ImGui::Checkbox("Invert Look Y", &invertLookY)) {
                    state.camera->setInvertLookY(invertLookY);
                    requests.cameraInvertLookY = invertLookY;
                }
            }
            ImGui::SeparatorText("Preview");
            editorDebugViewCombo("Debug View", popupSettings, changed);
            changed |= ImGui::SliderFloat("Render Scale", &popupSettings.renderResolutionScale, 0.25f, 1.0f, "%.2f");
            if (changed) {
                requestSettings(requests, popupSettings);
            }
            ImGui::EndPopup();
        }

        if (beginOverlayPopup("ViewportStats", 320.0f)) {
            const GpuPipelineStatistics pipelineStats = state.renderer.pipelineStats();
            ImGui::SeparatorText("Frame");
            if (ImGui::BeginTable("ViewportStatsFrame", 2, ImGuiTableFlags_SizingStretchProp)) {
                auto row = [](const char* label, const char* value) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", label);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(value);
                };
                char value[64] = {};
                std::snprintf(value, sizeof(value), "%.3f ms", gpuTotal);
                row("GPU", value);
                std::snprintf(value, sizeof(value), "%.3f ms", state.cpuFrameMs);
                row("CPU", value);
                std::snprintf(value, sizeof(value), "%u / %u", state.renderer.sampleCount(), settings.accumulationLimit);
                row("Samples", value);
                std::snprintf(value, sizeof(value), "%ux%u", state.viewport.renderExtent.width, state.viewport.renderExtent.height);
                row("Render", value);
                std::snprintf(value, sizeof(value), "%ux%u", state.viewport.displayExtent.width, state.viewport.displayExtent.height);
                row("Display", value);
                row("Debug View", rendererDebugViewName(settings.debugView));
                ImGui::EndTable();
            }
            ImGui::SeparatorText("GPU Passes");
            if (ImGui::BeginTable("ViewportStatsPasses", 2, ImGuiTableFlags_SizingStretchProp)) {
                auto timingRow = [](const char* label, float ms) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", label);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f ms", ms);
                };
                timingRow("Path Trace", timings.pathTraceMs);
                timingRow("ReSTIR DI", timings.restirSpatialMs);
                timingRow("ReSTIR GI", timings.restirGiSpatialMs + timings.restirGiFinalMs);
                timingRow("Denoiser", timings.denoiserMs + timings.momentUpdateMs);
                timingRow("TAA", timings.taaMs + timings.taaHistoryCopyMs);
                timingRow("Tone Map", timings.toneMapMs);
                timingRow("Presentation", timings.editorPresentationMs + timings.fullscreenMs);
                ImGui::EndTable();
            }
            ImGui::SeparatorText("Pipeline");
            if (pipelineStats.valid) {
                ImGui::Text("Rays: %llu", static_cast<unsigned long long>(pipelineStats.rayInvocations));
                ImGui::Text("Triangle hits: %llu", static_cast<unsigned long long>(pipelineStats.triangleHits));
                ImGui::Text("AABB hits: %llu", static_cast<unsigned long long>(pipelineStats.aabbHits));
            } else {
                ImGui::TextDisabled("Pipeline counters unavailable");
            }
            ImGui::EndPopup();
        }

        if (beginOverlayPopup("ViewportDrawDebug", 260.0f)) {
            ImGui::SeparatorText("Draw Layers");
            ImGui::Checkbox("Grid", &showGrid_);
            ImGui::Checkbox("Axes", &showAxes_);
            ImGui::Checkbox("Actor icons", &showActorIcons_);
            ImGui::Checkbox("Selection overlay", &showSelectionOverlay_);
            ImGui::SeparatorText("Selection Filters");
            ImGui::Checkbox("Pick meshes", &pickMeshEntities_);
            ImGui::Checkbox("Pick actor icons", &pickActorIcons_);
            ImGui::BeginDisabled();
            bool meshBounds = false;
            ImGui::Checkbox("Mesh bounds", &meshBounds);
            ImGui::EndDisabled();
            ImGui::SeparatorText("Active Debug View");
            ImGui::TextWrapped("%s", rendererDebugViewName(settings.debugView));
            ImGui::EndPopup();
        }

        ImGui::PopStyleVar(3);

        lastSampleCount_ = state.renderer.sampleCount();

        if (showActorIcons_) {
            drawActorIconsOverlay(state, selection);
        }

        if (showSelectionOverlay_) {
            drawSelectionOverlay(state, selection);
        }

        if (state.camera != nullptr) {
            if (showAxes_) drawAxesIndicator(state, *state.camera);
            if (showGrid_) drawGridOverlay(state, *state.camera);
        }

        if (state.sceneDocument != nullptr && state.camera != nullptr && selection.entityId().valid()) {
            std::vector<EntityId> transformSelection = editableTransformSelection(state, selection);
            Entity* entity = state.sceneDocument->registry().entity(selection.entityId());
            if ((entity == nullptr || entity->locked) && !transformSelection.empty()) {
                entity = state.sceneDocument->registry().entity(transformSelection.front());
            }
            if (entity != nullptr && !entity->locked) {
                if (transformGizmoMode_ >= 0) {
                    const glm::mat4 view = editorViewMatrix(*state.camera);
                    const glm::mat4 projection = editorProjectionMatrix(
                        activeCameraFov(*state.sceneDocument),
                        viewportAspect(state));

                    const bool nonActivePivot = state.sceneDocument->editorPivot().mode != EditorPivotMode::Active;
                    const bool groupTransform = transformSelection.size() > 1 || nonActivePivot;
                    glm::mat4 world = groupTransform
                        ? pivotWorldMatrixForSelection(state, *entity, transformSelection)
                        : entityWorldMatrix(state.sceneDocument->registry(), *entity);
                    const ImGuizmo::OPERATION operation = transformGizmoMode_ == 0
                        ? ImGuizmo::TRANSLATE
                        : (transformGizmoMode_ == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
                    const glm::mat4 previousWorld = world;
                    ImGuizmo::BeginFrame();
                    ImGuizmo::SetOrthographic(false);
                    ImGuizmo::SetDrawlist(dl);
                    ImGuizmo::SetRect(state.viewport.imageOrigin.x, state.viewport.imageOrigin.y, state.viewport.imageSize.x, state.viewport.imageSize.y);
                    float snapValues[3] = {};
                    if (snap_.enabled) {
                        const float value = transformGizmoMode_ == 0
                            ? snap_.translation
                            : (transformGizmoMode_ == 1 ? snap_.rotation : snap_.scale);
                        snapValues[0] = value;
                        snapValues[1] = value;
                        snapValues[2] = value;
                    }
                    const bool manipulated = ImGuizmo::Manipulate(
                        glm::value_ptr(view),
                        glm::value_ptr(projection),
                        operation,
                        localGizmoMode_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                        glm::value_ptr(world),
                        nullptr,
                        snap_.enabled ? snapValues : nullptr);
                    const bool isOver = ImGuizmo::IsOver();
                    const bool isUsing = ImGuizmo::IsUsing();
                    gizmoHoveredOrUsing = isOver || isUsing;

                    if (isUsing && groupTransform && (!gizmoDragActive_ || groupGizmoDragOriginals_.empty())) {
                        gizmoDragActive_ = true;
                        gizmoDragModified_ = false;
                        gizmoDragEntity_ = entity->id;
                        gizmoDragOriginal_ = entity->transform;
                        gizmoDragParentWorld_ = parentWorldMatrix(state.sceneDocument->registry(), *entity);
                        gizmoDragOriginalWorld_ = entityWorldMatrix(state.sceneDocument->registry(), *entity);
                        groupGizmoDragOriginalWorld_ = previousWorld;
                        groupGizmoDragOriginals_.clear();
                        groupGizmoDragOriginals_.reserve(transformSelection.size());
                        for (EntityId id : transformSelection) {
                            if (Entity* selected = state.sceneDocument->registry().entity(id); selected != nullptr && !selected->locked) {
                                groupGizmoDragOriginals_.push_back(GroupGizmoOriginal{
                                    .entity = id,
                                    .transform = selected->transform,
                                    .parentWorld = parentWorldMatrix(state.sceneDocument->registry(), *selected),
                                    .world = entityWorldMatrix(state.sceneDocument->registry(), *selected),
                                });
                            }
                        }
                    } else if (isUsing && !groupTransform && (!gizmoDragActive_ || gizmoDragEntity_ != entity->id || !groupGizmoDragOriginals_.empty())) {
                        gizmoDragActive_ = true;
                        gizmoDragModified_ = false;
                        gizmoDragEntity_ = entity->id;
                        gizmoDragOriginal_ = entity->transform;
                        gizmoDragParentWorld_ = parentWorldMatrix(state.sceneDocument->registry(), *entity);
                        gizmoDragOriginalWorld_ = previousWorld;
                        groupGizmoDragOriginals_.clear();
                    }

                    if (manipulated && operation == ImGuizmo::TRANSLATE) {
                        const EditorSurfaceSnapSettings surfaceSnap = surfaceSnapSettingsFromPreferences(state.editorPrefs);
                        if (const std::optional<glm::mat4> snappedWorld = surfaceSnappedGizmoWorldMatrix(
                                state,
                                *entity,
                                world,
                                groupGizmoDragOriginalWorld_,
                                groupGizmoDragOriginals_,
                                transformSelection,
                                surfaceSnap)) {
                            world = *snappedWorld;
                        }
                    }

                    if (manipulated && world != previousWorld) {
                        const bool linkedScale = state.editorPrefs != nullptr && state.editorPrefs->linkedScale;
                        if (groupTransform && !groupGizmoDragOriginals_.empty()) {
                            const glm::mat4 delta = world * glm::inverse(groupGizmoDragOriginalWorld_);
                            EditorEntityTransformBatchPreview batchPreview;
                            for (const GroupGizmoOriginal& original : groupGizmoDragOriginals_) {
                                Entity* selected = state.sceneDocument->registry().entity(original.entity);
                                if (selected == nullptr || selected->locked) {
                                    continue;
                                }
                                const glm::mat4 local = glm::inverse(original.parentWorld) * delta * original.world;
                                const std::optional<glm::vec3> linkedScaleReference = operation == ImGuizmo::SCALE && linkedScale
                                    ? std::optional<glm::vec3>{original.transform.scale}
                                    : std::nullopt;
                                writeLocalTransformFromMatrix(*selected, local, linkedScaleReference);
                                const SceneUpdateKind updateKind = transformUpdateKind(*state.sceneDocument, *selected);
                                state.sceneDocument->markDirty(updateKind);
                                requests.sceneUpdate = updateKind;
                                batchPreview.previews.push_back(EditorEntityTransformPreview{
                                    .entity = selected->id,
                                    .transform = selected->transform,
                                    .updateKind = updateKind,
                                });
                            }
                            if (!batchPreview.previews.empty()) {
                                requests.previewEntityTransforms = std::move(batchPreview);
                                gizmoDragModified_ = true;
                            }
                        } else {
                            const glm::mat4 parentWorld = gizmoDragActive_ && gizmoDragEntity_ == entity->id
                                ? gizmoDragParentWorld_
                                : parentWorldMatrix(state.sceneDocument->registry(), *entity);
                            const glm::mat4 local = glm::inverse(parentWorld) * world;
                            const std::optional<glm::vec3> linkedScaleReference =
                                operation == ImGuizmo::SCALE && linkedScale && gizmoDragActive_ && gizmoDragEntity_ == entity->id
                                    ? std::optional<glm::vec3>{gizmoDragOriginal_.scale}
                                    : std::nullopt;
                            writeLocalTransformFromMatrix(*entity, local, linkedScaleReference);
                            if (gizmoDragActive_ && gizmoDragEntity_ == entity->id) {
                                gizmoDragModified_ = true;
                            }
                            const SceneUpdateKind updateKind = transformUpdateKind(*state.sceneDocument, *entity);
                            state.sceneDocument->markDirty(updateKind);
                            requests.sceneUpdate = updateKind;
                            requests.previewEntityTransform = EditorEntityTransformPreview{
                                .entity = entity->id,
                                .transform = entity->transform,
                                .updateKind = updateKind,
                            };
                        }
                    }

                    updateGizmoState(isOver, isUsing, transformGizmoMode_);

                    if (isUsing) {
                        const char* label = transformGizmoMode_ == 0
                            ? "Moving selection"
                            : (transformGizmoMode_ == 1 ? "Rotating selection" : "Scaling selection");
                        const ImVec2 textSize = ImGui::CalcTextSize(label);
                        const ImVec2 labelPos(
                            state.viewport.imageOrigin.x + state.viewport.imageSize.x * 0.5f - textSize.x * 0.5f - 10.0f,
                            state.viewport.imageOrigin.y + state.viewport.imageSize.y - 52.0f);
                        dl->AddRectFilled(
                            labelPos,
                            ImVec2(labelPos.x + textSize.x + 20.0f, labelPos.y + textSize.y + 12.0f),
                            IM_COL32(20, 24, 28, 210),
                            5.0f);
                        dl->AddText(
                            ImVec2(labelPos.x + 10.0f, labelPos.y + 6.0f),
                            IM_COL32(170, 215, 255, 255),
                            label);
                    }

                    if (!isUsing && gizmoDragActive_) {
                        commitGizmoDrag(requests, *state.sceneDocument);
                    }
                }
            }
        }

        if (const std::optional<uint32_t> pickedInstance = state.renderer.consumePickedInstanceId()) {
            if (const std::optional<EntityId> pickedEntity = entityForInstance(state, *pickedInstance)) {
                const Entity* entity = state.sceneDocument != nullptr ? state.sceneDocument->registry().entity(*pickedEntity) : nullptr;
                if (entity == nullptr || !entity->locked) {
                    selection.selectEntity(*pickedEntity);
                }
            }
        }

        if (state.viewport.leftClicked && !placementBrushClicked && !gizmoHoveredOrUsing && !viewportUiHovered) {
            if (showActorIcons_ && pickActorIcons_) {
                if (const std::optional<EntityId> pickedActor = actorIconUnderCursor(state)) {
                    selection.selectEntity(*pickedActor);
                    selection.setLastClickedId(*pickedActor);
                } else if (pickMeshEntities_) {
                    state.renderer.requestPickInstanceId(state.viewport.mouseUv);
                }
            } else if (pickMeshEntities_) {
                state.renderer.requestPickInstanceId(state.viewport.mouseUv);
            }
        }
        selection.setPickPending(state.renderer.pickPending());

        const bool gizmoDragging = gizmoState_ == GizmoInteractionState::DraggingTranslate
            || gizmoState_ == GizmoInteractionState::DraggingRotate
            || gizmoState_ == GizmoInteractionState::DraggingScale;
        state.viewport.mouseCaptureActive = state.viewport.mouseCaptureActive || gizmoDragging;

        state.renderer.setSelectedInstanceId(instanceForEntity(state, selection.entityId()));
    }
    if (state.editorPrefs != nullptr && (
        snapAtFrameStart.enabled != snap_.enabled ||
        snapAtFrameStart.translation != snap_.translation ||
        snapAtFrameStart.rotation != snap_.rotation ||
        snapAtFrameStart.scale != snap_.scale ||
        showGridAtFrameStart != showGrid_ ||
        showAxesAtFrameStart != showAxes_ ||
        localGizmoAtFrameStart != localGizmoMode_ ||
        pickMeshEntitiesAtFrameStart != pickMeshEntities_ ||
        pickActorIconsAtFrameStart != pickActorIcons_)) {
        persistViewportPreferences(*state.editorPrefs, state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

VkExtent2D ViewportPanel::desiredRenderExtent(VkExtent2D fallback) const {
    if (lastContentExtent_.width == 0 || lastContentExtent_.height == 0) {
        return fallback;
    }
    return lastContentExtent_;
}

void ViewportPanel::commitGizmoDrag(EditorRequests& requests, SceneDocument& document) {
    if (!gizmoDragActive_) {
        return;
    }
    if (!groupGizmoDragOriginals_.empty() && gizmoDragModified_) {
        EditorEntityTransformBatchChange batch;
        SceneUpdateMask updateMask = SceneUpdateMaskNone;
        for (const GroupGizmoOriginal& original : groupGizmoDragOriginals_) {
            Entity* entity = document.registry().entity(original.entity);
            if (entity == nullptr || entity->locked) {
                continue;
            }
            updateMask |= sceneUpdateKindMask(transformUpdateKind(document, *entity));
            batch.changes.push_back(EditorEntityTransformChange{
                .entity = original.entity,
                .oldTransform = original.transform,
                .newTransform = entity->transform,
            });
        }
        if (!batch.changes.empty()) {
            document.markDirty(updateMask);
            requests.sceneUpdate = sceneUpdateKindFromMask(updateMask);
            requests.setEntityTransforms = std::move(batch);
        }
    } else if (Entity* entity = document.registry().entity(gizmoDragEntity_); entity != nullptr && gizmoDragModified_) {
        const Transform finalTransform = entity->transform;
        const SceneUpdateKind updateKind = transformUpdateKind(document, *entity);
        document.markDirty(updateKind);
        requests.sceneUpdate = updateKind;
        requests.setEntityTransform = EditorEntityTransformChange{
            .entity = gizmoDragEntity_,
            .oldTransform = gizmoDragOriginal_,
            .newTransform = finalTransform,
        };
    }
    gizmoDragActive_ = false;
    gizmoDragModified_ = false;
    gizmoDragEntity_ = {};
    gizmoDragOriginal_ = {};
    gizmoDragParentWorld_ = glm::mat4{1.0f};
    gizmoDragOriginalWorld_ = glm::mat4{1.0f};
    groupGizmoDragOriginals_.clear();
    groupGizmoDragOriginalWorld_ = glm::mat4{1.0f};
}

void ViewportPanel::abortGizmoDrag() {
    if (!gizmoDragActive_) {
        return;
    }
    gizmoDragActive_ = false;
    gizmoDragModified_ = false;
    gizmoDragEntity_ = {};
    gizmoDragOriginal_ = {};
    gizmoDragParentWorld_ = glm::mat4{1.0f};
    gizmoDragOriginalWorld_ = glm::mat4{1.0f};
    groupGizmoDragOriginals_.clear();
    groupGizmoDragOriginalWorld_ = glm::mat4{1.0f};
}

void ViewportPanel::reloadViewportPreferences(const EditorPreferences& preferences) {
    showGrid_ = preferences.gridVisible;
    showAxes_ = preferences.viewportAxesVisible;
    localGizmoMode_ = preferences.viewportLocalTransformFrame;
    snap_.enabled = preferences.viewportSnapEnabled;
    snap_.translation = std::clamp(preferences.viewportTranslationSnap, 0.001f, 100.0f);
    snap_.rotation = std::clamp(preferences.viewportRotationSnap, 0.1f, 180.0f);
    snap_.scale = std::clamp(preferences.viewportScaleSnap, 0.001f, 10.0f);
    pickMeshEntities_ = preferences.viewportPickMeshEntities;
    pickActorIcons_ = preferences.viewportPickActorIcons;
    viewportPreferencesLoaded_ = true;
}

void ViewportPanel::persistViewportPreferences(EditorPreferences& preferences, const std::filesystem::path& path) const {
    preferences.gridVisible = showGrid_;
    preferences.viewportAxesVisible = showAxes_;
    preferences.viewportLocalTransformFrame = localGizmoMode_;
    preferences.viewportSnapEnabled = snap_.enabled;
    preferences.viewportTranslationSnap = std::clamp(snap_.translation, 0.001f, 100.0f);
    preferences.viewportRotationSnap = std::clamp(snap_.rotation, 0.1f, 180.0f);
    preferences.viewportScaleSnap = std::clamp(snap_.scale, 0.001f, 10.0f);
    preferences.viewportPickMeshEntities = pickMeshEntities_;
    preferences.viewportPickActorIcons = pickActorIcons_;
    preferences.save(path.empty() ? EditorPreferences::defaultPath() : path);
}

void ViewportPanel::executeCommand(EditorCommandId id) {
    switch (id) {
    case EditorCommandId::ViewportSelect:
        transformGizmoMode_ = -1;
        break;
    case EditorCommandId::ViewportMove:
        transformGizmoMode_ = 0;
        break;
    case EditorCommandId::ViewportRotate:
        transformGizmoMode_ = 1;
        break;
    case EditorCommandId::ViewportScale:
        transformGizmoMode_ = 2;
        break;
    case EditorCommandId::ViewportToggleLocal:
        localGizmoMode_ = !localGizmoMode_;
        break;
    case EditorCommandId::ViewportToggleSnap:
        snap_.enabled = !snap_.enabled;
        break;
    case EditorCommandId::ViewportToggleGrid:
        showGrid_ = !showGrid_;
        break;
    case EditorCommandId::ViewportToggleAxes:
        showAxes_ = !showAxes_;
        break;
    default:
        break;
    }
}

void ViewportPanel::updateGizmoState(bool isOver, bool isUsing, int gizmoMode) {
    switch (gizmoState_) {
        case GizmoInteractionState::Idle:
            if (isUsing) {
                gizmoState_ = gizmoMode == 0 ? GizmoInteractionState::DraggingTranslate
                    : (gizmoMode == 1 ? GizmoInteractionState::DraggingRotate
                    : GizmoInteractionState::DraggingScale);
            } else if (isOver) {
                gizmoState_ = GizmoInteractionState::Hovered;
            }
            break;
        case GizmoInteractionState::Hovered:
            if (isUsing) {
                gizmoState_ = gizmoMode == 0 ? GizmoInteractionState::DraggingTranslate
                    : (gizmoMode == 1 ? GizmoInteractionState::DraggingRotate
                    : GizmoInteractionState::DraggingScale);
            } else if (!isOver) {
                gizmoState_ = GizmoInteractionState::Idle;
            }
            break;
        case GizmoInteractionState::DraggingTranslate:
        case GizmoInteractionState::DraggingRotate:
        case GizmoInteractionState::DraggingScale:
            if (!isUsing) {
                gizmoState_ = isOver ? GizmoInteractionState::Hovered : GizmoInteractionState::Idle;
            }
            break;
    }
}

} // namespace rtv
