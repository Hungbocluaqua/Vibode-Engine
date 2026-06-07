#include "rtv/SceneHierarchyPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/EditorUiStyle.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

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

struct HierarchyLayerSummary {
    std::string name;
    uint32_t count = 0;
    uint32_t visibleCount = 0;
    uint32_t lockedCount = 0;
};

struct HierarchyTagSummary {
    std::string name;
    uint32_t count = 0;
};

struct HierarchyCollectionSummary {
    std::string name;
    uint32_t count = 0;
};

std::string normalizedLayerName(const Entity& entity) {
    return entity.layer.empty() ? "Default" : entity.layer;
}

std::vector<HierarchyLayerSummary> collectHierarchyLayers(SceneRegistry& registry) {
    std::vector<HierarchyLayerSummary> layers;
    for (Entity* entity : registry.entities()) {
        const std::string layerName = normalizedLayerName(*entity);
        auto it = std::find_if(layers.begin(), layers.end(), [&](const HierarchyLayerSummary& layer) {
            return layer.name == layerName;
        });
        if (it == layers.end()) {
            it = layers.insert(layers.end(), HierarchyLayerSummary{.name = layerName});
        }
        ++it->count;
        if (entity->visible) {
            ++it->visibleCount;
        }
        if (entity->locked) {
            ++it->lockedCount;
        }
    }
    std::sort(layers.begin(), layers.end(), [](const HierarchyLayerSummary& lhs, const HierarchyLayerSummary& rhs) {
        if (lhs.name == "Default") {
            return rhs.name != "Default";
        }
        if (rhs.name == "Default") {
            return false;
        }
        return lhs.name < rhs.name;
    });
    return layers;
}

std::vector<HierarchyTagSummary> collectHierarchyTags(SceneRegistry& registry) {
    std::vector<HierarchyTagSummary> tags;
    for (Entity* entity : registry.entities()) {
        for (const std::string& tagName : entity->tags) {
            if (tagName.empty()) {
                continue;
            }
            auto it = std::find_if(tags.begin(), tags.end(), [&](const HierarchyTagSummary& tag) {
                return tag.name == tagName;
            });
            if (it == tags.end()) {
                it = tags.insert(tags.end(), HierarchyTagSummary{.name = tagName});
            }
            ++it->count;
        }
    }
    std::sort(tags.begin(), tags.end(), [](const HierarchyTagSummary& lhs, const HierarchyTagSummary& rhs) {
        return lhs.name < rhs.name;
    });
    return tags;
}

