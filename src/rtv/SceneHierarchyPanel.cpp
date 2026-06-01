#include "rtv/SceneHierarchyPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/EditorUiStyle.h"

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

enum HierarchyTypeFilter : uint32_t {
    HierarchyTypeFilterMesh = 1u << 0u,
    HierarchyTypeFilterCamera = 1u << 1u,
    HierarchyTypeFilterLight = 1u << 2u,
    HierarchyTypeFilterWorld = 1u << 3u,
    HierarchyTypeFilterAtmosphere = 1u << 4u,
    HierarchyTypeFilterEffects = 1u << 5u,
};

bool entityNameMatchesFilter(const Entity& entity, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    std::string name = entity.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return name.find(filter) != std::string::npos;
}

bool entityMatchesTypeFilter(const Entity& entity, uint32_t filterMask) {
    if (filterMask == 0) {
        return true;
    }
    uint32_t entityMask = 0;
    if (entity.meshRenderer.has_value()) {
        entityMask |= HierarchyTypeFilterMesh;
    }
    if (entity.camera.has_value()) {
        entityMask |= HierarchyTypeFilterCamera;
    }
    if (entity.light.has_value() || entity.sun.has_value()) {
        entityMask |= HierarchyTypeFilterLight;
    }
    if (entity.environmentLight.has_value()) {
        entityMask |= HierarchyTypeFilterWorld;
    }
    if (entity.skyAtmosphere.has_value() || entity.heightFog.has_value() || entity.volumetricCloud.has_value()) {
        entityMask |= HierarchyTypeFilterAtmosphere;
    }
    if (entity.postProcessVolume.has_value() || entity.cameraPostProcess.has_value()) {
        entityMask |= HierarchyTypeFilterEffects;
    }
    return (entityMask & filterMask) != 0;
}

void hierarchyTooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

void drawHierarchyIndentGuides(float cursorX, ImVec2 rowMin, ImVec2 rowMax, int depth) {
    if (depth <= 0) {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float indent = EditorUiMetric::hierarchyIndentSpacing;
    const ImU32 lineColor = IM_COL32(76, 86, 102, 105);
    const float y0 = rowMin.y + 2.0f;
    const float y1 = rowMax.y - 2.0f;
    for (int level = 0; level < depth; ++level) {
        const float x = cursorX - (static_cast<float>(depth - level) * indent) + indent * 0.42f;
        if (x > rowMin.x - 1.0f && x < rowMax.x) {
            dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), lineColor, 1.0f);
        }
    }
}

void drawHierarchyRightFade(ImVec2 rowMin, ImVec2 rowMax) {
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float width = EditorUiMetric::hierarchyRowRightFadeWidth;
    if (right <= rowMin.x + width) {
        return;
    }
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        ImVec2(right - width, rowMin.y + 1.0f),
        ImVec2(right, rowMax.y - 1.0f),
        IM_COL32(8, 10, 13, 0),
        IM_COL32(8, 10, 13, 210),
        IM_COL32(8, 10, 13, 210),
        IM_COL32(8, 10, 13, 0));
}

void drawHierarchyRowGlyph(EditorGlyphIcon icon, ImVec2 rowMin, ImVec2 rowMax, bool muted = false) {
    const float iconSize = EditorUiMetric::hierarchyIconSize;
    const float iconX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() + 2.0f;
    const float iconY = rowMin.y + std::max(0.0f, (rowMax.y - rowMin.y - iconSize) * 0.5f);
    editorDrawIconGlyph(
        icon,
        ImVec2(iconX, iconY),
        ImVec2(iconX + iconSize, iconY + iconSize),
        ImGui::GetColorU32(muted ? ImVec4(0.45f, 0.48f, 0.52f, 1.0f) : ImVec4(0.70f, 0.78f, 0.88f, 1.0f)));
}

