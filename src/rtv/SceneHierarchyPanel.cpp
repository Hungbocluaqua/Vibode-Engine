#include "rtv/SceneHierarchyPanel.h"

#include "rtv/AssetManager.h"

#include <imgui.h>

#include <array>
#include <string>

namespace rtv {

void SceneHierarchyPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    if (!ImGui::Begin("Scene Hierarchy")) {
        ImGui::End();
        return;
    }

    if (state.sceneDocument != nullptr) {
        SceneDocument& document = *state.sceneDocument;
        SceneRegistry& registry = document.registry();

        if (ImGui::Button("Create Empty")) {
            const EntityId id = registry.createEntity("Entity");
            selection.selectEntity(id);
            requests.sceneUpdate = SceneUpdateKind::FullSceneRebuild;
        }
        ImGui::SameLine();
        if (ImGui::Button("Camera")) {
            const EntityId id = registry.createEntity("Camera");
            Camera camera;
            camera.active = true;
            registry.addCamera(id, camera);
            document.setActiveCamera(id);
            selection.selectEntity(id);
            requests.sceneUpdate = SceneUpdateKind::CameraOnly;
        }
        ImGui::SameLine();
        if (ImGui::Button("Light")) {
            const EntityId id = registry.createEntity("Point Light");
            registry.addLight(id, Light{});
            selection.selectEntity(id);
            requests.sceneUpdate = SceneUpdateKind::LightOnly;
        }

        ImGui::Separator();
        for (Entity* entity : registry.entities()) {
            std::string label;
            if (entity->meshRenderer.has_value()) {
                label += "[M] ";
            }
            if (entity->light.has_value()) {
                label += "[L] ";
            }
            if (entity->camera.has_value()) {
                label += "[C] ";
            }
            label += entity->name.empty() ? "Entity" : entity->name;
            label += "##entity" + std::to_string(entity->id.index) + "_" + std::to_string(entity->id.generation);

            if (ImGui::Selectable(label.c_str(), selection.entityId() == entity->id)) {
                selection.selectEntity(entity->id);
            }
        }

        if (registry.liveCount() == 0) {
            ImGui::TextDisabled("No scene entities");
        }
        if (selection.entityId().valid() && !registry.contains(selection.entityId())) {
            selection.clear();
        }
        ImGui::End();
        return;
    }

    if (ImGui::Selectable("Camera", selection.is(EditorSelectionKind::Camera))) {
        selection.selectCamera();
    }

    const MeshParamsUniform& params = state.renderer.scene().meshParams();
    if (ImGui::TreeNodeEx("Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (uint32_t i = 0; i < params.lightCount; ++i) {
            const std::string label = "Emissive Light " + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), selection.is(EditorSelectionKind::Light) && selection.index() == i)) {
                selection.selectLight(i);
            }
        }
        if (params.lightCount == 0) {
            ImGui::TextDisabled("No scene lights");
        }
        ImGui::TreePop();
    }

    if (state.importedScene != nullptr && !state.importedScene->nodes.empty()) {
        if (ImGui::TreeNodeEx("glTF Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (uint32_t root : state.importedScene->rootNodes) {
                drawImportedNode(*state.importedScene, root, selection);
            }
            ImGui::TreePop();
        }
    } else if (ImGui::TreeNodeEx("Cornell Fallback", ImGuiTreeNodeFlags_DefaultOpen)) {
        static constexpr const char* objects[] = {
            "Cornell Box",
            "Left Wall",
            "Right Wall",
            "Back Wall",
            "Floor",
            "Ceiling",
            "Area Light",
        };
        for (uint32_t i = 0; i < static_cast<uint32_t>(std::size(objects)); ++i) {
            if (ImGui::Selectable(objects[i], selection.is(EditorSelectionKind::Object) && selection.index() == i)) {
                selection.selectObject(i);
            }
        }
        for (uint32_t i = 0; i < params.sphereCount; ++i) {
            const std::string label = "Sphere " + std::to_string(i);
            const uint32_t objectId = 1000u + i;
            if (ImGui::Selectable(label.c_str(), selection.is(EditorSelectionKind::Object) && selection.index() == objectId)) {
                selection.selectObject(objectId);
            }
        }
        ImGui::TreePop();
    }

    ImGui::End();
}

void SceneHierarchyPanel::drawImportedNode(const SceneAsset& scene, uint32_t nodeIndex, EditorSelection& selection) {
    if (nodeIndex >= scene.nodes.size()) {
        return;
    }
    const SceneNodeAsset& node = scene.nodes[nodeIndex];
    const std::string label = (node.name.empty() ? "Node " + std::to_string(nodeIndex) : node.name) + "##node" + std::to_string(nodeIndex);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selection.is(EditorSelectionKind::Object) && selection.index() == nodeIndex) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selection.selectObject(nodeIndex);
    }
    if (open && !node.children.empty()) {
        for (uint32_t child : node.children) {
            drawImportedNode(scene, child, selection);
        }
        ImGui::TreePop();
    }
}

} // namespace rtv