std::vector<HierarchyCollectionSummary> collectHierarchyCollections(SceneRegistry& registry) {
    std::vector<HierarchyCollectionSummary> collections;
    for (Entity* entity : registry.entities()) {
        for (const std::string& collectionName : entity->collections) {
            if (collectionName.empty()) {
                continue;
            }
            auto it = std::find_if(collections.begin(), collections.end(), [&](const HierarchyCollectionSummary& collection) {
                return collection.name == collectionName;
            });
            if (it == collections.end()) {
                it = collections.insert(collections.end(), HierarchyCollectionSummary{.name = collectionName});
            }
            ++it->count;
        }
    }
    std::sort(collections.begin(), collections.end(), [](const HierarchyCollectionSummary& lhs, const HierarchyCollectionSummary& rhs) {
        return lhs.name < rhs.name;
    });
    return collections;
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

void appendHierarchySearchTerm(std::string& text, const char* term) {
    if (term == nullptr || term[0] == '\0') {
        return;
    }
    if (!text.empty()) {
        text.push_back(' ');
    }
    text += term;
}

bool entityTextMatchesFilter(const Entity& entity, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }

    std::string searchable = entity.name;
    appendHierarchySearchTerm(searchable, entity.layer.c_str());
    for (const std::string& tag : entity.tags) {
        appendHierarchySearchTerm(searchable, tag.c_str());
    }
    for (const std::string& collection : entity.collections) {
        appendHierarchySearchTerm(searchable, collection.c_str());
    }
    appendHierarchySearchTerm(searchable, entity.visible ? "visible" : "hidden");
    appendHierarchySearchTerm(searchable, entity.locked ? "locked" : "unlocked");
    if (entity.meshRenderer.has_value()) {
        appendHierarchySearchTerm(searchable, "mesh model meshrenderer mesh renderer geometry");
    }
    if (entity.camera.has_value()) {
        appendHierarchySearchTerm(searchable, "camera view projection");
    }
    if (entity.light.has_value()) {
        appendHierarchySearchTerm(searchable, "light point spot area");
    }
    if (entity.sun.has_value()) {
        appendHierarchySearchTerm(searchable, "sun directional light");
    }
    if (entity.environmentLight.has_value()) {
        appendHierarchySearchTerm(searchable, "environment environmentlight environment light world hdr hdri");
    }
    if (entity.skyAtmosphere.has_value()) {
        appendHierarchySearchTerm(searchable, "sky atmosphere skyatmosphere");
    }
    if (entity.heightFog.has_value()) {
        appendHierarchySearchTerm(searchable, "heightfog height fog atmosphere");
    }
    if (entity.volumetricCloud.has_value()) {
        appendHierarchySearchTerm(searchable, "volumetriccloud volumetric cloud atmosphere");
    }
    if (entity.postProcessVolume.has_value()) {
        appendHierarchySearchTerm(searchable, "postprocess post process volume effects");
    }
    if (entity.cameraPostProcess.has_value()) {
        appendHierarchySearchTerm(searchable, "camerapostprocess camera post process effects");
    }

    std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return searchable.find(filter) != std::string::npos;
}

bool entityMatchesLayerFilter(const Entity& entity, const std::string& layerFilter) {
    return layerFilter.empty() || normalizedLayerName(entity) == layerFilter;
}

bool entityMatchesTagFilter(const Entity& entity, const std::string& tagFilter) {
    return tagFilter.empty() || std::find(entity.tags.begin(), entity.tags.end(), tagFilter) != entity.tags.end();
}

bool entityMatchesCollectionFilter(const Entity& entity, const std::string& collectionFilter) {
    return collectionFilter.empty() || std::find(entity.collections.begin(), entity.collections.end(), collectionFilter) != entity.collections.end();
}

bool entityMatchesHierarchyFilters(
    const Entity& entity,
    const std::string& filter,
    const std::string& layerFilter,
    const std::string& tagFilter,
    const std::string& collectionFilter,
    uint32_t typeFilterMask) {
    return entityTextMatchesFilter(entity, filter) &&
        entityMatchesLayerFilter(entity, layerFilter) &&
        entityMatchesTagFilter(entity, tagFilter) &&
        entityMatchesCollectionFilter(entity, collectionFilter) &&
        entityMatchesTypeFilter(entity, typeFilterMask);
}

std::vector<EntityId> collectFilteredSelectableEntities(
    SceneRegistry& registry,
    const std::string& filter,
    const std::string& layerFilter,
    const std::string& tagFilter,
    const std::string& collectionFilter,
    uint32_t typeFilterMask) {
    std::vector<EntityId> entities;
    for (Entity* entity : registry.entities()) {
        if (entity == nullptr || entity->locked) {
            continue;
        }
        if (entityMatchesHierarchyFilters(*entity, filter, layerFilter, tagFilter, collectionFilter, typeFilterMask)) {
            entities.push_back(entity->id);
        }
    }
    return entities;
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
    const float rowHeight = rowMax.y - rowMin.y;
    const float iconSize = editorIconSizeForRow(rowHeight, EditorUiMetric::hierarchyIconSize);
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
    editorDrawPreRowBand(EditorUiMetric::hierarchyRowHeight);
    editorPushRowSelectionStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    const bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_None, ImVec2(0.0f, EditorUiMetric::hierarchyRowHeight));
    ImGui::PopStyleVar();
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
    editorDrawPreRowBand(EditorUiMetric::hierarchyRowHeight);
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
            active ? ImGui::GetColorU32(editorSelectedRowColor()) : IM_COL32(30, 34, 41, 220),
            EditorUiMetric::compactButtonRounding);
    }
    editorDrawIconGlyph(
        icon,
        ImVec2(min.x + 1.0f, min.y + 1.0f),
        ImVec2(min.x + 17.0f, min.y + 17.0f),
        ImGui::GetColorU32(editorIconTint(active)));
    hierarchyTooltip(tooltip);
}

