#include "rtv/MaterialEditorPanel.h"

#include "rtv/AssetManager.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <array>
#include <algorithm>

namespace rtv {

namespace {

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

struct ConductorPreset {
    const char* name = "";
    glm::vec3 eta{0.0f};
    glm::vec3 k{0.0f};
};

const std::array<ConductorPreset, 4> conductorPresets{{
    {"Gold", {0.17f, 0.35f, 1.50f}, {3.14f, 2.70f, 1.94f}},
    {"Copper", {0.27f, 0.68f, 1.32f}, {3.61f, 2.62f, 2.29f}},
    {"Aluminum", {1.35f, 0.97f, 0.62f}, {7.58f, 6.40f, 5.30f}},
    {"Silver", {0.16f, 0.14f, 0.13f}, {4.10f, 3.10f, 2.30f}},
}};

glm::vec3 conductorF0(const glm::vec3& eta, const glm::vec3& k) {
    const glm::vec3 etaMinusOne = eta - glm::vec3{1.0f};
    const glm::vec3 etaPlusOne = eta + glm::vec3{1.0f};
    const glm::vec3 k2 = k * k;
    const glm::vec3 numerator = etaMinusOne * etaMinusOne + k2;
    const glm::vec3 denominator = etaPlusOne * etaPlusOne + k2;
    return glm::clamp(numerator / glm::max(denominator, glm::vec3{1.0e-6f}), glm::vec3{0.0f}, glm::vec3{1.0f});
}

glm::vec3 approximateConductorKFromF0(const glm::vec3& f0) {
    const glm::vec3 clamped = glm::clamp(f0, glm::vec3{0.02f}, glm::vec3{0.98f});
    return 2.0f * glm::sqrt(clamped / (glm::vec3{1.0f} - clamped));
}

void initializeConductorFromBaseColor(MaterialAsset& material) {
    material.conductorEta = glm::vec3{1.0f};
    material.conductorK = approximateConductorKFromF0(glm::vec3{material.baseColorFactor});
}

void applyConductorPreset(MaterialAsset& material, const ConductorPreset& preset) {
    material.useConductorOptics = 1u;
    material.conductorEta = preset.eta;
    material.conductorK = preset.k;
    material.metallicFactor = 1.0f;
    material.baseColorFactor = glm::vec4{conductorF0(preset.eta, preset.k), material.baseColorFactor.a};
    material.roughnessFactor = std::min(material.roughnessFactor, 0.35f);
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
    {
        ImGui::Button("Drag Material to Entity");
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("MATERIAL", &materialId, sizeof(materialId));
            ImGui::Text("Material %u", materialId);
            ImGui::EndDragDropSource();
        }
    }
    changed |= ImGui::ColorEdit4("Base Color", glm::value_ptr(edited.baseColorFactor));
    changed |= ImGui::SliderFloat("Metallic", &edited.metallicFactor, 0.0f, 1.0f, "%.3f");
    tooltip("0 = dielectric, 1 = metal. Most materials are near one endpoint.");
    changed |= ImGui::SliderFloat("Roughness", &edited.roughnessFactor, 0.0f, 1.0f, "%.3f");
    tooltip("0 = mirror-like, 1 = diffuse.");
    ImGui::SeparatorText("Conductor Optics");
    bool useConductorOptics = edited.useConductorOptics != 0u;
    if (ImGui::Checkbox("Eta/K Fresnel", &useConductorOptics)) {
        edited.useConductorOptics = useConductorOptics ? 1u : 0u;
        if (useConductorOptics &&
            edited.conductorEta.x + edited.conductorEta.y + edited.conductorEta.z +
                edited.conductorK.x + edited.conductorK.y + edited.conductorK.z <= 1.0e-4f) {
            initializeConductorFromBaseColor(edited);
        }
        changed = true;
    }
    tooltip("Use measured conductor eta/k values for metallic Fresnel. Disable to use base-color metallic fallback.");
    if (edited.useConductorOptics != 0u) {
        changed |= ImGui::InputFloat3("Eta", glm::value_ptr(edited.conductorEta), "%.3f");
        changed |= ImGui::InputFloat3("K", glm::value_ptr(edited.conductorK), "%.3f");
        if (ImGui::Button("From Base Color")) {
            initializeConductorFromBaseColor(edited);
            edited.metallicFactor = 1.0f;
            changed = true;
        }
    }
    for (const ConductorPreset& preset : conductorPresets) {
        if (ImGui::Button(preset.name)) {
            applyConductorPreset(edited, preset);
            changed = true;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
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
