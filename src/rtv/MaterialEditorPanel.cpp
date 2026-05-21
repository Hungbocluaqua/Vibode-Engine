#include "rtv/MaterialEditorPanel.h"

#include "rtv/AssetManager.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

namespace rtv {

namespace {

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

uint32_t materialIdForSelection(const EditorRuntimeState& state, const EditorSelection& selection) {
    const EditorSelectionId selected = selection.current();
    if (selected.kind == EditorSelectionKind::Material) {
        return selected.index;
    }
    if (state.sceneDocument != nullptr && selected.entity.valid()) {
        const Entity* entity = state.sceneDocument->registry().entity(selected.entity);
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            return UINT32_MAX;
        }
        const MeshRenderer& renderer = *entity->meshRenderer;
        if (!renderer.materialSlots.empty()) {
            const MaterialAssetHandle material = renderer.materialSlots.front().resolvedMaterial();
            return material.valid() ? material.index : UINT32_MAX;
        }
        if (state.assets != nullptr) {
            const MeshAsset* mesh = state.assets->mesh(renderer.mesh);
            if (mesh != nullptr && !mesh->primitives.empty()) {
                return mesh->primitives.front().material.index;
            }
        }
        return UINT32_MAX;
    }
    if (selected.kind != EditorSelectionKind::Object || state.importedScene == nullptr || state.assets == nullptr) {
        return UINT32_MAX;
    }
    if (selected.index >= state.importedScene->nodes.size()) {
        return UINT32_MAX;
    }
    const SceneNodeAsset& node = state.importedScene->nodes[selected.index];
    const MeshAsset* mesh = state.assets->mesh(node.mesh);
    if (mesh == nullptr || mesh->primitives.empty()) {
        return UINT32_MAX;
    }
    return mesh->primitives.front().material.index;
}

} // namespace

void MaterialEditorPanel::draw(const EditorRuntimeState& state, const EditorSelection& selection, EditorRequests& requests) {
    if (!ImGui::Begin("Material Editor")) {
        ImGui::End();
        return;
    }

    const uint32_t materialId = materialIdForSelection(state, selection);
    if (state.assets == nullptr || materialId == UINT32_MAX) {
        ImGui::TextDisabled("Select a material or an object with a material.");
        ImGui::End();
        return;
    }

    const MaterialAsset* source = state.assets->material(MaterialAssetHandle{materialId});
    if (source == nullptr) {
        ImGui::TextDisabled("Selected material is unavailable.");
        ImGui::End();
        return;
    }

    MaterialAsset edited = *source;
    bool changed = false;
    ImGui::Text("Material ID: %u", materialId);
    ImGui::Text("Name: %s", edited.name.empty() ? "(unnamed)" : edited.name.c_str());
    changed |= ImGui::ColorEdit4("Base Color", glm::value_ptr(edited.baseColorFactor));
    changed |= ImGui::SliderFloat("Metallic", &edited.metallicFactor, 0.0f, 1.0f, "%.3f");
    tooltip("0 = dielectric, 1 = metal. Most materials are near one endpoint.");
    changed |= ImGui::SliderFloat("Roughness", &edited.roughnessFactor, 0.0f, 1.0f, "%.3f");
    tooltip("0 = mirror-like, 1 = diffuse.");
    changed |= ImGui::ColorEdit3("Emissive", glm::value_ptr(edited.emissiveFactor));
    int alphaMode = static_cast<int>(edited.alphaMode);
    changed |= ImGui::Combo("Alpha Mode", &alphaMode, "Opaque\0Mask\0Blend\0");
    edited.alphaMode = static_cast<uint32_t>(alphaMode);
    bool doubleSided = edited.doubleSided != 0;
    if (ImGui::Checkbox("Double Sided", &doubleSided)) {
        edited.doubleSided = doubleSided ? 1u : 0u;
        changed = true;
    }
    changed |= ImGui::SliderFloat("Alpha Cutoff", &edited.alphaCutoff, 0.0f, 1.0f, "%.3f");

    ImGui::SeparatorText("Textures");
    ImGui::Text("Base color: %u", edited.baseColorTexture.index);
    ImGui::Text("Normal: %u", edited.normalTexture.index);
    ImGui::Text("Metallic roughness: %u", edited.metallicRoughnessTexture.index);
    ImGui::Text("Emissive: %u", edited.emissiveTexture.index);

    if (changed) {
        requests.materialUpdate = EditorMaterialUpdate{.materialId = materialId, .material = edited};
        requests.resetAccumulation = AccumulationResetReason::MaterialChanged;
        if (state.sceneDocument != nullptr) {
            state.sceneDocument->markDirty(SceneUpdateKind::MaterialOnly);
            requests.sceneUpdate = SceneUpdateKind::MaterialOnly;
        }
    }

    ImGui::End();
}

} // namespace rtv