bool hierarchyRowIconButton(const char* id, EditorGlyphIcon icon, bool enabled, bool muted, ImVec2 size) {
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool active = enabled && ImGui::IsItemActive();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || active) {
        dl->AddRectFilled(min, max, IM_COL32(38, 43, 52, active ? 190 : 145), EditorUiMetric::compactButtonRounding);
        dl->AddRect(min, max, IM_COL32(70, 78, 92, 135), EditorUiMetric::compactButtonRounding);
    }
    const ImVec4 tint = muted || !enabled ? editorDisabledIconTint() : ImVec4(0.62f, 0.66f, 0.72f, 0.95f);
    constexpr float glyphSize = 13.0f;
    const ImVec2 glyphMin(
        min.x + std::max(0.0f, (size.x - glyphSize) * 0.5f),
        min.y + std::max(0.0f, (size.y - glyphSize) * 0.5f));
    editorDrawIconGlyph(
        icon,
        glyphMin,
        ImVec2(glyphMin.x + glyphSize, glyphMin.y + glyphSize),
        ImGui::GetColorU32(tint));
    if (!enabled) {
        ImGui::EndDisabled();
    }
    return enabled && pressed;
}

void applyLayerVisibility(SceneDocument& document, const std::string& layerName, bool visible) {
    for (Entity* entity : document.registry().entities()) {
        if (normalizedLayerName(*entity) == layerName) {
            entity->visible = visible;
            if (entity->meshRenderer.has_value()) {
                entity->meshRenderer->visible = visible;
            }
        }
    }
}

void applyLayerLocked(SceneDocument& document, const std::string& layerName, bool locked) {
    for (Entity* entity : document.registry().entities()) {
        if (normalizedLayerName(*entity) == layerName) {
            entity->locked = locked;
        }
    }
}

