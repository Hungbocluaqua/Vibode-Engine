#include "rtv/SceneHierarchyPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace rtv {

std::array<char, 256> SceneHierarchyPanel::renameBuffer_{};

namespace {

void placeCreatedEntity(const EditorRuntimeState& state, Entity* entity) {
    if (entity == nullptr || state.camera == nullptr) {
        return;
    }
    glm::vec3 forward = state.camera->direction();
    if (glm::dot(forward, forward) <= 0.0f) {
        forward = glm::vec3(0.0f, 0.0f, -1.0f);
    } else {
        forward = glm::normalize(forward);
    }
    entity->transform.position = state.camera->position() + forward * 2.5f;
    entity->transform.dirty = true;
}

} // namespace

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
            placeCreatedEntity(state, registry.entity(id));
            selection.selectEntity(id);
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        ImGui::SameLine();
        if (ImGui::Button("Camera")) {
            const EntityId id = registry.createEntity("Camera");
            placeCreatedEntity(state, registry.entity(id));
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
            placeCreatedEntity(state, registry.entity(id));
            registry.addLight(id, Light{});
            selection.selectEntity(id);
            requests.sceneUpdate = SceneUpdateKind::LightOnly;
        }

        ImGui::Separator();
        static std::array<char, 128> filterBuffer{};
        ImGui::InputTextWithHint("##entityFilter", "Filter entities...", filterBuffer.data(), filterBuffer.size());
        std::string filter = filterBuffer.data();
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        ImGui::Separator();
        for (Entity* entity : registry.entities()) {
            if (entity->parent.valid()) {
                continue;
            }
            drawEntityNode(registry, *entity, selection, requests, filter);
        }

        if (registry.liveCount() == 0) {
            ImGui::TextDisabled("No scene entities");
        }
        if (selection.entityId().valid() && !registry.contains(selection.entityId())) {
            selection.clear();
        }
        if (!selection.entityId().valid()) {
            lastScrolledSelection_ = {};
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

bool SceneHierarchyPanel::entityContainsSelection(const SceneRegistry& registry, const Entity& entity, EntityId selected) const {
    if (!selected.valid()) {
        return false;
    }
    if (entity.id == selected) {
        return true;
    }
    for (EntityId childId : entity.children) {
        if (const Entity* child = registry.entity(childId)) {
            if (entityContainsSelection(registry, *child, selected)) {
                return true;
            }
        }
    }
    return false;
}

bool SceneHierarchyPanel::entityContainsFilter(const SceneRegistry& registry, const Entity& entity, const std::string& filter) const {
    if (filter.empty()) {
        return true;
    }
    std::string name = entity.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (name.find(filter) != std::string::npos) {
        return true;
    }
    for (EntityId childId : entity.children) {
        if (const Entity* child = registry.entity(childId)) {
            if (entityContainsFilter(registry, *child, filter)) {
                return true;
            }
        }
    }
    return false;
}

void recurseFlatten(SceneRegistry& registry, const Entity& entity, std::vector<EntityId>& out) {
    out.push_back(entity.id);
    for (EntityId childId : entity.children) {
        const Entity* child = registry.entity(childId);
        if (child != nullptr) {
            recurseFlatten(registry, *child, out);
        }
    }
}

void SceneHierarchyPanel::drawEntityNode(SceneRegistry& registry, Entity& entity, EditorSelection& selection, EditorRequests& requests, const std::string& filter) {
    if (!entityContainsFilter(registry, entity, filter)) {
        return;
    }
    const EntityId selected = selection.entityId();
    const bool containsSelection = entityContainsSelection(registry, entity, selected);
    if (containsSelection && entity.id != selected) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }

    std::string label;
    if (entity.meshRenderer.has_value()) {
        label += "[M] ";
    }
    if (entity.light.has_value()) {
        label += "[L] ";
    }
    if (entity.camera.has_value()) {
        label += "[C] ";
    }
    label += entity.name.empty() ? "Entity" : entity.name;
    label += "##entity" + std::to_string(entity.id.index) + "_" + std::to_string(entity.id.generation);

    if (renameTarget_.has_value() && *renameTarget_ == entity.id) {
        ImGui::SetKeyboardFocusHere();
        const size_t len = std::min(entity.name.size(), renameBuffer_.size() - 1);
        std::memcpy(renameBuffer_.data(), entity.name.c_str(), len);
        renameBuffer_[len] = '\0';
        ImGui::PushItemWidth(-1.0f);
        const bool confirm = ImGui::InputText("##renameInput", renameBuffer_.data(), renameBuffer_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopItemWidth();
        const bool loseFocus = ImGui::IsItemDeactivated();
        if (confirm || loseFocus) {
            std::string newName(renameBuffer_.data());
            if (!newName.empty()) {
                entity.name = newName;
                requests.sceneUpdate = SceneUpdateKind::TransformOnly;
            }
            renameTarget_.reset();
            renameBuffer_.fill('\0');
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            renameTarget_.reset();
            renameBuffer_.fill('\0');
        }
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (entity.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selection.entityId() == entity.id) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    ImGui::PushID(static_cast<int>(entity.id.index));
    bool locked = entity.locked;
    if (ImGui::Checkbox("##locked", &locked)) {
        requests.setEntityLocked = EditorEntityBoolChange{.entity = entity.id, .value = locked};
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", locked ? "Locked" : "Unlocked");
    }
    ImGui::SameLine();
    {
        bool visible = entity.visible;
        ImGui::BeginDisabled(entity.locked);
        if (ImGui::Checkbox("##visible", &visible)) {
            requests.setEntityVisibility = EditorEntityBoolChange{.entity = entity.id, .value = visible};
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", visible ? "Visible" : "Hidden");
        }
        ImGui::SameLine();
    }
    ImGui::PopID();
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (!entity.locked) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("ENTITY", &entity.id, sizeof(entity.id));
            ImGui::Text("Reparent %s", entity.name.empty() ? "Entity" : entity.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("ENTITY")) {
                const EntityId sourceId = *static_cast<const EntityId*>(payload->Data);
                if (sourceId.valid() && sourceId != entity.id) {
                    EntityId ancestor = entity.id;
                    bool valid = true;
                    while (ancestor.valid()) {
                        if (ancestor == sourceId) { valid = false; break; }
                        const Entity* anc = registry.entity(ancestor);
                        ancestor = anc ? anc->parent : EntityId{};
                    }
                    if (valid) {
                        requests.reparentEntity = std::make_pair(sourceId, entity.id);
                        requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                    }
                }
            }
            if (const auto* payload = ImGui::AcceptDragDropPayload("MATERIAL")) {
                const uint32_t materialId = *static_cast<const uint32_t*>(payload->Data);
                if (entity.meshRenderer.has_value() && materialId < UINT32_MAX) {
                    requests.materialAssignment = EditorMaterialAssignment{
                        .mesh = entity.meshRenderer->mesh,
                        .primitiveIndex = UINT32_MAX,
                        .material = MaterialAssetHandle{materialId},
                    };
                    requests.sceneUpdate = SceneUpdateKind::MaterialOnly;
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    if (!entity.locked && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyCtrl) {
            selection.toggleEntity(entity.id);
        } else if (ImGui::GetIO().KeyShift && selection.lastClickedId().valid()) {
            std::vector<EntityId> flattened;
            for (Entity* root : registry.entities()) {
                if (!root->parent.valid()) {
                    recurseFlatten(registry, *root, flattened);
                }
            }
            selection.selectRangeFromFlattenedList(flattened, entity.id);
        } else {
            selection.selectEntity(entity.id);
        }
    }
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Duplicate")) {
            requests.duplicateEntity = entity.id;
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        if (ImGui::MenuItem("Rename")) {
            renameTarget_ = entity.id;
        }
        if (ImGui::MenuItem("Delete")) {
            requests.deleteEntity = entity.id;
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        if (ImGui::BeginMenu("Create Child")) {
            if (ImGui::MenuItem("Empty")) {
                const EntityId parentId = entity.id;
                const EntityId childId = registry.createEntity("Entity");
                if (Entity* child = registry.entity(childId)) {
                    child->parent = parentId;
                    if (Entity* parent = registry.entity(parentId)) {
                        parent->children.push_back(childId);
                    }
                    requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                    selection.selectEntity(childId);
                }
            }
            if (ImGui::MenuItem("Camera")) {
                const EntityId parentId = entity.id;
                const EntityId childId = registry.createEntity("Camera");
                if (Entity* child = registry.entity(childId)) {
                    child->parent = parentId;
                    if (Entity* parent = registry.entity(parentId)) {
                        parent->children.push_back(childId);
                    }
                    child->camera = Camera{};
                    requests.sceneUpdate = SceneUpdateKind::CameraOnly;
                    selection.selectEntity(childId);
                }
            }
            if (ImGui::MenuItem("Light")) {
                const EntityId parentId = entity.id;
                const EntityId childId = registry.createEntity("Point Light");
                if (Entity* child = registry.entity(childId)) {
                    child->parent = parentId;
                    if (Entity* parent = registry.entity(parentId)) {
                        parent->children.push_back(childId);
                    }
                    child->light = Light{};
                    requests.sceneUpdate = SceneUpdateKind::LightOnly;
                    selection.selectEntity(childId);
                }
            }
            ImGui::EndMenu();
        }
        if (entity.parent.valid() && ImGui::MenuItem("Detach Parent")) {
            if (Entity* parent = registry.entity(entity.parent)) {
                parent->children.erase(
                    std::remove(parent->children.begin(), parent->children.end(), entity.id),
                    parent->children.end());
            }
            entity.parent = {};
            registry.markDirty(SceneUpdateKind::TopologyChanged);
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Focus in Viewport")) {
            requests.focusOnEntity = entity.id;
        }
        ImGui::EndPopup();
    }
    if (selected == entity.id && lastScrolledSelection_ != selected) {
        ImGui::SetScrollHereY(0.25f);
        lastScrolledSelection_ = selected;
    }
    if (open && !entity.children.empty()) {
        for (EntityId childId : entity.children) {
            if (Entity* child = registry.entity(childId)) {
                drawEntityNode(registry, *child, selection, requests, filter);
            }
        }
        ImGui::TreePop();
    }
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
