#include "rtv/InspectorPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace rtv {

namespace {

void drawMatrixTranslation(const glm::mat4& transform) {
    glm::vec3 translation{transform[3]};
    ImGui::InputFloat3("Position", glm::value_ptr(translation), "%.3f", ImGuiInputTextFlags_ReadOnly);
}

} // namespace

void InspectorPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    if (!ImGui::Begin("Inspector / Properties")) {
        ImGui::End();
        return;
    }

    const EditorSelectionId current = selection.current();
    if (!current.valid()) {
        ImGui::TextDisabled("No selection");
        ImGui::End();
        return;
    }

    if (state.sceneDocument != nullptr && current.entity.valid()) {
        SceneDocument& document = *state.sceneDocument;
        Entity* entity = document.registry().entity(current.entity);
        if (entity == nullptr) {
            selection.clear();
            ImGui::TextDisabled("Selection no longer exists");
            ImGui::End();
            return;
        }

        static EntityId nameEditId{};
        static std::array<char, 128> nameBuffer{};
        if (nameEditId != entity->id) {
            nameEditId = entity->id;
            nameBuffer.fill('\0');
            const std::string name = entity->name;
            name.copy(nameBuffer.data(), std::min(name.size(), nameBuffer.size() - 1u));
        }
        if (ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            document.registry().renameEntity(entity->id, nameBuffer.data());
            document.markDirty(SceneUpdateKind::None);
        }

        ImGui::SeparatorText("Transform");
        bool transformChanged = false;
        transformChanged |= ImGui::DragFloat3("Position", glm::value_ptr(entity->transform.position), 0.02f, -10000.0f, 10000.0f, "%.3f");
        glm::vec3 rotationDegrees = glm::degrees(entity->transform.rotationEuler);
        if (ImGui::DragFloat3("Rotation", glm::value_ptr(rotationDegrees), 0.2f, -360.0f, 360.0f, "%.2f")) {
            entity->transform.rotationEuler = glm::radians(rotationDegrees);
            transformChanged = true;
        }
        transformChanged |= ImGui::DragFloat3("Scale", glm::value_ptr(entity->transform.scale), 0.02f, 0.001f, 1000.0f, "%.3f");
        if (transformChanged) {
            entity->transform.dirty = true;
            document.markDirty(SceneUpdateKind::TransformOnly);
            requests.sceneUpdate = SceneUpdateKind::TransformOnly;
        }

        if (entity->camera.has_value()) {
            ImGui::SeparatorText("Camera");
            Camera& camera = *entity->camera;
            bool changed = false;
            float fovDegrees = glm::degrees(camera.verticalFovRadians);
            if (ImGui::SliderFloat("Vertical FOV", &fovDegrees, 20.0f, 120.0f, "%.1f")) {
                camera.verticalFovRadians = glm::radians(fovDegrees);
                changed = true;
            }
            changed |= ImGui::DragFloat("Near Plane", &camera.nearPlane, 0.005f, 0.001f, 100.0f, "%.3f");
            changed |= ImGui::DragFloat("Far Plane", &camera.farPlane, 1.0f, camera.nearPlane + 0.01f, 100000.0f, "%.1f");
            if (ImGui::Checkbox("Active Camera", &camera.active)) {
                if (camera.active) {
                    document.setActiveCamera(entity->id);
                }
                changed = true;
            }
            if (changed) {
                document.markDirty(SceneUpdateKind::CameraOnly);
                requests.sceneUpdate = SceneUpdateKind::CameraOnly;
            }
        }

        if (entity->light.has_value()) {
            ImGui::SeparatorText("Light");
            Light& light = *entity->light;
            bool changed = false;
            int lightType = static_cast<int>(light.type);
            if (ImGui::Combo("Type", &lightType, "Directional\0Point\0Area\0")) {
                light.type = static_cast<LightType>(lightType);
                changed = true;
            }
            changed |= ImGui::Checkbox("Enabled", &light.enabled);
            changed |= ImGui::ColorEdit3("Color", glm::value_ptr(light.color));
            changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100000.0f, "%.2f");
            changed |= ImGui::DragFloat("Size / Radius", &light.sizeOrRadius, 0.02f, 0.0f, 1000.0f, "%.3f");
            if (changed) {
                document.markDirty(SceneUpdateKind::LightOnly);
                requests.sceneUpdate = SceneUpdateKind::LightOnly;
            }
        }

        if (entity->meshRenderer.has_value()) {
            ImGui::SeparatorText("Mesh Renderer");
            MeshRenderer& renderer = *entity->meshRenderer;
            bool changed = false;
            changed |= ImGui::Checkbox("Visible", &renderer.visible);
            changed |= ImGui::Checkbox("Cast Shadow", &renderer.castShadow);
            changed |= ImGui::Checkbox("Visible To Camera", &renderer.visibleToCamera);
            ImGui::Text("Mesh Asset: %u", renderer.mesh.index);
            ImGui::Text("Renderer Instance Cache: %u", renderer.rendererInstanceIndex);
            if (state.assets != nullptr) {
                const MeshAsset* mesh = state.assets->mesh(renderer.mesh);
                if (mesh != nullptr) {
                    ImGui::Text("Mesh: %s", mesh->name.empty() ? "(unnamed)" : mesh->name.c_str());
                    ImGui::Text("%zu vertices, %zu primitives", mesh->vertices.size(), mesh->primitives.size());
                }
            }
            if (ImGui::TreeNodeEx("Material Slots", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < renderer.materialSlots.size(); ++i) {
                    MaterialSlot& slot = renderer.materialSlots[i];
                    MaterialAssetHandle material = slot.resolvedMaterial();
                    ImGui::Text("%zu: %s  material=%u", i, slot.name.empty() ? "slot" : slot.name.c_str(), material.index);
                }
                if (renderer.materialSlots.empty()) {
                    ImGui::TextDisabled("No material slot overrides");
                }
                ImGui::TreePop();
            }
            if (changed) {
                document.markDirty(SceneUpdateKind::FullSceneRebuild);
                requests.sceneUpdate = SceneUpdateKind::FullSceneRebuild;
            }
        }

        if (ImGui::Button("Clear Selection")) {
            selection.clear();
        }

        ImGui::End();
        return;
    }

    if (current.kind == EditorSelectionKind::Camera) {
        ImGui::TextUnformatted("Camera");
        float fov = 60.0f;
        ImGui::SliderFloat("FOV", &fov, 20.0f, 100.0f, "%.1f", ImGuiSliderFlags_NoInput);
        if (state.camera != nullptr) {
            glm::vec3 position = state.camera->position();
            glm::vec3 direction = state.camera->direction();
            float speed = state.camera->moveSpeed();
            ImGui::InputFloat3("Position", glm::value_ptr(position), "%.3f", ImGuiInputTextFlags_ReadOnly);
            ImGui::InputFloat3("Orientation", glm::value_ptr(direction), "%.3f", ImGuiInputTextFlags_ReadOnly);
            if (ImGui::SliderFloat("Move Speed", &speed, 0.25f, 20.0f, "%.2f")) {
                requests.cameraMoveSpeed = speed;
            }
        }
        if (ImGui::Button("Reset Camera")) {
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
        bool enabled = true;
        float intensity = 1.0f;
        float color[3] = {1.0f, 1.0f, 1.0f};
        ImGui::Checkbox("Enabled", &enabled);
        ImGui::SliderFloat("Intensity", &intensity, 0.0f, 100.0f, "%.2f");
        ImGui::ColorEdit3("Color", color);
        ImGui::TextDisabled("Imported light editing will be connected when light assets become mutable.");
    }

    if (ImGui::Button("Clear Selection")) {
        selection.clear();
    }

    ImGui::End();
}

} // namespace rtv