bool selectableHierarchyGlyph(const char* label, bool selected, EditorGlyphIcon icon, int depth = 0) {
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    editorPushRowSelectionStyle();
    const bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_None, ImVec2(0.0f, EditorUiMetric::hierarchyRowHeight));
    editorPopRowSelectionStyle();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    drawHierarchyIndentGuides(rowStart.x, rowMin, rowMax, depth);
    drawHierarchyRightFade(rowMin, rowMax);
    drawHierarchyRowGlyph(icon, rowMin, rowMax);
    return clicked;
}

bool treeNodeHierarchyGlyph(const char* label, ImGuiTreeNodeFlags flags, EditorGlyphIcon icon, int depth = 0) {
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    editorPushRowSelectionStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, editorRowFramePadding(EditorUiMetric::hierarchyRowHeight));
    const bool open = ImGui::TreeNodeEx(label, flags);
    ImGui::PopStyleVar();
    editorPopRowSelectionStyle();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    drawHierarchyIndentGuides(rowStart.x, rowMin, rowMax, depth);
    drawHierarchyRightFade(rowMin, rowMax);
    drawHierarchyRowGlyph(icon, rowMin, rowMax);
    return open;
}

void hierarchyTypeFilterButton(uint32_t& filterMask, uint32_t filterBit, EditorGlyphIcon icon, const char* tooltip) {
    const ImVec2 size(18.0f, 18.0f);
    const bool active = (filterMask & filterBit) != 0;
    ImGui::InvisibleButton(tooltip, size);
    if (ImGui::IsItemClicked()) {
        filterMask ^= filterBit;
    }
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (active || ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            min,
            max,
            active ? ImGui::GetColorU32(editorSelectedRowColor()) : IM_COL32(34, 42, 54, 220),
            EditorUiMetric::compactButtonRounding);
    }
    editorDrawIconGlyph(
        icon,
        ImVec2(min.x + 1.0f, min.y + 1.0f),
        ImVec2(min.x + 17.0f, min.y + 17.0f),
        ImGui::GetColorU32(editorIconTint(active)));
    hierarchyTooltip(tooltip);
}

} // namespace

void SceneHierarchyPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    if (!ImGui::Begin(EditorDockWindowTitle::Hierarchy)) {
        ImGui::End();
        return;
    }

    if (state.sceneDocument != nullptr) {
        SceneDocument& document = *state.sceneDocument;
        SceneRegistry& registry = document.registry();

        if (editorIconTextButton("HierarchyCreateEmpty", EditorGlyphIcon::Entity, "Create Empty")) {
            requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Empty};
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        hierarchyTooltip("Create an empty entity at the scene root");
        ImGui::SameLine();
        if (editorIconTextButton("HierarchyCreateCamera", EditorGlyphIcon::Camera, "Camera")) {
            requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Camera};
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        hierarchyTooltip("Create a camera entity");
        ImGui::SameLine();
        if (editorIconTextButton("HierarchyCreateLight", EditorGlyphIcon::Light, "Light")) {
            requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Light};
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        hierarchyTooltip("Create a point light entity");
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("PREFAB_ASSET")) {
                requests.placeAsset = std::string(static_cast<const char*>(payload->Data));
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::Separator();
        hierarchyTypeFilterButton(typeFilterMask_, HierarchyTypeFilterMesh, EditorGlyphIcon::Model, "Filter mesh objects");
        ImGui::SameLine();
        hierarchyTypeFilterButton(typeFilterMask_, HierarchyTypeFilterCamera, EditorGlyphIcon::Camera, "Filter cameras");
        ImGui::SameLine();
        hierarchyTypeFilterButton(typeFilterMask_, HierarchyTypeFilterLight, EditorGlyphIcon::Light, "Filter lights and suns");
        ImGui::SameLine();
        hierarchyTypeFilterButton(typeFilterMask_, HierarchyTypeFilterWorld, EditorGlyphIcon::Environment, "Filter world environment actors");
        ImGui::SameLine();
        hierarchyTypeFilterButton(typeFilterMask_, HierarchyTypeFilterAtmosphere, EditorGlyphIcon::Sky, "Filter atmosphere, fog, and cloud actors");
        ImGui::SameLine();
        hierarchyTypeFilterButton(typeFilterMask_, HierarchyTypeFilterEffects, EditorGlyphIcon::PostProcess, "Filter post-process and effects actors");
        if (typeFilterMask_ != 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##HierarchyTypeFilters")) {
                typeFilterMask_ = 0;
            }
            hierarchyTooltip("Clear hierarchy type filters");
        }
        static std::array<char, 128> filterBuffer{};
        ImGui::InputTextWithHint("##entityFilter", "Search...", filterBuffer.data(), filterBuffer.size());
        std::string filter = filterBuffer.data();
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        ImGui::Separator();

        const EntityId selectedEntity = selection.entityId();
        if (selectedEntity.valid() && registry.contains(selectedEntity) && selectedEntity != lastSelectionForReveal_) {
            lastSelectionForReveal_ = selectedEntity;
            pendingRevealSelection_ = selectedEntity;
            lastScrolledSelection_ = {};
        }
        if (!selectedEntity.valid() || !registry.contains(selectedEntity)) {
            pendingRevealSelection_ = {};
            lastSelectionForReveal_ = {};
        }

        for (Entity* entity : registry.entities()) {
            if (entity->parent.valid()) {
                continue;
            }
            drawEntityNode(registry, *entity, selection, requests, filter, typeFilterMask_, false);
        }

        if (ImGui::BeginPopupContextWindow("HierarchyEmptyContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::BeginMenu("Create")) {
                if (editorGlyphMenuItem(EditorGlyphIcon::Entity, "Empty Entity")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Empty};
                    requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Camera, "Camera")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Camera};
                    requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Point Light")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Light};
                    requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Spot Light")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::SpotLight};
                    requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Area Light")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::AreaLight};
                    requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Environment, "Environment Light")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::EnvironmentLight};
                    requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Sky, "Sky Atmosphere")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::SkyAtmosphere};
                    requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Fog, "Height Fog")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::HeightFog};
                    requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Cloud, "Volumetric Cloud")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::VolumetricCloud};
                    requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::PostProcess, "Post Process Volume")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::PostProcessVolume};
                    requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
                }
                ImGui::EndMenu();
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Frame, "Frame Selection", selection.entityId().valid())) {
                requests.focusOnEntity = selection.entityId();
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Exit, "Clear Selection", selection.entityId().valid())) {
                selection.clear();
            }
            ImGui::EndPopup();
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

    if (selectableHierarchyGlyph("     Camera##fallbackCamera", selection.is(EditorSelectionKind::Camera), EditorGlyphIcon::Camera)) {
        selection.selectCamera();
    }

    const MeshParamsUniform& params = state.renderer.scene().meshParams();
    if (treeNodeHierarchyGlyph("     Lights##fallbackLights", ImGuiTreeNodeFlags_DefaultOpen, EditorGlyphIcon::Light)) {
        for (uint32_t i = 0; i < params.lightCount; ++i) {
            const std::string label = "     Emissive Light " + std::to_string(i) + "##fallbackLight" + std::to_string(i);
            if (selectableHierarchyGlyph(label.c_str(), selection.is(EditorSelectionKind::Light) && selection.index() == i, EditorGlyphIcon::Light, 1)) {
                selection.selectLight(i);
            }
        }
        if (params.lightCount == 0) {
            ImGui::TextDisabled("No scene lights");
        }
        ImGui::TreePop();
    }

    if (state.importedScene != nullptr && !state.importedScene->nodes.empty()) {
        if (treeNodeHierarchyGlyph("     glTF Scene##ImportedSceneRoot", ImGuiTreeNodeFlags_DefaultOpen, EditorGlyphIcon::SceneFile)) {
            for (uint32_t root : state.importedScene->rootNodes) {
                drawImportedNode(*state.importedScene, root, selection, 1);
            }
            ImGui::TreePop();
        }
    } else if (treeNodeHierarchyGlyph("     Cornell Fallback##CornellFallbackRoot", ImGuiTreeNodeFlags_DefaultOpen, EditorGlyphIcon::SceneFile)) {
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
            const bool lightObject = i == 6;
            const std::string label = std::string("     ") + objects[i] + "##fallbackObject" + std::to_string(i);
            if (selectableHierarchyGlyph(label.c_str(), selection.is(EditorSelectionKind::Object) && selection.index() == i, lightObject ? EditorGlyphIcon::Light : EditorGlyphIcon::Model, 1)) {
                selection.selectObject(i);
            }
        }
        for (uint32_t i = 0; i < params.sphereCount; ++i) {
            const std::string label = "     Sphere " + std::to_string(i) + "##fallbackSphere" + std::to_string(i);
            const uint32_t objectId = 1000u + i;
            if (selectableHierarchyGlyph(label.c_str(), selection.is(EditorSelectionKind::Object) && selection.index() == objectId, EditorGlyphIcon::Model, 1)) {
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

bool SceneHierarchyPanel::entityContainsFilter(const SceneRegistry& registry, const Entity& entity, const std::string& filter, uint32_t typeFilterMask) const {
    if (entityNameMatchesFilter(entity, filter) && entityMatchesTypeFilter(entity, typeFilterMask)) {
        return true;
    }
    for (EntityId childId : entity.children) {
        if (const Entity* child = registry.entity(childId)) {
            if (entityContainsFilter(registry, *child, filter, typeFilterMask)) {
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

void SceneHierarchyPanel::drawEntityNode(
    SceneRegistry& registry,
    Entity& entity,
    EditorSelection& selection,
    EditorRequests& requests,
    const std::string& filter,
    uint32_t typeFilterMask,
    bool ancestorMatchesFilter,
    int depth) {
    const bool selfMatchesFilter = entityNameMatchesFilter(entity, filter) && entityMatchesTypeFilter(entity, typeFilterMask);
    if (!ancestorMatchesFilter && !entityContainsFilter(registry, entity, filter, typeFilterMask)) {
        return;
    }
    const EntityId selected = selection.entityId();
    const bool containsSelection = entityContainsSelection(registry, entity, selected);
    const bool revealPending = pendingRevealSelection_.valid() && containsSelection;
    const bool forceOpenForReveal =
        (containsSelection && entity.id != selected) ||
        (revealPending && entity.id == selected && !entity.children.empty());

    std::string label = "     ";
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

    if (forceOpenForReveal) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    if (entity.locked || !entity.visible) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    editorPushRowSelectionStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, editorRowFramePadding(EditorUiMetric::hierarchyRowHeight));
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    ImGui::PopStyleVar();
    editorPopRowSelectionStyle();
    if (entity.locked || !entity.visible) {
        ImGui::PopStyleColor();
    }
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    drawHierarchyIndentGuides(rowStart.x, rowMin, rowMax, depth);
    drawHierarchyRightFade(rowMin, rowMax);
    drawHierarchyRowGlyph(editorGlyphForEntity(entity), rowMin, rowMax, entity.locked || !entity.visible);
    auto drawRowControls = [&] {
        const ImVec2 iconButtonSize = editorIconButtonSize();
        const float controlY = rowMin.y + std::max(0.0f, (rowMax.y - rowMin.y - iconButtonSize.y) * 0.5f);
        const float lockWidth = iconButtonSize.x;
        const float eyeWidth = iconButtonSize.x;
        const float gap = 3.0f;
        const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const ImVec2 restoreCursor = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(entity.id.index));
        ImGui::SetCursorScreenPos(ImVec2(rightEdge - lockWidth - eyeWidth - gap, controlY));
        if (editorIconButton("lock", entity.locked ? EditorGlyphIcon::Lock : EditorGlyphIcon::Unlock, entity.locked, iconButtonSize)) {
            requests.setEntityLocked = EditorEntityBoolChange{.entity = entity.id, .value = !entity.locked};
        }
        hierarchyTooltip(entity.locked ? "Locked" : "Unlocked");
        ImGui::SameLine(0.0f, gap);
        ImGui::BeginDisabled(entity.locked);
        if (editorIconButton("visible", entity.visible ? EditorGlyphIcon::EyeVisible : EditorGlyphIcon::EyeHidden, entity.visible, iconButtonSize)) {
            requests.setEntityVisibility = EditorEntityBoolChange{.entity = entity.id, .value = !entity.visible};
        }
        ImGui::EndDisabled();
        hierarchyTooltip(entity.visible ? "Visible" : "Hidden");
        ImGui::PopID();
        ImGui::SetCursorScreenPos(restoreCursor);
    };
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
                        .entity = entity.id,
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
        if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Duplicate", !entity.locked)) {
            requests.duplicateEntity = entity.id;
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Rename", !entity.locked)) {
            renameTarget_ = entity.id;
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete", !entity.locked)) {
            requests.deleteEntity = entity.id;
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        ImGui::Separator();
        if (editorGlyphMenuItem(entity.visible ? EditorGlyphIcon::EyeHidden : EditorGlyphIcon::EyeVisible, entity.visible ? "Hide" : "Show")) {
            requests.setEntityVisibility = EditorEntityBoolChange{.entity = entity.id, .value = !entity.visible};
        }
        if (editorGlyphMenuItem(entity.locked ? EditorGlyphIcon::Unlock : EditorGlyphIcon::Lock, entity.locked ? "Unlock" : "Lock")) {
            requests.setEntityLocked = EditorEntityBoolChange{.entity = entity.id, .value = !entity.locked};
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Create Child")) {
            if (editorGlyphMenuItem(EditorGlyphIcon::Entity, "Empty")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Empty, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Camera, "Camera")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Camera, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Light")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Light, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Spot Light")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::SpotLight, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Area Light")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::AreaLight, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Sky, "Sky Atmosphere")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::SkyAtmosphere, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Fog, "Height Fog")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::HeightFog, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::PostProcess, "Post Process Volume")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::PostProcessVolume, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
            }
            ImGui::EndMenu();
        }
        if (entity.parent.valid() && editorGlyphMenuItem(EditorGlyphIcon::Group, "Detach Parent")) {
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
        if (editorGlyphMenuItem(EditorGlyphIcon::Frame, "Focus in Viewport")) {
            requests.focusOnEntity = entity.id;
        }
        ImGui::EndPopup();
    }
    if (selected == entity.id && (lastScrolledSelection_ != selected || pendingRevealSelection_ == selected)) {
        ImGui::SetScrollHereY(0.25f);
        lastScrolledSelection_ = selected;
        if (pendingRevealSelection_ == selected) {
            pendingRevealSelection_ = {};
        }
    }
    drawRowControls();
    if (open && !entity.children.empty()) {
        for (EntityId childId : entity.children) {
            if (Entity* child = registry.entity(childId)) {
                drawEntityNode(registry, *child, selection, requests, filter, typeFilterMask, ancestorMatchesFilter || selfMatchesFilter, depth + 1);
            }
        }
        ImGui::TreePop();
    }
}

void SceneHierarchyPanel::drawImportedNode(const SceneAsset& scene, uint32_t nodeIndex, EditorSelection& selection, int depth) {
    if (nodeIndex >= scene.nodes.size()) {
        return;
    }
    const SceneNodeAsset& node = scene.nodes[nodeIndex];
    const std::string label = "     " +
        (node.name.empty() ? "Node " + std::to_string(nodeIndex) : node.name) + "##node" + std::to_string(nodeIndex);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selection.is(EditorSelectionKind::Object) && selection.index() == nodeIndex) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const bool open = treeNodeHierarchyGlyph(label.c_str(), flags, node.children.empty() ? EditorGlyphIcon::Model : EditorGlyphIcon::Group, depth);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selection.selectObject(nodeIndex);
    }
    if (open && !node.children.empty()) {
        for (uint32_t child : node.children) {
            drawImportedNode(scene, child, selection, depth + 1);
        }
        ImGui::TreePop();
    }
}

} // namespace rtv