void drawHierarchyLayerControls(SceneDocument& document, EditorRequests& requests, std::string& layerFilter) {
    std::vector<HierarchyLayerSummary> layers = collectHierarchyLayers(document.registry());
    if (layers.empty()) {
        layerFilter.clear();
        return;
    }
    if (!layerFilter.empty() && std::none_of(layers.begin(), layers.end(), [&](const HierarchyLayerSummary& layer) {
            return layer.name == layerFilter;
        })) {
        layerFilter.clear();
    }

    if (!ImGui::TreeNodeEx("Layers", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }

    for (const HierarchyLayerSummary& layer : layers) {
        ImGui::PushID(layer.name.c_str());
        const bool allVisible = layer.visibleCount == layer.count;
        const bool allLocked = layer.lockedCount == layer.count;
        ImGui::TextUnformatted(layer.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%u", layer.count);

        ImGui::SameLine();
        const bool layerFilterActive = layerFilter == layer.name;
        if (editorIconButton("LayerFilter", EditorGlyphIcon::List, layerFilterActive, ImVec2(20.0f, 18.0f))) {
            layerFilter = layerFilterActive ? std::string{} : layer.name;
        }
        hierarchyTooltip(layerFilterActive ? "Clear this layer filter" : "Filter hierarchy to this layer");

        ImGui::SameLine();
        if (editorIconButton("LayerVisibility", allVisible ? EditorGlyphIcon::EyeVisible : EditorGlyphIcon::EyeHidden, allVisible, ImVec2(20.0f, 18.0f))) {
            const SceneDocument before = document;
            applyLayerVisibility(document, layer.name, !allVisible);
            requests.sceneSnapshot = EditorSceneSnapshotChange{
                .before = before,
                .updateKind = SceneUpdateKind::VisibilityOnly,
                .label = allVisible ? "Hide Layer" : "Show Layer",
            };
        }
        hierarchyTooltip(allVisible ? "Hide all entities in this layer" : "Show all entities in this layer");

        ImGui::SameLine();
        if (editorIconButton("LayerLock", allLocked ? EditorGlyphIcon::Lock : EditorGlyphIcon::Unlock, allLocked, ImVec2(20.0f, 18.0f))) {
            const SceneDocument before = document;
            applyLayerLocked(document, layer.name, !allLocked);
            requests.sceneSnapshot = EditorSceneSnapshotChange{
                .before = before,
                .updateKind = SceneUpdateKind::None,
                .label = allLocked ? "Unlock Layer" : "Lock Layer",
            };
        }
        hierarchyTooltip(allLocked ? "Unlock all entities in this layer" : "Lock all entities in this layer");
        ImGui::PopID();
    }

    ImGui::TreePop();
}

void drawHierarchyTagControls(SceneDocument& document, std::string& tagFilter) {
    std::vector<HierarchyTagSummary> tags = collectHierarchyTags(document.registry());
    if (tags.empty()) {
        tagFilter.clear();
        return;
    }
    if (!tagFilter.empty() && std::none_of(tags.begin(), tags.end(), [&](const HierarchyTagSummary& tag) {
            return tag.name == tagFilter;
        })) {
        tagFilter.clear();
    }

    if (!ImGui::TreeNodeEx("Tags", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }

    for (const HierarchyTagSummary& tag : tags) {
        ImGui::PushID(tag.name.c_str());
        ImGui::TextUnformatted(tag.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%u", tag.count);
        ImGui::SameLine();
        const bool tagFilterActive = tagFilter == tag.name;
        if (editorIconButton("TagFilter", EditorGlyphIcon::List, tagFilterActive, ImVec2(20.0f, 18.0f))) {
            tagFilter = tagFilterActive ? std::string{} : tag.name;
        }
        hierarchyTooltip(tagFilterActive ? "Clear this tag filter" : "Filter hierarchy to this tag");
        ImGui::PopID();
    }

    ImGui::TreePop();
}

void drawHierarchyCollectionControls(SceneDocument& document, std::string& collectionFilter) {
    std::vector<HierarchyCollectionSummary> collections = collectHierarchyCollections(document.registry());
    if (collections.empty()) {
        collectionFilter.clear();
        return;
    }
    if (!collectionFilter.empty() && std::none_of(collections.begin(), collections.end(), [&](const HierarchyCollectionSummary& collection) {
            return collection.name == collectionFilter;
        })) {
        collectionFilter.clear();
    }

    if (!ImGui::TreeNodeEx("Collections", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }

    for (const HierarchyCollectionSummary& collection : collections) {
        ImGui::PushID(collection.name.c_str());
        ImGui::TextUnformatted(collection.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%u", collection.count);
        ImGui::SameLine();
        const bool collectionFilterActive = collectionFilter == collection.name;
        if (editorIconButton("CollectionFilter", EditorGlyphIcon::Group, collectionFilterActive, ImVec2(20.0f, 18.0f))) {
            collectionFilter = collectionFilterActive ? std::string{} : collection.name;
        }
        hierarchyTooltip(collectionFilterActive ? "Clear this collection filter" : "Filter hierarchy to this collection");
        ImGui::PopID();
    }

    ImGui::TreePop();
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

        ImGui::BeginGroup();
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
            if (editorIconButton("HierarchyClearTypeFilters", EditorGlyphIcon::Exit, false, ImVec2(18.0f, 18.0f))) {
                typeFilterMask_ = 0;
            }
            hierarchyTooltip("Clear hierarchy type filters");
        }
        ImGui::EndGroup();
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("PREFAB_ASSET")) {
                requests.placeAsset = std::string(static_cast<const char*>(payload->Data));
            }
            ImGui::EndDragDropTarget();
        }

        static std::array<char, 128> filterBuffer{};
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##entityFilter", "Search...", filterBuffer.data(), filterBuffer.size());
        std::string filter = filterBuffer.data();
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        drawHierarchyLayerControls(document, requests, layerFilter_);
        drawHierarchyTagControls(document, tagFilter_);
        drawHierarchyCollectionControls(document, collectionFilter_);
        const std::vector<EntityId> filteredSelectable = collectFilteredSelectableEntities(
            registry,
            filter,
            layerFilter_,
            tagFilter_,
            collectionFilter_,
            typeFilterMask_);
        ImGui::BeginDisabled(filteredSelectable.empty());
        if (editorIconTextButton("HierarchySelectFiltered", EditorGlyphIcon::Select, "Select Filtered")) {
            selection.selectEntities(filteredSelectable);
        }
        ImGui::EndDisabled();
        hierarchyTooltip("Select all unlocked entities matching the current hierarchy filters.");
        ImGui::SameLine();
        ImGui::BeginDisabled(selection.selectionCount() == 0);
        if (editorIconTextButton("HierarchyClearSelection", EditorGlyphIcon::Exit, "Clear Selection")) {
            selection.clear();
        }
        ImGui::EndDisabled();
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
            drawEntityNode(registry, *entity, selection, requests, filter, layerFilter_, tagFilter_, collectionFilter_, typeFilterMask_, false);
        }

        if (ImGui::BeginPopupContextWindow("HierarchyEmptyContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (editorGlyphBeginMenu(EditorGlyphIcon::Add, "Create")) {
                if (editorGlyphMenuItem(EditorGlyphIcon::Entity, "Empty Entity")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Empty};
                    requests.sceneUpdate = SceneUpdateKind::None;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Camera, "Camera")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Camera};
                    requests.sceneUpdate = SceneUpdateKind::CameraOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Point Light")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Light};
                    requests.sceneUpdate = SceneUpdateKind::LightOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Sun, "Sun")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Sun};
                    requests.sceneUpdate = SceneUpdateKind::LightOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Spot Light")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::SpotLight};
                    requests.sceneUpdate = SceneUpdateKind::LightOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Light, "Area Light")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::AreaLight};
                    requests.sceneUpdate = SceneUpdateKind::LightOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Environment, "Environment Light")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::EnvironmentLight};
                    requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Sky, "Sky Atmosphere")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::SkyAtmosphere};
                    requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Fog, "Height Fog")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::HeightFog};
                    requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Cloud, "Volumetric Cloud")) {
                    requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::VolumetricCloud};
                    requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
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

    const std::string fallbackCameraLabel = editorGlyphLabel("Camera") + "##fallbackCamera";
    if (selectableHierarchyGlyph(fallbackCameraLabel.c_str(), selection.is(EditorSelectionKind::Camera), EditorGlyphIcon::Camera)) {
        selection.selectCamera();
    }

    const MeshParamsUniform& params = state.renderer.scene().meshParams();
    const std::string fallbackLightsLabel = editorGlyphLabel("Lights") + "##fallbackLights";
    if (treeNodeHierarchyGlyph(fallbackLightsLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen, EditorGlyphIcon::Light)) {
        for (uint32_t i = 0; i < params.lightCount; ++i) {
            std::string label = editorGlyphLabel("Emissive Light " + std::to_string(i));
            label += "##fallbackLight" + std::to_string(i);
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
        const std::string importedSceneLabel = editorGlyphLabel("glTF Scene") + "##ImportedSceneRoot";
        if (treeNodeHierarchyGlyph(importedSceneLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen, EditorGlyphIcon::SceneFile)) {
            for (uint32_t root : state.importedScene->rootNodes) {
                drawImportedNode(*state.importedScene, root, selection, 1);
            }
            ImGui::TreePop();
        }
    } else {
        const std::string cornellFallbackLabel = editorGlyphLabel("Cornell Fallback") + "##CornellFallbackRoot";
        if (treeNodeHierarchyGlyph(cornellFallbackLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen, EditorGlyphIcon::SceneFile)) {
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
                std::string label = editorGlyphLabel(objects[i]);
                label += "##fallbackObject" + std::to_string(i);
                if (selectableHierarchyGlyph(label.c_str(), selection.is(EditorSelectionKind::Object) && selection.index() == i, lightObject ? EditorGlyphIcon::Light : EditorGlyphIcon::Model, 1)) {
                    selection.selectObject(i);
                }
            }
            for (uint32_t i = 0; i < params.sphereCount; ++i) {
                std::string label = editorGlyphLabel("Sphere " + std::to_string(i));
                label += "##fallbackSphere" + std::to_string(i);
                const uint32_t objectId = 1000u + i;
                if (selectableHierarchyGlyph(label.c_str(), selection.is(EditorSelectionKind::Object) && selection.index() == objectId, EditorGlyphIcon::Model, 1)) {
                    selection.selectObject(objectId);
                }
            }
            ImGui::TreePop();
        }
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

bool SceneHierarchyPanel::entityContainsFilter(const SceneRegistry& registry, const Entity& entity, const std::string& filter, const std::string& layerFilter, const std::string& tagFilter, const std::string& collectionFilter, uint32_t typeFilterMask) const {
    if (entityMatchesHierarchyFilters(entity, filter, layerFilter, tagFilter, collectionFilter, typeFilterMask)) {
        return true;
    }
    for (EntityId childId : entity.children) {
        if (const Entity* child = registry.entity(childId)) {
            if (entityContainsFilter(registry, *child, filter, layerFilter, tagFilter, collectionFilter, typeFilterMask)) {
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
    const std::string& layerFilter,
    const std::string& tagFilter,
    const std::string& collectionFilter,
    uint32_t typeFilterMask,
    bool ancestorMatchesFilter,
    int depth) {
    const bool selfMatchesFilter = entityMatchesHierarchyFilters(entity, filter, layerFilter, tagFilter, collectionFilter, typeFilterMask);
    if (!ancestorMatchesFilter && !entityContainsFilter(registry, entity, filter, layerFilter, tagFilter, collectionFilter, typeFilterMask)) {
        return;
    }
    const EntityId selected = selection.entityId();
    const bool containsSelection = entityContainsSelection(registry, entity, selected);
    const bool revealPending = pendingRevealSelection_.valid() && containsSelection;
    const bool forceOpenForReveal =
        (containsSelection && entity.id != selected) ||
        (revealPending && entity.id == selected && !entity.children.empty());

    std::string label = editorGlyphLabel(entity.name.empty() ? "Entity" : entity.name);
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
    const bool entitySelected = selection.isSelected(entity.id);
    if (entitySelected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    if (forceOpenForReveal) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    editorDrawPreRowBand(EditorUiMetric::hierarchyRowHeight);
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
    const bool rowItemClicked = ImGui::IsItemClicked();
    const bool rowItemToggledOpen = ImGui::IsItemToggledOpen();
    auto drawRowControls = [&]() -> bool {
        const ImVec2 iconButtonSize(20.0f, 20.0f);
        const float controlY = rowMin.y + std::max(0.0f, (rowMax.y - rowMin.y - iconButtonSize.y) * 0.5f);
        const float lockWidth = iconButtonSize.x;
        const float eyeWidth = iconButtonSize.x;
        const float gap = 3.0f;
        const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const bool rowHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax, true);
        const bool showLockControl = entity.locked || rowHovered || entitySelected;
        const ImVec2 restoreCursor = ImGui::GetCursorScreenPos();
        bool activated = false;
        ImGui::PushID(static_cast<int>(entity.id.index));
        if (showLockControl) {
            ImGui::SetCursorScreenPos(ImVec2(rightEdge - lockWidth - eyeWidth - gap, controlY));
            if (hierarchyRowIconButton("lock", entity.locked ? EditorGlyphIcon::Lock : EditorGlyphIcon::Unlock, true, !entity.locked, iconButtonSize)) {
                requests.setEntityLocked = EditorEntityBoolChange{.entity = entity.id, .value = !entity.locked};
                activated = true;
            }
            hierarchyTooltip(entity.locked ? "Locked" : "Unlocked");
        }
        ImGui::SetCursorScreenPos(ImVec2(rightEdge - eyeWidth, controlY));
        if (hierarchyRowIconButton("visible", entity.visible ? EditorGlyphIcon::EyeVisible : EditorGlyphIcon::EyeHidden, true, !entity.visible, iconButtonSize)) {
            requests.setEntityVisibility = EditorEntityBoolChange{.entity = entity.id, .value = !entity.visible};
            activated = true;
        }
        hierarchyTooltip(entity.visible ? "Visible" : "Hidden");
        ImGui::PopID();
        ImGui::SetCursorScreenPos(restoreCursor);
        return activated;
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
    if (ImGui::BeginPopupContextItem()) {
        if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Duplicate", !entity.locked)) {
            requests.duplicateEntity = entity.id;
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Rename", !entity.locked)) {
            renameTarget_ = entity.id;
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete", !entity.locked)) {
            requests.deleteEntities = selection.selectedEntitiesOr(entity.id);
        }
        ImGui::Separator();
        if (editorGlyphMenuItem(entity.visible ? EditorGlyphIcon::EyeHidden : EditorGlyphIcon::EyeVisible, entity.visible ? "Hide" : "Show")) {
            requests.setEntityVisibility = EditorEntityBoolChange{.entity = entity.id, .value = !entity.visible};
        }
        if (editorGlyphMenuItem(entity.locked ? EditorGlyphIcon::Unlock : EditorGlyphIcon::Lock, entity.locked ? "Unlock" : "Lock")) {
            requests.setEntityLocked = EditorEntityBoolChange{.entity = entity.id, .value = !entity.locked};
        }
        ImGui::Separator();
        if (editorGlyphBeginMenu(EditorGlyphIcon::Add, "Create Child")) {
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
            if (editorGlyphMenuItem(EditorGlyphIcon::Sun, "Sun")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::Sun, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::LightOnly;
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
                requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
            }
            if (editorGlyphMenuItem(EditorGlyphIcon::Fog, "Height Fog")) {
                requests.createEntity = EditorEntityCreateRequest{.kind = EditorEntityCreateKind::HeightFog, .parent = entity.id};
                requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
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
    const bool rowControlActivated = drawRowControls();
    if (!rowControlActivated && !entity.locked && rowItemClicked && !rowItemToggledOpen) {
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
    if (selected == entity.id && (lastScrolledSelection_ != selected || pendingRevealSelection_ == selected)) {
        ImGui::SetScrollHereY(0.25f);
        lastScrolledSelection_ = selected;
        if (pendingRevealSelection_ == selected) {
            pendingRevealSelection_ = {};
        }
    }
    if (open && !entity.children.empty()) {
        for (EntityId childId : entity.children) {
            if (Entity* child = registry.entity(childId)) {
                drawEntityNode(registry, *child, selection, requests, filter, layerFilter, tagFilter, collectionFilter, typeFilterMask, ancestorMatchesFilter || selfMatchesFilter, depth + 1);
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
    std::string label = editorGlyphLabel(node.name.empty() ? "Node " + std::to_string(nodeIndex) : node.name);
    label += "##node" + std::to_string(nodeIndex);
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
