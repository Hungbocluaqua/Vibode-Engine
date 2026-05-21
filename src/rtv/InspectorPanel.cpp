#include "rtv/InspectorPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
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

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
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

        const bool entityLocked = entity->locked;
        if (entityLocked) {
            ImGui::TextDisabled("This entity is locked.");
        }
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
            document.registry().renameEntity(entity->id, nameBuffer.data());
            document.markDirty(SceneUpdateKind::None);
        }

        ImGui::SeparatorText("Transform");
        bool transformChanged = false;
        static std::optional<Transform> copiedTransform;
        if (ImGui::SmallButton("Reset Position")) {
            entity->transform.position = glm::vec3(0.0f);
            transformChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset Rotation")) {
            entity->transform.rotationEuler = glm::vec3(0.0f);
            transformChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset Scale")) {
            entity->transform.scale = glm::vec3(1.0f);
            transformChanged = true;
        }
        if (ImGui::SmallButton("Copy Transform")) {
            copiedTransform = entity->transform;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!copiedTransform.has_value());
        if (ImGui::SmallButton("Paste Transform") && copiedTransform.has_value()) {
            entity->transform = *copiedTransform;
            transformChanged = true;
        }
        ImGui::EndDisabled();
        transformChanged |= ImGui::DragFloat3("Position", glm::value_ptr(entity->transform.position), 0.02f, -10000.0f, 10000.0f, "%.3f");
        glm::vec3 rotationDegrees = glm::degrees(entity->transform.rotationEuler);
        if (ImGui::DragFloat3("Rotation", glm::value_ptr(rotationDegrees), 0.2f, -360.0f, 360.0f, "%.2f")) {
            entity->transform.rotationEuler = glm::radians(rotationDegrees);
            transformChanged = true;
        }
        tooltip("Euler angles in degrees.");
        transformChanged |= ImGui::DragFloat3("Scale", glm::value_ptr(entity->transform.scale), 0.02f, 0.001f, 1000.0f, "%.3f");
        if (transformChanged) {
            entity->transform.dirty = true;
            const SceneUpdateKind updateKind = transformUpdateKind(document, *entity);
            document.markDirty(updateKind);
            requests.sceneUpdate = updateKind;
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
            ensureMaterialSlots(renderer, state.assets);
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
                                    slot.overrideMaterial = candidate.index == slot.material.index ? std::optional<MaterialAssetHandle>{} : std::optional<MaterialAssetHandle>{candidate};
                                    requests.materialAssignment = EditorMaterialAssignment{
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
            if (changed) {
                document.markDirty(SceneUpdateKind::VisibilityOnly);
                requests.sceneUpdate = SceneUpdateKind::VisibilityOnly;
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Duplicate Entity")) {
            const Entity source = *entity;
            const EntityId copyId = document.registry().createEntity(source.name.empty() ? "Entity Copy" : source.name + " Copy");
            if (Entity* copy = document.registry().entity(copyId)) {
                copy->transform = source.transform;
                copy->transform.dirty = true;
                copy->meshRenderer = source.meshRenderer;
                copy->light = source.light;
                copy->camera = source.camera;
                if (copy->camera.has_value()) {
                    copy->camera->active = false;
                }
                document.markDirty(copy->meshRenderer.has_value() ? SceneUpdateKind::TopologyChanged :
                    (copy->light.has_value() ? SceneUpdateKind::LightOnly :
                        (copy->camera.has_value() ? SceneUpdateKind::CameraOnly : SceneUpdateKind::TopologyChanged)));
                requests.sceneUpdate = document.pendingUpdate();
                selection.selectEntity(copyId);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Entity")) {
            ImGui::OpenPopup("Delete##confirm");
        }
        if (ImGui::BeginPopupModal("Delete##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete '%s'?", entity->name.empty() ? "Entity" : entity->name.c_str());
            if (ImGui::Button("Yes")) {
                const EntityId deleted = entity->id;
                if (document.activeCamera() == deleted) {
                    document.setActiveCamera({});
                }
                if (document.registry().destroyEntity(deleted)) {
                    selection.clear();
                    document.markDirty(SceneUpdateKind::TopologyChanged);
                    requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                }
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
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

        if (ImGui::Button("Clear Selection")) {
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
        ImGui::TextDisabled("Select an entity light in the scene hierarchy to edit authored light data.");
    }

    if (ImGui::Button("Clear Selection")) {
        selection.clear();
    }

    ImGui::End();
}

} // namespace rtv
