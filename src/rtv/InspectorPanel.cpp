#include "rtv/InspectorPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/SunController.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace rtv {

namespace {

void drawMatrixTranslation(const glm::mat4& transform) {
    glm::vec3 translation{transform[3]};
    ImGui::InputFloat3("Position", glm::value_ptr(translation), "%.3f", ImGuiInputTextFlags_ReadOnly);
}

void ensureMaterialSlots(MeshRenderer& renderer, const AssetManager* assets) {
    if (!renderer.materialSlots.empty() || assets == nullptr) {
        return;
    }
    const MeshAsset* mesh = assets->mesh(renderer.mesh);
    if (mesh == nullptr) {
        return;
    }
    renderer.materialSlots.reserve(mesh->primitives.size());
    for (size_t i = 0; i < mesh->primitives.size(); ++i) {
        renderer.materialSlots.push_back(MaterialSlot{
            .name = "Primitive " + std::to_string(i),
            .material = mesh->primitives[i].material,
        });
    }
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

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

bool resetFieldButton(const char* id, const char* tooltipText = "Reset to default") {
    ImGui::SameLine();
    const std::string buttonId = std::string("InspectorReset") + id;
    const bool clicked = editorIconButton(buttonId.c_str(), EditorGlyphIcon::Reset, false, ImVec2(22.0f, 20.0f));
    tooltip(tooltipText);
    return clicked;
}

bool inspectorActionButton(const char* id, const char* tooltipText) {
    const bool clicked = editorIconButton(id, EditorGlyphIcon::More, false, ImVec2(22.0f, 20.0f));
    tooltip(tooltipText);
    return clicked;
}

bool beginInspectorPropertyRow(const char* label) {
    const std::string tableId = std::string("InspectorPropertyRow##") + label;
    if (!ImGui::BeginTable(tableId.c_str(), 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
        return false;
    }
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::inspectorLabelWidth);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow(ImGuiTableRowFlags_None, EditorUiMetric::inspectorRowHeight);
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return true;
}

void endInspectorPropertyRow() {
    ImGui::PopID();
    ImGui::EndTable();
}

bool inspectorCheckboxRow(const char* label, bool* value) {
    if (!beginInspectorPropertyRow(label)) {
        return false;
    }
    const bool changed = ImGui::Checkbox("##Value", value);
    endInspectorPropertyRow();
    return changed;
}

bool inspectorDragFloatRow(const char* label, float* value, float speed, float minValue, float maxValue, const char* format) {
    if (!beginInspectorPropertyRow(label)) {
        return false;
    }
    const bool changed = ImGui::DragFloat("##Value", value, speed, minValue, maxValue, format);
    endInspectorPropertyRow();
    return changed;
}

bool inspectorDragIntRow(const char* label, int* value, float speed, int minValue, int maxValue) {
    if (!beginInspectorPropertyRow(label)) {
        return false;
    }
    const bool changed = ImGui::DragInt("##Value", value, speed, minValue, maxValue);
    endInspectorPropertyRow();
    return changed;
}

bool inspectorDragFloat3Row(const char* label, float* values, float speed, float minValue, float maxValue, const char* format) {
    if (!beginInspectorPropertyRow(label)) {
        return false;
    }
    const bool changed = ImGui::DragFloat3("##Value", values, speed, minValue, maxValue, format);
    endInspectorPropertyRow();
    return changed;
}

bool inspectorSliderFloatRow(const char* label, float* value, float minValue, float maxValue, const char* format) {
    if (!beginInspectorPropertyRow(label)) {
        return false;
    }
    const bool changed = ImGui::SliderFloat("##Value", value, minValue, maxValue, format);
    endInspectorPropertyRow();
    return changed;
}

bool inspectorColorEdit3Row(const char* label, float* values) {
    if (!beginInspectorPropertyRow(label)) {
        return false;
    }
    const bool changed = ImGui::ColorEdit3("##Value", values);
    endInspectorPropertyRow();
    return changed;
}

bool inspectorColorEdit4Row(const char* label, float* values) {
    if (!beginInspectorPropertyRow(label)) {
        return false;
    }
    const bool changed = ImGui::ColorEdit4("##Value", values);
    endInspectorPropertyRow();
    return changed;
}

bool inspectorComboRow(const char* label, int* value, const char* items) {
    if (!beginInspectorPropertyRow(label)) {
        return false;
    }
    const bool changed = ImGui::Combo("##Value", value, items);
    endInspectorPropertyRow();
    return changed;
}

void inspectorReadonlyRow(const char* label, const char* value) {
    if (!beginInspectorPropertyRow(label)) {
        return;
    }
    ImGui::TextDisabled("%s", value);
    endInspectorPropertyRow();
}

void inspectorReadonlyRow(const char* label, const std::string& value) {
    inspectorReadonlyRow(label, value.c_str());
}

void drawInspectorComponentActionsPopup(
    const char* popupId,
    EntityId entity,
    EditorComponentKind kind,
    SceneUpdateKind updateKind,
    EditorRequests& requests) {
    if (ImGui::BeginPopup(popupId)) {
        editorGlyphMenuItem(EditorGlyphIcon::Reset, "Reset to Defaults", false);
        editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Component", false);
        editorGlyphMenuItem(EditorGlyphIcon::Command, "Paste Values", false);
        ImGui::Separator();
        if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove Component")) {
            requests.removeComponent = EditorComponentRequest{.entity = entity, .kind = kind};
            requests.sceneUpdate = updateKind;
        }
        ImGui::EndPopup();
    }
}

void drawInspectorComponentHeader(
    EditorGlyphIcon icon,
    const char* title,
    const char* detail = nullptr,
    bool enabled = true,
    const char* popupId = nullptr,
    EntityId entity = {},
    EditorComponentKind kind = EditorComponentKind::Light,
    SceneUpdateKind updateKind = SceneUpdateKind::None,
    EditorRequests* requests = nullptr) {
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 size(width, EditorUiMetric::inspectorComponentHeaderHeight);
    const ImVec2 min = cursor;
    const ImVec2 max(cursor.x + size.x, cursor.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = enabled ? IM_COL32(38, 41, 46, 238) : IM_COL32(25, 27, 31, 214);
    const ImU32 border = enabled ? IM_COL32(61, 66, 74, 150) : IM_COL32(44, 48, 56, 130);
    dl->AddRectFilled(min, max, bg, EditorUiMetric::compactButtonRounding);
    dl->AddRect(min, max, border, EditorUiMetric::compactButtonRounding);

    const float iconSize = EditorUiMetric::inspectorComponentHeaderIconSize;
    const ImVec2 iconMin(min.x + 8.0f, min.y + (size.y - iconSize) * 0.5f);
    const ImVec4 iconTint = enabled ? editorIconTint(false) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    editorDrawIconGlyph(icon, iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize), ImGui::GetColorU32(iconTint));

    const ImU32 titleColor = ImGui::GetColorU32(enabled ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const ImU32 detailColor = ImGui::GetColorU32(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    const float textX = iconMin.x + iconSize + 14.0f;
    const float titleY = detail != nullptr ? min.y + 4.0f : min.y + (size.y - titleSize.y) * 0.5f;
    dl->AddText(ImVec2(textX, titleY), titleColor, title);
    if (detail != nullptr && detail[0] != '\0') {
        dl->AddText(ImVec2(textX, min.y + 17.0f), detailColor, detail);
    }

    ImGui::Dummy(size);
    if (popupId != nullptr && requests != nullptr) {
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const float actionSize = EditorUiMetric::inspectorComponentActionSize;
        ImGui::SetCursorScreenPos(ImVec2(max.x - actionSize - 5.0f, min.y + (size.y - actionSize) * 0.5f));
        const std::string buttonId = std::string("ComponentActions") + popupId;
        if (inspectorActionButton(buttonId.c_str(), "Component actions")) {
            ImGui::OpenPopup(popupId);
        }
        drawInspectorComponentActionsPopup(popupId, entity, kind, updateKind, *requests);
        ImGui::SetCursorScreenPos(after);
    }
}

void addComponentMenuItem(
    const char* label,
    Entity& entity,
    EditorComponentKind kind,
    SceneUpdateKind updateKind,
    EditorGlyphIcon icon,
    bool alreadyAttached,
    EditorRequests& requests) {
    if (editorGlyphMenuItem(icon, label, !alreadyAttached)) {
        requests.addComponent = EditorComponentRequest{.entity = entity.id, .kind = kind};
        requests.sceneUpdate = updateKind;
    }
}

void drawEntityActionsMenu(Entity& entity, EditorSelection& selection, EditorRequests& requests) {
    if (inspectorActionButton("EntityActions", "Entity actions")) {
        ImGui::OpenPopup("EntityActionsPopup");
    }
    if (!ImGui::BeginPopup("EntityActionsPopup")) {
        return;
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Duplicate Entity", !entity.locked)) {
        requests.duplicateEntity = entity.id;
        requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete Entity", !entity.locked)) {
        requests.deleteEntity = entity.id;
        requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        selection.clear();
    }
    ImGui::Separator();
    if (editorGlyphMenuItem(entity.visible ? EditorGlyphIcon::EyeHidden : EditorGlyphIcon::EyeVisible, entity.visible ? "Hide Entity" : "Show Entity")) {
        requests.setEntityVisibility = EditorEntityBoolChange{.entity = entity.id, .value = !entity.visible};
    }
    if (editorGlyphMenuItem(entity.locked ? EditorGlyphIcon::Unlock : EditorGlyphIcon::Lock, entity.locked ? "Unlock Entity" : "Lock Entity")) {
        requests.setEntityLocked = EditorEntityBoolChange{.entity = entity.id, .value = !entity.locked};
    }
    ImGui::Separator();
    if (editorGlyphBeginMenu(EditorGlyphIcon::Add, "Add Component", !entity.locked)) {
        addComponentMenuItem("Light", entity, EditorComponentKind::Light, SceneUpdateKind::TopologyChanged, EditorGlyphIcon::Light, entity.light.has_value(), requests);
        addComponentMenuItem("Primary Sun", entity, EditorComponentKind::Sun, SceneUpdateKind::LightOnly, EditorGlyphIcon::Sun, entity.sun.has_value(), requests);
        addComponentMenuItem("Camera", entity, EditorComponentKind::Camera, SceneUpdateKind::TopologyChanged, EditorGlyphIcon::Camera, entity.camera.has_value(), requests);
        addComponentMenuItem("Mesh Renderer", entity, EditorComponentKind::MeshRenderer, SceneUpdateKind::TopologyChanged, EditorGlyphIcon::Model, entity.meshRenderer.has_value(), requests);
        addComponentMenuItem("Environment Light", entity, EditorComponentKind::EnvironmentLight, SceneUpdateKind::EnvironmentOnly, EditorGlyphIcon::Environment, entity.environmentLight.has_value(), requests);
        addComponentMenuItem("Sky Atmosphere", entity, EditorComponentKind::SkyAtmosphere, SceneUpdateKind::EnvironmentOnly, EditorGlyphIcon::Sky, entity.skyAtmosphere.has_value(), requests);
        addComponentMenuItem("Height Fog", entity, EditorComponentKind::HeightFog, SceneUpdateKind::EnvironmentOnly, EditorGlyphIcon::Fog, entity.heightFog.has_value(), requests);
        addComponentMenuItem("Volumetric Cloud", entity, EditorComponentKind::VolumetricCloud, SceneUpdateKind::EnvironmentOnly, EditorGlyphIcon::Cloud, entity.volumetricCloud.has_value(), requests);
        addComponentMenuItem("Post Process Volume", entity, EditorComponentKind::PostProcessVolume, SceneUpdateKind::RendererSettingsOnly, EditorGlyphIcon::PostProcess, entity.postProcessVolume.has_value(), requests);
        addComponentMenuItem("Camera Post Process", entity, EditorComponentKind::CameraPostProcess, SceneUpdateKind::RendererSettingsOnly, EditorGlyphIcon::PostProcess, !entity.camera.has_value() || entity.cameraPostProcess.has_value(), requests);
        ImGui::EndMenu();
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Exit, "Clear Selection")) {
        selection.clear();
    }
    ImGui::EndPopup();
}

void drawInspectorStateCard(EditorGlyphIcon icon, const char* title, const char* detail) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 size(width, 78.0f);
    const ImVec2 min = cursor;
    const ImVec2 max(cursor.x + size.x, cursor.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(17, 20, 26, 255), EditorUiMetric::cardRounding);
    dl->AddRect(min, max, IM_COL32(48, 56, 70, 210), EditorUiMetric::cardRounding);

    const ImVec2 iconMin(min.x + 14.0f, min.y + 18.0f);
    editorDrawIconGlyph(icon, iconMin, ImVec2(iconMin.x + 30.0f, iconMin.y + 30.0f), IM_COL32(112, 144, 190, 235));
    dl->AddText(ImVec2(min.x + 62.0f, min.y + 16.0f), IM_COL32(220, 226, 235, 255), title);
    dl->AddText(ImVec2(min.x + 62.0f, min.y + 40.0f), IM_COL32(142, 150, 164, 255), detail);
    ImGui::Dummy(size);
}

void drawInspectorLockedBanner() {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 size(width, 42.0f);
    const ImVec2 min = cursor;
    const ImVec2 max(cursor.x + size.x, cursor.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(23, 22, 20, 235), EditorUiMetric::cardRounding);
    dl->AddRect(min, max, IM_COL32(92, 78, 46, 215), EditorUiMetric::cardRounding);
    const ImVec2 iconMin(min.x + 11.0f, min.y + 11.0f);
    editorDrawIconGlyph(EditorGlyphIcon::Lock, iconMin, ImVec2(iconMin.x + 18.0f, iconMin.y + 18.0f), IM_COL32(214, 178, 98, 235));
    dl->AddText(ImVec2(min.x + 45.0f, min.y + 6.0f), IM_COL32(224, 218, 204, 255), "Entity locked");
    dl->AddText(ImVec2(min.x + 45.0f, min.y + 23.0f), IM_COL32(157, 150, 137, 255), "Unlock from the entity actions menu to edit components.");
    ImGui::Dummy(size);
    ImGui::Spacing();
}

void drawInspectorAddComponentButton(
    const char* id,
    EditorGlyphIcon icon,
    const char* label,
    Entity& entity,
    EditorComponentKind kind,
    SceneUpdateKind updateKind,
    EditorRequests& requests,
    float rowWidth,
    float& rowUsedWidth) {
    const float width = editorIconTextButtonWidth(label);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    if (rowUsedWidth > 0.0f && rowUsedWidth + spacing + width <= rowWidth) {
        ImGui::SameLine();
        rowUsedWidth += spacing + width;
    } else {
        rowUsedWidth = width;
    }
    if (editorIconTextButton(id, icon, label)) {
        requests.addComponent = EditorComponentRequest{.entity = entity.id, .kind = kind};
        requests.sceneUpdate = updateKind;
    }
}

} // namespace

void InspectorPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    if (!ImGui::Begin(EditorDockWindowTitle::Inspector)) {
        ImGui::End();
        return;
    }

    const EditorSelectionId current = selection.current();
    const size_t selCount = selection.selectionCount();
    if (!current.valid() && selCount == 0) {
        drawInspectorStateCard(EditorGlyphIcon::Details, "No selection", "Select an entity in the Hierarchy or viewport.");
        ImGui::End();
        return;
    }

    if (selCount > 1) {
        const std::string title = std::to_string(selCount) + " entities selected";
        drawInspectorStateCard(EditorGlyphIcon::Group, title.c_str(), "Bulk component editing is not available in this build.");
        if (editorIconTextButton("InspectorDeleteSelected", EditorGlyphIcon::Trash, "Delete Selected")) {
            for (EntityId id : selection.selectedEntities()) {
                requests.deleteEntity = id;
            }
        }
        ImGui::SameLine();
        if (editorIconTextButton("InspectorClearSelection", EditorGlyphIcon::Exit, "Clear Selection")) {
            selection.clear();
        }
        ImGui::End();
        return;
    }

    if (state.sceneDocument != nullptr && current.entity.valid()) {
        SceneDocument& document = *state.sceneDocument;
        Entity* entity = document.registry().entity(current.entity);
        if (entity == nullptr) {
            selection.clear();
            drawInspectorStateCard(EditorGlyphIcon::Details, "Selection unavailable", "The selected entity no longer exists.");
            ImGui::End();
            return;
        }

        const bool entityLocked = entity->locked;
        if (entityLocked) {
            drawInspectorLockedBanner();
        }
        ImGui::TextDisabled("Entity");
        ImGui::SameLine();
        drawEntityActionsMenu(*entity, selection, requests);
        ImGui::BeginDisabled(entityLocked);

        static EntityId nameEditId{};
        static std::array<char, 128> nameBuffer{};
        if (nameEditId != entity->id) {
            nameEditId = entity->id;
            nameBuffer.fill('\0');
            const std::string name = entity->name;
            name.copy(nameBuffer.data(), std::min(name.size(), nameBuffer.size() - 1u));
        }
        if (ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            requests.renameEntity = EditorEntityRenameRequest{.entity = entity->id, .name = nameBuffer.data()};
        }

        drawInspectorComponentHeader(EditorGlyphIcon::Move, "Transform", "Local position, rotation, and scale", !entityLocked);
        bool transformChanged = false;
        const Transform oldTransform = entity->transform;
        static std::optional<Transform> copiedTransform;
        if (editorIconTextButton("InspectorQuickResetPosition", EditorGlyphIcon::Reset, "Position")) {
            entity->transform.position = glm::vec3(0.0f);
            transformChanged = true;
        }
        tooltip("Reset position");
        ImGui::SameLine();
        if (editorIconTextButton("InspectorQuickResetRotation", EditorGlyphIcon::Reset, "Rotation")) {
            entity->transform.rotationEuler = glm::vec3(0.0f);
            transformChanged = true;
        }
        tooltip("Reset rotation");
        ImGui::SameLine();
        if (editorIconTextButton("InspectorQuickResetScale", EditorGlyphIcon::Reset, "Scale")) {
            entity->transform.scale = glm::vec3(1.0f);
            transformChanged = true;
        }
        tooltip("Reset scale");
        if (editorIconTextButton("InspectorCopyTransform", EditorGlyphIcon::Command, "Copy Transform")) {
            copiedTransform = entity->transform;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!copiedTransform.has_value());
        if (editorIconTextButton("InspectorPasteTransform", EditorGlyphIcon::Command, "Paste Transform") && copiedTransform.has_value()) {
            entity->transform = *copiedTransform;
            transformChanged = true;
        }
        ImGui::EndDisabled();
        transformChanged |= inspectorDragFloat3Row("Position", glm::value_ptr(entity->transform.position), 0.02f, -10000.0f, 10000.0f, "%.3f");
        if (resetFieldButton("TransformPosition")) {
            entity->transform.position = glm::vec3(0.0f);
            transformChanged = true;
        }
        glm::vec3 rotationDegrees = glm::degrees(entity->transform.rotationEuler);
        if (inspectorDragFloat3Row("Rotation", glm::value_ptr(rotationDegrees), 0.2f, -360.0f, 360.0f, "%.2f")) {
            entity->transform.rotationEuler = glm::radians(rotationDegrees);
            transformChanged = true;
        }
        tooltip("Euler angles in degrees.");
        if (resetFieldButton("TransformRotation")) {
            entity->transform.rotationEuler = glm::vec3(0.0f);
            transformChanged = true;
        }
        transformChanged |= inspectorDragFloat3Row("Scale", glm::value_ptr(entity->transform.scale), 0.02f, 0.001f, 1000.0f, "%.3f");
        if (resetFieldButton("TransformScale")) {
            entity->transform.scale = glm::vec3(1.0f);
            transformChanged = true;
        }
        if (transformChanged) {
            entity->transform.dirty = true;
            const SceneUpdateKind updateKind = transformUpdateKind(document, *entity);
            document.markDirty(updateKind);
            requests.sceneUpdate = updateKind;
            requests.setEntityTransform = EditorEntityTransformChange{
                .entity = entity->id,
                .oldTransform = oldTransform,
                .newTransform = entity->transform,
            };
        }

        if (entity->camera.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::Camera, "Camera", "Lens, exposure, and post-process settings", !entityLocked, "##CameraActions", entity->id, EditorComponentKind::Camera, SceneUpdateKind::TopologyChanged, &requests);
            Camera& camera = *entity->camera;
            const Camera oldCamera = camera;
            const EntityId oldActiveCamera = document.activeCamera();
            const SceneDocument beforeRenderSettings = document;
            bool changed = false;
            float fovDegrees = glm::degrees(camera.verticalFovRadians);
            if (inspectorSliderFloatRow("Vertical FOV", &fovDegrees, 20.0f, 120.0f, "%.1f")) {
                camera.verticalFovRadians = glm::radians(fovDegrees);
                changed = true;
            }
            if (resetFieldButton("CameraFov")) {
                camera.verticalFovRadians = Camera{}.verticalFovRadians;
                changed = true;
            }
            changed |= inspectorDragFloatRow("Near Plane", &camera.nearPlane, 0.005f, 0.001f, 100.0f, "%.3f");
            if (resetFieldButton("CameraNear")) {
                camera.nearPlane = Camera{}.nearPlane;
                changed = true;
            }
            changed |= inspectorDragFloatRow("Far Plane", &camera.farPlane, 1.0f, camera.nearPlane + 0.01f, 100000.0f, "%.1f");
            if (resetFieldButton("CameraFar")) {
                camera.farPlane = Camera{}.farPlane;
                changed = true;
            }
            RenderSettings& render = document.renderSettings();
            bool renderChanged = false;
            if (ImGui::TreeNodeEx("Physical Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
                renderChanged |= inspectorCheckboxRow("Use Physical Exposure", &render.usePhysicalCamera);
                renderChanged |= inspectorDragFloatRow("Aperture Size", &render.physicalAperture, 0.1f, 1.0f, 64.0f, "f/%.1f");
                renderChanged |= inspectorDragFloatRow("ISO", &render.physicalIso, 5.0f, 25.0f, 51200.0f, "%.0f");
                renderChanged |= inspectorDragFloatRow("Shutter Speed", &render.physicalShutterSeconds, 0.001f, 1.0f / 8000.0f, 30.0f, "%.4f s");
                renderChanged |= inspectorDragFloatRow("Exposure Compensation", &render.physicalExposureCompensation, 0.05f, -10.0f, 10.0f, "%.2f EV");
                if (inspectorDragFloatRow("Film Size", &fovDegrees, 0.1f, 20.0f, 120.0f, "%.1f FOV")) {
                    camera.verticalFovRadians = glm::radians(fovDegrees);
                    changed = true;
                }
                renderChanged |= inspectorDragFloatRow("Focal Length", &render.dofFocusDistance, 0.05f, 0.01f, 10000.0f, "%.2f");
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Post Process", ImGuiTreeNodeFlags_DefaultOpen)) {
                renderChanged |= inspectorCheckboxRow("Motion Blur", &render.motionBlurEnabled);
                bool dofEnabled = render.dofApertureRadius > 0.0f;
                if (inspectorCheckboxRow("DOF", &dofEnabled)) {
                    render.dofApertureRadius = dofEnabled ? std::max(render.dofApertureRadius, 0.02f) : 0.0f;
                    renderChanged = true;
                }
                renderChanged |= inspectorDragFloatRow("DOF Aperture Radius", &render.dofApertureRadius, 0.005f, 0.0f, 10.0f, "%.3f");
                renderChanged |= inspectorDragFloatRow("DOF Focus Distance", &render.dofFocusDistance, 0.05f, 0.01f, 10000.0f, "%.2f");
                if (entity->cameraPostProcess.has_value()) {
                    CameraPostProcess& cameraPost = *entity->cameraPostProcess;
                    renderChanged |= inspectorCheckboxRow("Bloom", &cameraPost.bloomEnabled);
                    renderChanged |= inspectorDragFloatRow("Bloom Intensity", &cameraPost.bloomIntensity, 0.02f, 0.0f, 100.0f, "%.2f");
                    renderChanged |= inspectorCheckboxRow("Color correction", &cameraPost.colorCorrectionEnabled);
                    renderChanged |= inspectorCheckboxRow("Vignetting", &cameraPost.vignettingEnabled);
                    renderChanged |= inspectorCheckboxRow("Film grain", &cameraPost.filmGrainEnabled);
                } else {
                    ImGui::TextDisabled("Add Camera Post Process for bloom, color correction, vignetting, and film grain overrides.");
                }
                ImGui::TreePop();
            }
            if (inspectorCheckboxRow("Active Camera", &camera.active)) {
                if (camera.active) {
                    document.setActiveCamera(entity->id);
                } else if (document.activeCamera() == entity->id) {
                    document.setActiveCamera({});
                }
                changed = true;
            }
            if (changed) {
                document.markDirty(SceneUpdateKind::CameraOnly);
                requests.sceneUpdate = SceneUpdateKind::CameraOnly;
                requests.setCamera = EditorCameraChange{
                    .entity = entity->id,
                    .oldCamera = oldCamera,
                    .newCamera = camera,
                    .oldActiveCamera = oldActiveCamera,
                    .newActiveCamera = document.activeCamera(),
                };
            }
            if (renderChanged) {
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = beforeRenderSettings, .updateKind = SceneUpdateKind::RendererSettingsOnly, .label = "Edit Camera Settings"};
            }
        }

        if (entity->light.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::Light, "Light", "Area, spot, point, and directional authoring", !entityLocked, "##LightActions", entity->id, EditorComponentKind::Light, SceneUpdateKind::TopologyChanged, &requests);
            Light& light = *entity->light;
            const Light oldLight = light;
            bool changed = false;
            int lightType = static_cast<int>(light.type);
            if (inspectorComboRow("Type", &lightType, "Directional\0Point\0Area\0Spot\0")) {
                light.type = static_cast<LightType>(lightType);
                if (light.type == LightType::Directional &&
                    (oldLight.type != LightType::Directional || light.sizeOrRadius > 0.08f || light.sizeOrRadius <= 0.0f)) {
                    light.sizeOrRadius = 0.00465f;
                }
                changed = true;
            }
            if (light.type == LightType::Area) {
                inspectorReadonlyRow("Shape", "Area Disk");
                inspectorReadonlyRow("Units", "LuminousPower (lumen)");
            } else if (light.type == LightType::Directional) {
                inspectorReadonlyRow("Units", "Illuminance (lux)");
            } else {
                inspectorReadonlyRow("Units", "Intensity");
            }
            changed |= inspectorCheckboxRow("Enabled", &light.enabled);
            if (resetFieldButton("LightEnabled")) {
                light.enabled = Light{}.enabled;
                changed = true;
            }
            changed |= inspectorCheckboxRow("Color Temperature", &light.useColorTemperature);
            ImGui::BeginDisabled(!light.useColorTemperature);
            changed |= inspectorDragFloatRow("Temperature", &light.colorTemperatureKelvin, 10.0f, 1000.0f, 40000.0f, "%.0f K");
            ImGui::EndDisabled();
            changed |= inspectorColorEdit3Row("Color", glm::value_ptr(light.color));
            if (resetFieldButton("LightColor")) {
                light.color = Light{}.color;
                changed = true;
            }
            changed |= inspectorDragFloatRow("Intensity", &light.intensity, 0.05f, 0.0f, 100000.0f, "%.2f");
            if (resetFieldButton("LightIntensity")) {
                light.intensity = Light{}.intensity;
                changed = true;
            }
            changed |= inspectorDragFloatRow("Exposure Multiplier", &light.exposureMultiplier, 0.01f, 0.0f, 100.0f, "%.3f");
            inspectorReadonlyRow("IES Profile", light.iesProfile.empty() ? "None" : light.iesProfile.c_str());
            int materialSource = light.materialSource == "Light Material" ? 1 : light.materialSource == "Emissive Mesh" ? 2 : 0;
            if (inspectorComboRow("Material Source", &materialSource, "None\0Light Material\0Emissive Mesh\0")) {
                light.materialSource = materialSource == 1 ? "Light Material" : materialSource == 2 ? "Emissive Mesh" : "None";
                changed = true;
            }
            if (light.type == LightType::Directional) {
                changed |= inspectorDragFloatRow("Angular Radius", &light.sizeOrRadius, 0.0001f, 0.0001f, 0.08f, "%.5f rad");
                if (resetFieldButton("LightAngularRadius")) {
                    light.sizeOrRadius = 0.00465f;
                    changed = true;
                }
            } else if (light.type == LightType::Spot) {
                changed |= inspectorDragFloatRow("Range", &light.sizeOrRadius, 0.02f, 0.0f, 1000.0f, "%.3f");
                if (resetFieldButton("LightSpotRange")) {
                    light.sizeOrRadius = Light{}.sizeOrRadius;
                    changed = true;
                }
                changed |= inspectorDragFloatRow("Inner Cone", &light.innerConeRadians, 0.01f, 0.0f, 3.14159f, "%.2f rad");
                if (resetFieldButton("LightInnerCone")) {
                    light.innerConeRadians = Light{}.innerConeRadians;
                    changed = true;
                }
                changed |= inspectorDragFloatRow("Outer Cone", &light.outerConeRadians, 0.01f, light.innerConeRadians, 3.14159f, "%.2f rad");
                if (resetFieldButton("LightOuterCone")) {
                    light.outerConeRadians = Light{}.outerConeRadians;
                    changed = true;
                }
            } else {
                changed |= inspectorDragFloatRow("Size / Radius", &light.sizeOrRadius, 0.02f, 0.0f, 1000.0f, "%.3f");
                if (resetFieldButton("LightSizeRadius")) {
                    light.sizeOrRadius = Light{}.sizeOrRadius;
                    changed = true;
                }
            }
            changed |= inspectorCheckboxRow("Visible To Camera", &light.visibleToCamera);
            changed |= inspectorCheckboxRow("Cast Surface Shadows", &light.castSurfaceShadows);
            changed |= inspectorCheckboxRow("Cast Volumetric Shadows", &light.castVolumetricShadows);
            if (changed) {
                document.markDirty(SceneUpdateKind::LightOnly);
                requests.sceneUpdate = SceneUpdateKind::LightOnly;
                requests.setLight = EditorLightChange{
                    .entity = entity->id,
                    .oldLight = oldLight,
                    .newLight = light,
                };
            }
        }

        if (entity->sun.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::Sun, "Primary Sun", "Directional daylight and shadow controls", !entityLocked, "##SunActions", entity->id, EditorComponentKind::Sun, SceneUpdateKind::LightOnly, &requests);
            Sun& sun = *entity->sun;
            const Sun oldSun = sun;
            const Transform oldSunTransform = entity->transform;
            bool changed = false;
            bool sunTransformChanged = false;
            float elevation = 0.0f;
            float azimuth = 0.0f;
            SunController::anglesFromWorldTransform(document.registry(), *entity, elevation, azimuth);
            inspectorReadonlyRow("Light Type", "Sun");
            inspectorReadonlyRow("Units", "Illuminance (lux)");
            changed |= inspectorCheckboxRow("Enabled", &sun.enabled);
            if (resetFieldButton("SunEnabled")) {
                sun.enabled = Sun{}.enabled;
                changed = true;
            }
            if (inspectorDragFloatRow("Azimuth", &azimuth, 0.01f, -6.28318f, 6.28318f, "%.3f rad")) {
                sunTransformChanged = true;
            }
            if (resetFieldButton("SunAzimuth", "Reset direction angle")) {
                azimuth = 0.0f;
                sunTransformChanged = true;
            }
            if (inspectorDragFloatRow("Elevation", &elevation, 0.01f, -1.5707f, 1.5707f, "%.3f rad")) {
                sunTransformChanged = true;
            }
            if (resetFieldButton("SunElevation", "Reset direction angle")) {
                elevation = 0.0f;
                sunTransformChanged = true;
            }
            changed |= inspectorCheckboxRow("Use Color Temperature", &sun.useColorTemperature);
            changed |= inspectorDragFloatRow("Illuminance", &sun.illuminanceLux, 100.0f, 0.0f, 200000.0f, "%.0f lux");
            if (resetFieldButton("SunIlluminance")) {
                sun.illuminanceLux = Sun{}.illuminanceLux;
                changed = true;
            }
            changed |= inspectorDragFloatRow("Exposure Multiplier", &sun.exposureMultiplier, 0.01f, 0.0f, 100.0f, "%.3f");
            changed |= inspectorDragFloatRow("Softness", &sun.angularRadiusRadians, 0.0001f, 0.0001f, 0.08f, "%.5f rad");
            if (resetFieldButton("SunSoftness")) {
                sun.angularRadiusRadians = Sun{}.angularRadiusRadians;
                changed = true;
            }
            changed |= inspectorDragFloatRow("Color Temperature", &sun.colorTemperatureKelvin, 10.0f, 1000.0f, 40000.0f, "%.0f K");
            if (resetFieldButton("SunColorTemperature")) {
                sun.colorTemperatureKelvin = Sun{}.colorTemperatureKelvin;
                changed = true;
            }
            changed |= inspectorCheckboxRow("Cast Surface Shadows", &sun.castSurfaceShadows);
            changed |= inspectorCheckboxRow("Cast Volumetric Shadows", &sun.castVolumetricShadows);
            int shadowBounces = static_cast<int>(sun.shadowBounces);
            if (inspectorDragIntRow("Shadow Bounces", &shadowBounces, 1.0f, 0, 16)) {
                sun.shadowBounces = static_cast<uint32_t>(std::clamp(shadowBounces, 0, 16));
                changed = true;
            }
            int volumetricShadowBounces = static_cast<int>(sun.volumetricShadowBounces);
            if (inspectorDragIntRow("Volumetric Shadow Bounces", &volumetricShadowBounces, 1.0f, 0, 16)) {
                sun.volumetricShadowBounces = static_cast<uint32_t>(std::clamp(volumetricShadowBounces, 0, 16));
                changed = true;
            }
            if (sunTransformChanged) {
                entity->transform = SunController::transformFromWorldAngles(document.registry(), *entity, entity->transform, elevation, azimuth);
                entity->transform.dirty = true;
                requests.setEntityTransform = EditorEntityTransformChange{
                    .entity = entity->id,
                    .oldTransform = oldSunTransform,
                    .newTransform = entity->transform,
                };
            }
            if (changed) {
                document.setPrimarySun(entity->id);
                document.markDirty(SceneUpdateKind::LightOnly);
                requests.sceneUpdate = SceneUpdateKind::LightOnly;
                requests.setSun = EditorSunChange{
                    .entity = entity->id,
                    .oldSun = oldSun,
                    .newSun = sun,
                };
            }
            if (sunTransformChanged && !changed) {
                document.setPrimarySun(entity->id);
                document.markDirty(SceneUpdateKind::LightOnly);
                requests.sceneUpdate = SceneUpdateKind::LightOnly;
            }
        }

        if (entity->meshRenderer.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::Model, "Mesh Renderer", "Visibility, material slots, and render flags", !entityLocked, "##MeshRendererActions", entity->id, EditorComponentKind::MeshRenderer, SceneUpdateKind::TopologyChanged, &requests);
            MeshRenderer& renderer = *entity->meshRenderer;
            ensureMaterialSlots(renderer, state.assets);
            const MeshRenderer oldRenderer = renderer;
            bool changed = false;
            changed |= inspectorCheckboxRow("Visible", &renderer.visible);
            changed |= inspectorCheckboxRow("Cast Shadow", &renderer.castShadow);
            changed |= inspectorCheckboxRow("Visible To Camera", &renderer.visibleToCamera);
            inspectorReadonlyRow("Mesh Asset", std::to_string(renderer.mesh.index));
            inspectorReadonlyRow("Renderer Instance Cache", std::to_string(renderer.rendererInstanceIndex));
            if (state.assets != nullptr) {
                const MeshAsset* mesh = state.assets->mesh(renderer.mesh);
                if (mesh != nullptr) {
                    inspectorReadonlyRow("Mesh", mesh->name.empty() ? "(unnamed)" : mesh->name.c_str());
                    inspectorReadonlyRow("Geometry", std::to_string(mesh->vertices.size()) + " vertices, " + std::to_string(mesh->primitives.size()) + " primitives");
                }
            }
            if (ImGui::TreeNodeEx("Material Slots", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < renderer.materialSlots.size(); ++i) {
                    MaterialSlot& slot = renderer.materialSlots[i];
                    MaterialAssetHandle material = slot.resolvedMaterial();
                    ImGui::PushID(static_cast<int>(i));
                    std::string preview = std::to_string(material.index);
                    if (state.assets != nullptr) {
                        if (const MaterialAsset* source = state.assets->material(material)) {
                            preview = source->name.empty() ? preview : source->name;
                        }
                    }
                    if (ImGui::BeginCombo(slot.name.empty() ? "Material" : slot.name.c_str(), preview.c_str())) {
                        if (state.assets != nullptr) {
                            const auto& materials = state.assets->materials();
                            for (uint32_t materialIndex = 0; materialIndex < materials.size(); ++materialIndex) {
                                const MaterialAssetHandle candidate{materialIndex};
                                const std::string label = materials[materialIndex].name.empty()
                                    ? "Material " + std::to_string(materialIndex)
                                    : materials[materialIndex].name + "##" + std::to_string(materialIndex);
                                const bool selected = material.index == materialIndex;
                                if (ImGui::Selectable(label.c_str(), selected)) {
                                    requests.materialAssignment = EditorMaterialAssignment{
                                        .entity = entity->id,
                                        .mesh = renderer.mesh,
                                        .primitiveIndex = static_cast<uint32_t>(i),
                                        .material = candidate,
                                    };
                                    document.markDirty(SceneUpdateKind::TopologyChanged);
                                    requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                                }
                                if (selected) {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
                if (renderer.materialSlots.empty()) {
                    ImGui::TextDisabled("No material slot overrides");
                }
                ImGui::TreePop();
            }
            if (!renderer.materialSlots.empty() && state.assets != nullptr) {
                const MaterialAssetHandle materialHandle = renderer.materialSlots.front().resolvedMaterial();
                if (const MaterialAsset* source = state.assets->material(materialHandle)) {
                    MaterialAsset edited = *source;
                    bool materialChanged = false;
                    drawInspectorComponentHeader(EditorGlyphIcon::Material, "Material", "First resolved material slot", !entityLocked);
                    inspectorReadonlyRow("Material", edited.name.empty() ? "(unnamed)" : edited.name.c_str());
                    materialChanged |= inspectorColorEdit4Row("Base Color", glm::value_ptr(edited.baseColorFactor));
                    materialChanged |= inspectorSliderFloatRow("Metallic", &edited.metallicFactor, 0.0f, 1.0f, "%.3f");
                    materialChanged |= inspectorSliderFloatRow("Roughness", &edited.roughnessFactor, 0.0f, 1.0f, "%.3f");
                    materialChanged |= inspectorColorEdit3Row("Emissive", glm::value_ptr(edited.emissiveFactor));
                    int alphaMode = static_cast<int>(edited.alphaMode);
                    if (inspectorComboRow("Alpha Mode", &alphaMode, "Opaque\0Mask\0Blend\0")) {
                        edited.alphaMode = static_cast<uint32_t>(alphaMode);
                        materialChanged = true;
                    }
                    bool doubleSided = edited.doubleSided != 0;
                    if (inspectorCheckboxRow("Double Sided", &doubleSided)) {
                        edited.doubleSided = doubleSided ? 1u : 0u;
                        materialChanged = true;
                    }
                    materialChanged |= inspectorSliderFloatRow("Alpha Cutoff", &edited.alphaCutoff, 0.0f, 1.0f, "%.3f");
                    if (materialChanged) {
                        requests.materialUpdate = EditorMaterialUpdate{.materialId = materialHandle.index, .material = edited};
                        requests.resetAccumulation = AccumulationResetReason::MaterialChanged;
                        document.markDirty(SceneUpdateKind::MaterialOnly);
                        requests.sceneUpdate = SceneUpdateKind::MaterialOnly;
                    }
                }
            }
            if (changed) {
                document.markDirty(SceneUpdateKind::VisibilityOnly);
                requests.sceneUpdate = SceneUpdateKind::VisibilityOnly;
                requests.setMeshRenderer = EditorMeshRendererChange{
                    .entity = entity->id,
                    .oldRenderer = oldRenderer,
                    .newRenderer = renderer,
                    .updateKind = SceneUpdateKind::VisibilityOnly,
                };
            }
        }

        if (entity->environmentLight.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::Environment, "Environment Light", "IBL intensity, background, and rotation", !entityLocked, "##EnvironmentLightActions", entity->id, EditorComponentKind::EnvironmentLight, SceneUpdateKind::EnvironmentOnly, &requests);
            const SceneDocument before = document;
            EnvironmentLight& component = *entity->environmentLight;
            bool changed = false;
            changed |= inspectorCheckboxRow("Environment Enabled", &component.enabled);
            changed |= inspectorDragFloatRow("Environment Intensity", &component.intensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            changed |= inspectorDragFloatRow("Background Intensity", &component.backgroundIntensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            changed |= inspectorDragFloatRow("Environment Rotation", &component.rotation, 0.01f, -6.28318f, 6.28318f, "%.3f");
            if (changed) {
                document.environment().enabled = component.enabled;
                document.environment().intensity = component.intensity;
                document.environment().backgroundIntensity = component.backgroundIntensity;
                document.environment().rotation = component.rotation;
                document.worldSettings().activeEnvironment = entity->id;
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::EnvironmentOnly, .label = "Edit Environment Light"};
            }
        }

        if (entity->skyAtmosphere.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::Sky, "Sky Atmosphere", "Rayleigh, Mie, and ground albedo controls", !entityLocked, "##SkyAtmosphereActions", entity->id, EditorComponentKind::SkyAtmosphere, SceneUpdateKind::EnvironmentOnly, &requests);
            const SceneDocument before = document;
            SkyAtmosphere& component = *entity->skyAtmosphere;
            bool changed = false;
            changed |= inspectorCheckboxRow("Atmosphere Enabled", &component.enabled);
            changed |= inspectorDragFloatRow("Rayleigh Scale Height", &component.rayleighScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
            changed |= inspectorDragFloatRow("Mie Scale Height", &component.mieScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
            changed |= inspectorSliderFloatRow("Mie Anisotropy", &component.mieAnisotropy, 0.0f, 0.99f, "%.3f");
            changed |= inspectorSliderFloatRow("Ground Albedo", &component.groundAlbedo, 0.0f, 1.0f, "%.3f");
            if (changed) {
                RenderSettings& render = document.renderSettings();
                render.rayleighScaleHeight = component.rayleighScaleHeight;
                render.mieScaleHeight = component.mieScaleHeight;
                render.mieAnisotropy = component.mieAnisotropy;
                render.groundAlbedo = component.groundAlbedo;
                document.worldSettings().skyAtmosphere = entity->id;
                document.worldSettings().atmosphereEnabled = component.enabled;
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::RendererSettingsOnly, .label = "Edit Sky Atmosphere"};
            }
        }

        if (entity->heightFog.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::Fog, "Height Fog", "Density, falloff, and fog color", !entityLocked, "##HeightFogActions", entity->id, EditorComponentKind::HeightFog, SceneUpdateKind::EnvironmentOnly, &requests);
            const SceneDocument before = document;
            HeightFog& component = *entity->heightFog;
            bool changed = false;
            changed |= inspectorCheckboxRow("Fog Enabled", &component.enabled);
            changed |= inspectorDragFloatRow("Density", &component.density, 0.001f, 0.0f, 10.0f, "%.4f");
            changed |= inspectorDragFloatRow("Height Falloff", &component.heightFalloff, 0.01f, 0.0f, 10.0f, "%.3f");
            changed |= inspectorColorEdit3Row("Fog Color", glm::value_ptr(component.color));
            if (changed) {
                document.worldSettings().heightFog = entity->id;
                document.worldSettings().fogEnabled = component.enabled;
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::EnvironmentOnly, .label = "Edit Height Fog"};
            }
        }

        if (entity->volumetricCloud.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::Cloud, "Volumetric Cloud", "Coverage and cloud density", !entityLocked, "##VolumetricCloudActions", entity->id, EditorComponentKind::VolumetricCloud, SceneUpdateKind::EnvironmentOnly, &requests);
            const SceneDocument before = document;
            VolumetricCloud& component = *entity->volumetricCloud;
            bool changed = false;
            changed |= inspectorCheckboxRow("Cloud Enabled", &component.enabled);
            changed |= inspectorSliderFloatRow("Cloud Density", &component.density, 0.0f, 1.0f, "%.3f");
            changed |= inspectorSliderFloatRow("Coverage", &component.coverage, 0.0f, 1.0f, "%.3f");
            if (changed) {
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::EnvironmentOnly, .label = "Edit Volumetric Cloud"};
            }
        }

        if (entity->postProcessVolume.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::PostProcess, "Post Process Volume", "Unbound volume exposure and color grading", !entityLocked, "##PostProcessVolumeActions", entity->id, EditorComponentKind::PostProcessVolume, SceneUpdateKind::RendererSettingsOnly, &requests);
            const SceneDocument before = document;
            PostProcessVolume& component = *entity->postProcessVolume;
            bool changed = false;
            changed |= inspectorCheckboxRow("Post Process Enabled", &component.enabled);
            changed |= inspectorCheckboxRow("Unbound", &component.unbound);
            changed |= inspectorDragFloatRow("Priority", &component.priority, 0.1f, -100.0f, 100.0f, "%.1f");
            changed |= inspectorDragFloatRow("Exposure Compensation", &component.exposureCompensation, 0.02f, -10.0f, 10.0f, "%.2f");
            changed |= inspectorSliderFloatRow("PP Saturation", &component.saturation, 0.0f, 2.0f, "%.3f");
            changed |= inspectorSliderFloatRow("PP Contrast", &component.contrast, 0.0f, 2.0f, "%.3f");
            if (changed) {
                RenderSettings& render = document.renderSettings();
                render.physicalExposureCompensation = component.exposureCompensation;
                render.saturation = component.saturation;
                render.contrast = component.contrast;
                document.worldSettings().postProcessVolume = entity->id;
                document.worldSettings().postProcessEnabled = component.enabled;
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::RendererSettingsOnly, .label = "Edit Post Process Volume"};
            }
        }

        if (entity->cameraPostProcess.has_value()) {
            drawInspectorComponentHeader(EditorGlyphIcon::PostProcess, "Camera Post Process", "Per-camera exposure, DOF, bloom, and grain", !entityLocked, "##CameraPostProcessActions", entity->id, EditorComponentKind::CameraPostProcess, SceneUpdateKind::RendererSettingsOnly, &requests);
            const SceneDocument before = document;
            CameraPostProcess& component = *entity->cameraPostProcess;
            bool changed = false;
            changed |= inspectorCheckboxRow("Camera PP Enabled", &component.enabled);
            changed |= inspectorCheckboxRow("Override Exposure", &component.overrideExposure);
            changed |= inspectorDragFloatRow("Exposure Compensation", &component.exposureCompensation, 0.02f, -10.0f, 10.0f, "%.2f");
            changed |= inspectorCheckboxRow("Override DOF", &component.overrideDepthOfField);
            changed |= inspectorDragFloatRow("DOF Aperture", &component.dofApertureRadius, 0.01f, 0.0f, 10.0f, "%.3f");
            changed |= inspectorDragFloatRow("Focus Distance", &component.dofFocusDistance, 0.05f, 0.01f, 10000.0f, "%.2f");
            if (ImGui::TreeNodeEx("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= inspectorCheckboxRow("Enabled", &component.bloomEnabled);
                changed |= inspectorDragFloatRow("Intensity", &component.bloomIntensity, 0.02f, 0.0f, 100.0f, "%.2f");
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Color correction", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= inspectorCheckboxRow("Enabled", &component.colorCorrectionEnabled);
                changed |= inspectorDragFloatRow("Saturation", &component.colorCorrectionSaturation, 0.01f, 0.0f, 2.0f, "%.3f");
                changed |= inspectorDragFloatRow("Contrast", &component.colorCorrectionContrast, 0.01f, 0.0f, 2.0f, "%.3f");
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Vignetting", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= inspectorCheckboxRow("Enabled", &component.vignettingEnabled);
                changed |= inspectorDragFloatRow("Intensity", &component.vignettingIntensity, 0.01f, 0.0f, 1.0f, "%.3f");
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Film grain", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= inspectorCheckboxRow("Enabled", &component.filmGrainEnabled);
                changed |= inspectorDragFloatRow("Intensity", &component.filmGrainIntensity, 0.01f, 0.0f, 1.0f, "%.3f");
                ImGui::TreePop();
            }
            if (changed) {
                RenderSettings& render = document.renderSettings();
                if (component.overrideExposure) {
                    render.physicalExposureCompensation = component.exposureCompensation;
                }
                if (component.overrideDepthOfField) {
                    render.dofApertureRadius = component.dofApertureRadius;
                    render.dofFocusDistance = component.dofFocusDistance;
                }
                if (component.colorCorrectionEnabled) {
                    render.saturation = component.colorCorrectionSaturation;
                    render.contrast = component.colorCorrectionContrast;
                }
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::RendererSettingsOnly, .label = "Edit Camera Post Process"};
            }
        }

        drawInspectorComponentHeader(EditorGlyphIcon::Add, "Add Component", "Attach available authoring components", !entityLocked);
        float addComponentRowUsedWidth = 0.0f;
        const float addComponentRowWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        if (!entity->light.has_value()) {
            drawInspectorAddComponentButton("InspectorAddLight", EditorGlyphIcon::Light, "Light", *entity, EditorComponentKind::Light, SceneUpdateKind::TopologyChanged, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (!entity->sun.has_value()) {
            drawInspectorAddComponentButton("InspectorAddSun", EditorGlyphIcon::Sun, "Primary Sun", *entity, EditorComponentKind::Sun, SceneUpdateKind::LightOnly, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (!entity->camera.has_value()) {
            drawInspectorAddComponentButton("InspectorAddCamera", EditorGlyphIcon::Camera, "Camera", *entity, EditorComponentKind::Camera, SceneUpdateKind::TopologyChanged, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (entity->camera.has_value() && !entity->cameraPostProcess.has_value()) {
            drawInspectorAddComponentButton("InspectorAddCameraPostProcess", EditorGlyphIcon::PostProcess, "Camera Post Process", *entity, EditorComponentKind::CameraPostProcess, SceneUpdateKind::RendererSettingsOnly, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (!entity->meshRenderer.has_value()) {
            drawInspectorAddComponentButton("InspectorAddMeshRenderer", EditorGlyphIcon::Model, "Mesh Renderer", *entity, EditorComponentKind::MeshRenderer, SceneUpdateKind::TopologyChanged, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (!entity->environmentLight.has_value()) {
            drawInspectorAddComponentButton("InspectorAddEnvironmentLight", EditorGlyphIcon::Environment, "Environment Light", *entity, EditorComponentKind::EnvironmentLight, SceneUpdateKind::EnvironmentOnly, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (!entity->skyAtmosphere.has_value()) {
            drawInspectorAddComponentButton("InspectorAddSkyAtmosphere", EditorGlyphIcon::Sky, "Sky Atmosphere", *entity, EditorComponentKind::SkyAtmosphere, SceneUpdateKind::EnvironmentOnly, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (!entity->heightFog.has_value()) {
            drawInspectorAddComponentButton("InspectorAddHeightFog", EditorGlyphIcon::Fog, "Height Fog", *entity, EditorComponentKind::HeightFog, SceneUpdateKind::EnvironmentOnly, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (!entity->volumetricCloud.has_value()) {
            drawInspectorAddComponentButton("InspectorAddVolumetricCloud", EditorGlyphIcon::Cloud, "Volumetric Cloud", *entity, EditorComponentKind::VolumetricCloud, SceneUpdateKind::EnvironmentOnly, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (!entity->postProcessVolume.has_value()) {
            drawInspectorAddComponentButton("InspectorAddPostProcessVolume", EditorGlyphIcon::PostProcess, "Post Process Volume", *entity, EditorComponentKind::PostProcessVolume, SceneUpdateKind::RendererSettingsOnly, requests, addComponentRowWidth, addComponentRowUsedWidth);
        }
        if (addComponentRowUsedWidth <= 0.0f) {
            ImGui::TextDisabled("All components attached");
        }

        ImGui::Separator();
        if (editorIconTextButton("InspectorDuplicateEntity", EditorGlyphIcon::Add, "Duplicate Entity")) {
            requests.duplicateEntity = entity->id;
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        ImGui::SameLine();
        if (editorIconTextButton("InspectorDeleteEntity", EditorGlyphIcon::Trash, "Delete Entity")) {
            ImGui::OpenPopup("Delete##confirm");
        }
        if (ImGui::BeginPopupModal("Delete##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete '%s'?", entity->name.empty() ? "Entity" : entity->name.c_str());
            if (ImGui::Button("Yes")) {
                const EntityId deleted = entity->id;
                requests.deleteEntity = deleted;
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                selection.clear();
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                ImGui::EndDisabled();
                ImGui::End();
                return;
            }
            ImGui::SameLine();
            if (ImGui::Button("No")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndDisabled();

        if (editorIconTextButton("InspectorClearEntitySelection", EditorGlyphIcon::Exit, "Clear Selection")) {
            selection.clear();
        }

        ImGui::End();
        return;
    }

    if (current.kind == EditorSelectionKind::Camera) {
        ImGui::TextUnformatted("Camera");
        if (state.camera != nullptr) {
            glm::vec3 position = state.camera->position();
            glm::vec3 direction = state.camera->direction();
            float speed = state.camera->moveSpeed();
            ImGui::InputFloat3("Position", glm::value_ptr(position), "%.3f", ImGuiInputTextFlags_ReadOnly);
            ImGui::InputFloat3("Orientation", glm::value_ptr(direction), "%.3f", ImGuiInputTextFlags_ReadOnly);
            const std::array<std::pair<const char*, float>, 4> presets = {{
                {"Slow", 1.0f},
                {"Medium", 2.4f},
                {"Fast", 6.0f},
                {"Very Fast", 15.0f},
            }};
            ImGui::TextUnformatted("Move Speed");
            for (const auto& [label, value] : presets) {
                ImGui::SameLine();
                if (ImGui::RadioButton(label, std::abs(speed - value) < 0.01f)) {
                    requests.cameraMoveSpeed = value;
                    speed = value;
                }
            }
            if (ImGui::SliderFloat("Move Speed", &speed, 0.25f, 20.0f, "%.2f")) {
                requests.cameraMoveSpeed = speed;
            }
        }
        if (editorIconTextButton("InspectorResetCameraSelection", EditorGlyphIcon::Reset, "Reset Camera")) {
            requests.resetCamera = true;
        }
    } else if (current.kind == EditorSelectionKind::Object) {
        if (state.importedScene != nullptr && current.index < state.importedScene->nodes.size()) {
            const SceneNodeAsset& node = state.importedScene->nodes[current.index];
            ImGui::Text("Name: %s", node.name.empty() ? "(unnamed)" : node.name.c_str());
            drawMatrixTranslation(node.transform);
            if (node.mesh.valid()) {
                ImGui::Text("Mesh: %u", node.mesh.index);
                if (state.assets != nullptr) {
                    const MeshAsset* mesh = state.assets->mesh(node.mesh);
                    if (mesh != nullptr && !mesh->primitives.empty()) {
                        ImGui::Text("Material: %u", mesh->primitives.front().material.index);
                    }
                }
            } else {
                ImGui::TextDisabled("No mesh");
            }
            bool visible = true;
            ImGui::Checkbox("Visible", &visible);
        } else {
            ImGui::Text("Fallback Object: %u", current.index);
            ImGui::TextDisabled("Fallback transforms are generated in the GPU scene build path.");
        }
    } else if (current.kind == EditorSelectionKind::Material) {
        ImGui::Text("Material: %u", current.index);
    } else if (current.kind == EditorSelectionKind::Asset) {
        ImGui::Text("Asset: %u", current.index);
    } else if (current.kind == EditorSelectionKind::Light) {
        ImGui::Text("Light: %u", current.index);
        ImGui::TextDisabled("Select an entity light in the scene hierarchy to edit authored light data.");
    }

    if (editorIconTextButton("InspectorClearSelectionFallback", EditorGlyphIcon::Exit, "Clear Selection")) {
        selection.clear();
    }

    ImGui::End();
}

} // namespace rtv
