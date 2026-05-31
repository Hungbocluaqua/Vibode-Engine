#include "rtv/ViewportPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/EditorCommands.h"
#include "rtv/RendererDebug.h"
#include "rtv/SceneOperations.h"

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
#include <iomanip>
#include <optional>
#include <sstream>
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

} // namespace

void ViewportPanel::draw(EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Scene")) {
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
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("PREFAB_ASSET")) {
                requests.placeAsset = std::string(static_cast<const char*>(payload->Data));
            }
            ImGui::EndDragDropTarget();
        }

        const RendererSettings& settings = state.renderer.settings();
        const GpuFrameTimings& timings = state.renderer.timings();
        bool gizmoHoveredOrUsing = false;

        if (focused_ && !state.viewport.mouseCaptureActive && !ImGui::GetIO().WantTextInput) {
            auto commandPressed = [](EditorCommandId id) {
                const EditorCommand* command = editorCommand(id);
                return command != nullptr && command->defaultKeybinding.imguiKey >= 0 &&
                    ImGui::IsKeyPressed(static_cast<ImGuiKey>(command->defaultKeybinding.imguiKey));
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
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.045f, 0.050f, 0.058f, 0.82f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.115f, 0.150f, 0.205f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.160f, 0.225f, 0.330f, 1.00f));
        ImGui::SetCursorScreenPos(ImVec2(imagePos.x + 7.0f, imagePos.y + 5.0f));
        ImGui::BeginGroup();
        auto toolButton = [&](const char* label, EditorCommandId command, bool active) {
            ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.35f, 0.62f, 1.0f, 1.0f) : ImVec4(0.70f, 0.72f, 0.74f, 1.0f));
            const bool pressed = ImGui::SmallButton(label);
            ImGui::PopStyleColor();
            viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s (%s)", editorCommandName(command), editorCommandShortcut(command));
            }
            if (pressed) {
                executeCommand(command);
            }
            ImGui::SameLine();
        };
        toolButton("Q", EditorCommandId::ViewportSelect, transformGizmoMode_ < 0);
        toolButton("W", EditorCommandId::ViewportMove, transformGizmoMode_ == 0);
        toolButton("E", EditorCommandId::ViewportRotate, transformGizmoMode_ == 1);
        toolButton("R", EditorCommandId::ViewportScale, transformGizmoMode_ == 2);
        toolButton(localGizmoMode_ ? "Local" : "World", EditorCommandId::ViewportToggleLocal, localGizmoMode_);
        toolButton(snap_.enabled ? "Snap" : "Snap", EditorCommandId::ViewportToggleSnap, snap_.enabled);
        toolButton(showGrid_ ? "Grid" : "Grid", EditorCommandId::ViewportToggleGrid, showGrid_);
        toolButton(showAxes_ ? "Axes" : "Axes", EditorCommandId::ViewportToggleAxes, showAxes_);
        if (selection.entityId().valid()) {
            if (ImGui::SmallButton("Frame")) { requests.focusOnEntity = selection.entityId(); }
            viewportUiHovered = viewportUiHovered || ImGui::IsItemHovered();
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s (%s)", editorCommandName(EditorCommandId::ViewportFrameSelected), editorCommandShortcut(EditorCommandId::ViewportFrameSelected)); }
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
        ImGui::PopStyleColor(3);
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
        const char* actionsText = "View Settings    Stats    Draw Debug    1.000";
        const ImVec2 statusSize = ImGui::CalcTextSize(statusText.c_str());
        const ImVec2 actionsSize = ImGui::CalcTextSize(actionsText);
        const float statusRight = imagePos.x + avail.x - 12.0f;
        const float statusY = imagePos.y + 7.0f;
        dl->AddText(ImVec2(statusRight - actionsSize.x, statusY), IM_COL32(170, 173, 176, 230), actionsText);
        dl->AddText(ImVec2(statusRight - actionsSize.x - statusSize.x - 18.0f, statusY), IM_COL32(186, 188, 190, 235), statusText.c_str());

        lastSampleCount_ = state.renderer.sampleCount();

        drawSelectionOverlay(state, selection);

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

                    if (isUsing && gizmoState_ == GizmoInteractionState::Idle) {
                        gizmoDragActive_ = true;
                        gizmoDragEntity_ = entity->id;
                        gizmoDragOriginal_ = entity->transform;
                    }

                    if (manipulated && world != previousWorld) {
                        const glm::mat4 local = glm::inverse(parentWorldMatrix(state.sceneDocument->registry(), *entity)) * world;
                        writeLocalTransformFromMatrix(*entity, local);
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
    if (entity != nullptr) {
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
    gizmoDragEntity_ = {};
    gizmoDragOriginal_ = {};
}

void ViewportPanel::abortGizmoDrag() {
    if (!gizmoDragActive_) {
        return;
    }
    gizmoDragActive_ = false;
    gizmoDragEntity_ = {};
    gizmoDragOriginal_ = {};
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
