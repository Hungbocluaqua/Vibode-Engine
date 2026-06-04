#include "rtv/ViewportPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/EditorCommands.h"
#include "rtv/EditorPreferences.h"
#include "rtv/EditorTransformUtils.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/RendererDebug.h"
#include "rtv/SceneOperations.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

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

struct ViewportDropPreview {
    bool active = false;
    bool placement = false;
    std::string title;
    std::string detail;
};

struct ViewportSceneRayHit {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
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
std::optional<Transform> viewportDropPlacementTransform(
    const EditorRuntimeState& state,
    bool snapEnabled,
    float translationSnap,
    bool forceGrid,
    bool surfaceAlign);

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
    bool surfaceAlign) {
    if (state.camera == nullptr || state.sceneDocument == nullptr) {
        return;
    }

    const glm::mat4 view = editorViewMatrix(*state.camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));
    const glm::mat4 world = transform.localMatrix();
    constexpr float halfExtent = 0.55f;
    const std::array<glm::vec3, 4> localCorners = {
        glm::vec3{-halfExtent, 0.0f, -halfExtent},
        glm::vec3{ halfExtent, 0.0f, -halfExtent},
        glm::vec3{ halfExtent, 0.0f,  halfExtent},
        glm::vec3{-halfExtent, 0.0f,  halfExtent},
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

void drawViewportDropPreview(const EditorRuntimeState& state, bool snapEnabled, float translationSnap) {
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
    bool forceGrid = false;
    bool surfaceAlign = false;
    if (preview.placement) {
        forceGrid = ImGui::GetIO().KeyCtrl;
        surfaceAlign = ImGui::GetIO().KeyAlt && !forceGrid;
        if (forceGrid) {
            label << "  Grid";
        } else if (surfaceAlign) {
            label << "  Align";
        }
        label << (snapEnabled ? "  Snap " : "  Free");
        if (snapEnabled) {
            label << std::fixed << std::setprecision(2) << translationSnap;
        }
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 cursor(state.viewport.mousePosition.x, state.viewport.mousePosition.y);
    constexpr float markerSize = 11.0f;
    const ImU32 markerColor = IM_COL32(110, 205, 190, 255);
    drawList->AddCircle(cursor, markerSize, markerColor, 24, 2.0f);
    drawList->AddLine(ImVec2(cursor.x - markerSize - 4.0f, cursor.y), ImVec2(cursor.x + markerSize + 4.0f, cursor.y), markerColor, 2.0f);
    drawList->AddLine(ImVec2(cursor.x, cursor.y - markerSize - 4.0f), ImVec2(cursor.x, cursor.y + markerSize + 4.0f), markerColor, 2.0f);

    if (preview.placement) {
        if (const std::optional<Transform> transform = viewportDropPlacementTransform(state, snapEnabled, translationSnap, forceGrid, surfaceAlign)) {
            drawViewportDropPlacementFootprint(state, *transform, forceGrid, surfaceAlign);
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
    const glm::vec3& rayDir) {
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

std::optional<Transform> viewportDropPlacementTransform(
    const EditorRuntimeState& state,
    bool snapEnabled,
    float translationSnap,
    bool forceGrid,
    bool surfaceAlign) {
    glm::vec3 origin{};
    glm::vec3 rayDir{};
    if (!viewportRayFromMouse(state, origin, rayDir)) {
        return std::nullopt;
    }

    glm::vec3 position = origin + rayDir * 5.0f;
    glm::vec3 rotationEuler{0.0f};
    bool usedSceneHit = false;
    if (!forceGrid) {
        if (const std::optional<ViewportSceneRayHit> sceneHit = viewportSceneRaycast(state, origin, rayDir)) {
            position = sceneHit->position;
            if (surfaceAlign) {
                rotationEuler = rotationEulerAligningUpToNormal(sceneHit->normal);
            }
            usedSceneHit = true;
        }
    }
    if (forceGrid || !usedSceneHit) {
        if (std::abs(rayDir.y) > 0.0001f) {
            const float t = -origin.y / rayDir.y;
            if (t > 0.0f && std::isfinite(t)) {
                position = origin + rayDir * t;
            }
            position.y = 0.0f;
        }
    }
    if (snapEnabled) {
        position.x = snappedPlacementCoordinate(position.x, translationSnap);
        position.y = snappedPlacementCoordinate(position.y, translationSnap);
        position.z = snappedPlacementCoordinate(position.z, translationSnap);
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
        if (viewportContentHovered) {
            drawViewportDropPreview(state, snap_.enabled, snap_.translation);
        }
        if (ImGui::BeginDragDropTarget()) {
            const bool forceGridDrop = ImGui::GetIO().KeyCtrl;
            const bool surfaceAlignDrop = ImGui::GetIO().KeyAlt && !forceGridDrop;
            if (const auto* payload = ImGui::AcceptDragDropPayload("PREFAB_ASSET")) {
                requests.placeAsset = std::string(static_cast<const char*>(payload->Data));
                requests.placeAssetTransform = viewportDropPlacementTransform(state, snap_.enabled, snap_.translation, forceGridDrop, surfaceAlignDrop);
            }
            if (const auto* payload = ImGui::AcceptDragDropPayload("MESH_ASSET")) {
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
                requests.meshAssetPlacement = EditorMeshAssetPlacement{
                    .meshGuid = std::string(static_cast<const char*>(payload->Data)),
                    .placementTransform = viewportDropPlacementTransform(state, snap_.enabled, snap_.translation, forceGridDrop, surfaceAlignDrop),
                    .replaceEntity = replaceEntity,
                };
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
            editorGlyphMenuItem(EditorGlyphIcon::Group, "Add As Sublevel", false);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Level instances and sublevels are not implemented yet");
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
        if (viewportContentHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
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
                requests.deleteEntity = selection.entityId();
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
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
            editorGlyphMenuItem(EditorGlyphIcon::Add, "Drop Content asset here", false);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Drag a prefab, mesh, material, HDRI, or level asset from Content onto the viewport");
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
                    ImGui::DragFloat("##snapTranslate", &snap_.translation, 0.01f, 0.001f, 100.0f, "%.2f");
                } else if (transformGizmoMode_ == 1) {
                    ImGui::DragFloat("##snapRotate", &snap_.rotation, 1.0f, 0.1f, 180.0f, "%.0f");
                } else {
                    ImGui::DragFloat("##snapScale", &snap_.scale, 0.01f, 0.001f, 10.0f, "%.2f");
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
        const ImVec2 statusSize = ImGui::CalcTextSize(statusText.c_str());
        const float statusRight = imagePos.x + avail.x - 6.0f;
        const float statusY = imagePos.y + 4.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, EditorUiMetric::rowPaddingY));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, EditorUiMetric::compactButtonRounding);
        std::string cameraSpeedText;
        if (state.camera != nullptr) {
            std::ostringstream cameraSpeed;
            cameraSpeed << std::fixed << std::setprecision(3) << state.camera->moveSpeed();
            cameraSpeedText = cameraSpeed.str();
        }
        const float cameraSpeedWidth = cameraSpeedText.empty() ? 0.0f : editorIconTextButtonWidth(cameraSpeedText.c_str()) + ImGui::GetStyle().ItemSpacing.x;
        const float overlayWidth = editorIconTextButtonWidth("View Settings") + editorIconTextButtonWidth("Stats") +
            editorIconTextButtonWidth("Draw Debug") + cameraSpeedWidth + ImGui::GetStyle().ItemSpacing.x * 3.0f;
        const float controlsX = std::max(imagePos.x + 8.0f, statusRight - overlayWidth);
        const float statusX = std::max(imagePos.x + 8.0f, controlsX - statusSize.x - 14.0f);
        dl->AddText(ImVec2(statusX, statusY), IM_COL32(216, 221, 228, 245), statusText.c_str());

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
        ImGui::SameLine();
        overlayButton(EditorGlyphIcon::Stats, "Stats", "ViewportStats", "ViewportStatsButton", "Frame timing and render statistics");
        ImGui::SameLine();
        overlayButton(EditorGlyphIcon::DrawDebug, "Draw Debug", "ViewportDrawDebug", "ViewportDrawDebugButton", "Viewport debug drawing controls");
        if (!cameraSpeedText.empty()) {
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
            ImGui::Checkbox("Grid", &showGrid_);
            ImGui::Checkbox("Axes", &showAxes_);
            ImGui::Checkbox("Local transform frame", &localGizmoMode_);
            ImGui::Checkbox("Snap", &snap_.enabled);
            if (snap_.enabled) {
                ImGui::SetNextItemWidth(140.0f);
                ImGui::DragFloat("Translate", &snap_.translation, 0.01f, 0.001f, 100.0f, "%.2f");
                ImGui::SetNextItemWidth(140.0f);
                ImGui::DragFloat("Rotate", &snap_.rotation, 1.0f, 0.1f, 180.0f, "%.0f");
                ImGui::SetNextItemWidth(140.0f);
                ImGui::DragFloat("Scale", &snap_.scale, 0.01f, 0.001f, 10.0f, "%.2f");
            }
            if (state.camera != nullptr) {
                ImGui::SeparatorText("Navigation");
                float cameraSpeed = state.camera->moveSpeed();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::DragFloat("Camera Speed", &cameraSpeed, 0.05f, 0.05f, 100.0f, "%.3f")) {
                    state.camera->setMoveSpeed(std::clamp(cameraSpeed, 0.05f, 100.0f));
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

        if (state.sceneDocument != nullptr && selection.entityId().valid()) {
            Entity* entity = state.sceneDocument->registry().entity(selection.entityId());
            if (entity != nullptr && !entity->locked && state.camera != nullptr) {
                if (transformGizmoMode_ >= 0) {
                    const glm::mat4 view = editorViewMatrix(*state.camera);
                    const glm::mat4 projection = editorProjectionMatrix(
                        activeCameraFov(*state.sceneDocument),
                        viewportAspect(state));

                    glm::mat4 world = entityWorldMatrix(state.sceneDocument->registry(), *entity);
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

                    if (isUsing && (!gizmoDragActive_ || gizmoDragEntity_ != entity->id)) {
                        gizmoDragActive_ = true;
                        gizmoDragModified_ = false;
                        gizmoDragEntity_ = entity->id;
                        gizmoDragOriginal_ = entity->transform;
                        gizmoDragParentWorld_ = parentWorldMatrix(state.sceneDocument->registry(), *entity);
                        gizmoDragOriginalWorld_ = previousWorld;
                    }

                    if (manipulated && world != previousWorld) {
                        const glm::mat4 parentWorld = gizmoDragActive_ && gizmoDragEntity_ == entity->id
                            ? gizmoDragParentWorld_
                            : parentWorldMatrix(state.sceneDocument->registry(), *entity);
                        const glm::mat4 local = glm::inverse(parentWorld) * world;
                        const bool linkedScale = state.editorPrefs != nullptr && state.editorPrefs->linkedScale;
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

        if (state.viewport.leftClicked && !gizmoHoveredOrUsing && !viewportUiHovered) {
            state.renderer.requestPickInstanceId(state.viewport.mouseUv);
        }
        selection.setPickPending(state.renderer.pickPending());

        const bool gizmoDragging = gizmoState_ == GizmoInteractionState::DraggingTranslate
            || gizmoState_ == GizmoInteractionState::DraggingRotate
            || gizmoState_ == GizmoInteractionState::DraggingScale;
        state.viewport.mouseCaptureActive = state.viewport.mouseCaptureActive || gizmoDragging;

        state.renderer.setSelectedInstanceId(instanceForEntity(state, selection.entityId()));
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
    Entity* entity = document.registry().entity(gizmoDragEntity_);
    if (entity != nullptr && gizmoDragModified_) {
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
