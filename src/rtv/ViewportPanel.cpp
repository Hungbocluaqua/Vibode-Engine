#include "rtv/ViewportPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/RendererDebug.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace rtv {

namespace {

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

void writeLocalTransformFromMatrix(Entity& entity, const glm::mat4& matrix) {
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
    entity.transform.scale = scale;
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
    return glm::perspectiveRH_ZO(fovY, aspect, 0.01f, 1000.0f);
}

SceneUpdateKind transformUpdateKind(const SceneDocument& document, const Entity& entity) {
    const bool hasMesh = entity.meshRenderer.has_value();
    const bool hasLight = entity.light.has_value();
    const bool hasActiveCamera = entity.camera.has_value() && document.activeCamera() == entity.id;
    if ((hasLight && hasMesh) || (hasActiveCamera && hasMesh) || (hasActiveCamera && hasLight)) {
        return SceneUpdateKind::TopologyChanged;
    }
    if (hasActiveCamera) {
        return SceneUpdateKind::CameraOnly;
    }
    if (hasLight) {
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
    const glm::vec3 center = glm::vec3(world[3]);
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

void drawSelectionOverlay(const EditorRuntimeState& state, const EditorSelection& selection) {
    if (state.sceneDocument == nullptr || state.camera == nullptr || !selection.entityId().valid()) {
        return;
    }
    const Entity* entity = state.sceneDocument->registry().entity(selection.entityId());
    if (entity == nullptr) {
        return;
    }
    const glm::mat4 view = editorViewMatrix(*state.camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), state.viewport.imageSize.x / state.viewport.imageSize.y);
    if (entity->light.has_value()) {
        drawSelectedLightOverlay(state, view, projection, *entity);
    }
}

} // namespace

void ViewportPanel::draw(EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Viewport")) {
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

        VkExtent2D expectedExtent = lastContentExtent_;
        const float scale = state.renderer.settings().renderResolutionScale;
        expectedExtent.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(expectedExtent.width) * scale));
        expectedExtent.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(expectedExtent.height) * scale));
        const bool imageMatchesPanel =
            expectedExtent.width == state.viewport.renderExtent.width &&
            expectedExtent.height == state.viewport.renderExtent.height;

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

        const RendererSettings& settings = state.renderer.settings();
        const GpuFrameTimings& timings = state.renderer.timings();
        const VkExtent2D extent = state.viewport.renderExtent;
        const float frameMs = timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs;
        bool gizmoHoveredOrUsing = false;

        if (focused_ && !state.viewport.mouseCaptureActive && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_T)) {
                transformGizmoMode_ = 0;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                transformGizmoMode_ = 1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_S)) {
                transformGizmoMode_ = 2;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_L)) {
                localGizmoMode_ = !localGizmoMode_;
            }
        }
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(imagePos.x + 10.0f, imagePos.y + 10.0f),
            ImVec2(imagePos.x + 360.0f, imagePos.y + 104.0f),
            IM_COL32(0, 0, 0, 150),
            4.0f);
        ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 18.0f, imagePos.y + 18.0f), IM_COL32_WHITE,
            ("Resolution: " + std::to_string(extent.width) + "x" + std::to_string(extent.height)).c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 18.0f, imagePos.y + 38.0f), IM_COL32_WHITE,
            ("Samples: " + std::to_string(state.renderer.sampleCount())).c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 18.0f, imagePos.y + 58.0f), IM_COL32_WHITE,
            rendererDebugViewName(settings.debugView));
        ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 18.0f, imagePos.y + 78.0f), IM_COL32_WHITE,
            ("Frame: " + std::to_string(frameMs) + " ms").c_str());

            drawSelectionOverlay(state, selection);

        if (state.sceneDocument != nullptr && selection.entityId().valid()) {
            Entity* entity = state.sceneDocument->registry().entity(selection.entityId());
            if (entity != nullptr && !entity->locked && state.camera != nullptr) {
                ImGui::SetCursorScreenPos(ImVec2(imagePos.x + 370.0f, imagePos.y + 10.0f));
                ImGui::BeginGroup();
                if (ImGui::RadioButton("T", transformGizmoMode_ == 0)) {
                    transformGizmoMode_ = 0;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("R", transformGizmoMode_ == 1)) {
                    transformGizmoMode_ = 1;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("S", transformGizmoMode_ == 2)) {
                    transformGizmoMode_ = 2;
                }
                ImGui::SameLine();
                ImGui::Checkbox("Local", &localGizmoMode_);
                ImGui::SameLine();
                ImGui::Checkbox("Snap", &snap_.enabled);
                if (snap_.enabled) {
                    ImGui::SameLine();
                    if (transformGizmoMode_ == 0) {
                        ImGui::SetNextItemWidth(74.0f);
                        ImGui::DragFloat("##snapTranslate", &snap_.translation, 0.01f, 0.001f, 100.0f, "%.2f");
                    } else if (transformGizmoMode_ == 1) {
                        ImGui::SetNextItemWidth(74.0f);
                        ImGui::DragFloat("##snapRotate", &snap_.rotation, 1.0f, 0.1f, 180.0f, "%.0f");
                    } else {
                        ImGui::SetNextItemWidth(74.0f);
                        ImGui::DragFloat("##snapScale", &snap_.scale, 0.01f, 0.001f, 10.0f, "%.2f");
                    }
                }
                ImGui::EndGroup();

                const glm::mat4 view = editorViewMatrix(*state.camera);
                const glm::mat4 projection = editorProjectionMatrix(
                    activeCameraFov(*state.sceneDocument),
                    state.viewport.imageSize.x / state.viewport.imageSize.y);

                glm::mat4 world = entityWorldMatrix(state.sceneDocument->registry(), *entity);
                const ImGuizmo::OPERATION operation = transformGizmoMode_ == 0
                    ? ImGuizmo::TRANSLATE
                    : (transformGizmoMode_ == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
                const glm::mat4 previousWorld = world;
                ImGuizmo::BeginFrame();
                ImGuizmo::SetOrthographic(false);
                ImGuizmo::SetDrawlist();
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
                gizmoHoveredOrUsing = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
                if (manipulated && world != previousWorld) {
                    const glm::mat4 local = glm::inverse(parentWorldMatrix(state.sceneDocument->registry(), *entity)) * world;
                    writeLocalTransformFromMatrix(*entity, local);
                    const SceneUpdateKind updateKind = transformUpdateKind(*state.sceneDocument, *entity);
                    state.sceneDocument->markDirty(updateKind);
                    requests.sceneUpdate = updateKind;
                }
            }
        }

        if (state.viewport.leftClicked && !gizmoHoveredOrUsing) {
            if (const std::optional<uint32_t> pickedInstance = state.renderer.pickInstanceId(state.viewport.mouseUv)) {
                if (const std::optional<EntityId> pickedEntity = entityForInstance(state, *pickedInstance)) {
                    const Entity* entity = state.sceneDocument != nullptr ? state.sceneDocument->registry().entity(*pickedEntity) : nullptr;
                    if (entity == nullptr || !entity->locked) {
                        selection.selectEntity(*pickedEntity);
                    }
                }
            }
        }

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

} // namespace rtv
