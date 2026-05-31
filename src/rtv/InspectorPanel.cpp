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

} // namespace

void InspectorPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }

    const EditorSelectionId current = selection.current();
    const size_t selCount = selection.selectionCount();
    if (!current.valid() && selCount == 0) {
        ImGui::TextDisabled("No selection");
        ImGui::End();
        return;
    }

    if (selCount > 1) {
        ImGui::Text("%zu entities selected", selCount);
        if (ImGui::Button("Delete Selected")) {
            for (EntityId id : selection.selectedEntities()) {
                requests.deleteEntity = id;
            }
        }
        if (ImGui::Button("Clear Selection")) {
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
            requests.renameEntity = EditorEntityRenameRequest{.entity = entity->id, .name = nameBuffer.data()};
        }

        ImGui::SeparatorText("Transform");
        bool transformChanged = false;
        const Transform oldTransform = entity->transform;
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
            requests.setEntityTransform = EditorEntityTransformChange{
                .entity = entity->id,
                .oldTransform = oldTransform,
                .newTransform = entity->transform,
            };
        }

        if (entity->camera.has_value()) {
            ImGui::SeparatorText("Camera");
            if (ImGui::SmallButton("Remove Camera")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::Camera};
            }
            Camera& camera = *entity->camera;
            const Camera oldCamera = camera;
            const EntityId oldActiveCamera = document.activeCamera();
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
        }

        if (entity->light.has_value()) {
            ImGui::SeparatorText("Light");
            if (ImGui::SmallButton("Remove Light")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::Light};
            }
            Light& light = *entity->light;
            const Light oldLight = light;
            bool changed = false;
            int lightType = static_cast<int>(light.type);
            if (ImGui::Combo("Type", &lightType, "Directional\0Point\0Area\0Spot\0")) {
                light.type = static_cast<LightType>(lightType);
                if (light.type == LightType::Directional &&
                    (oldLight.type != LightType::Directional || light.sizeOrRadius > 0.08f || light.sizeOrRadius <= 0.0f)) {
                    light.sizeOrRadius = 0.00465f;
                }
                changed = true;
            }
            changed |= ImGui::Checkbox("Enabled", &light.enabled);
            changed |= ImGui::ColorEdit3("Color", glm::value_ptr(light.color));
            changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100000.0f, "%.2f");
            if (light.type == LightType::Directional) {
                changed |= ImGui::DragFloat("Angular Radius", &light.sizeOrRadius, 0.0001f, 0.0001f, 0.08f, "%.5f rad");
            } else if (light.type == LightType::Spot) {
                changed |= ImGui::DragFloat("Range", &light.sizeOrRadius, 0.02f, 0.0f, 1000.0f, "%.3f");
                changed |= ImGui::DragFloat("Inner Cone", &light.innerConeRadians, 0.01f, 0.0f, 3.14159f, "%.2f rad");
                changed |= ImGui::DragFloat("Outer Cone", &light.outerConeRadians, 0.01f, light.innerConeRadians, 3.14159f, "%.2f rad");
            } else {
                changed |= ImGui::DragFloat("Size / Radius", &light.sizeOrRadius, 0.02f, 0.0f, 1000.0f, "%.3f");
            }
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
            ImGui::SeparatorText("Primary Sun");
            if (ImGui::SmallButton("Remove Primary Sun")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::Sun};
            }
            Sun& sun = *entity->sun;
            const Sun oldSun = sun;
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled", &sun.enabled);
            changed |= ImGui::DragFloat("Illuminance", &sun.illuminanceLux, 100.0f, 0.0f, 200000.0f, "%.0f lux");
            changed |= ImGui::DragFloat("Angular Radius", &sun.angularRadiusRadians, 0.0001f, 0.0001f, 0.08f, "%.5f rad");
            changed |= ImGui::DragFloat("Color Temperature", &sun.colorTemperatureKelvin, 10.0f, 1000.0f, 40000.0f, "%.0f K");
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
        }

        if (entity->meshRenderer.has_value()) {
            ImGui::SeparatorText("Mesh Renderer");
            if (ImGui::SmallButton("Remove Mesh Renderer")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::MeshRenderer};
            }
            MeshRenderer& renderer = *entity->meshRenderer;
            ensureMaterialSlots(renderer, state.assets);
            const MeshRenderer oldRenderer = renderer;
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
                    ImGui::SeparatorText("Material");
                    ImGui::Text("Material: %s", edited.name.empty() ? "(unnamed)" : edited.name.c_str());
                    materialChanged |= ImGui::ColorEdit4("Base Color", glm::value_ptr(edited.baseColorFactor));
                    materialChanged |= ImGui::SliderFloat("Metallic", &edited.metallicFactor, 0.0f, 1.0f, "%.3f");
                    materialChanged |= ImGui::SliderFloat("Roughness", &edited.roughnessFactor, 0.0f, 1.0f, "%.3f");
                    materialChanged |= ImGui::ColorEdit3("Emissive", glm::value_ptr(edited.emissiveFactor));
                    int alphaMode = static_cast<int>(edited.alphaMode);
                    if (ImGui::Combo("Alpha Mode", &alphaMode, "Opaque\0Mask\0Blend\0")) {
                        edited.alphaMode = static_cast<uint32_t>(alphaMode);
                        materialChanged = true;
                    }
                    bool doubleSided = edited.doubleSided != 0;
                    if (ImGui::Checkbox("Double Sided", &doubleSided)) {
                        edited.doubleSided = doubleSided ? 1u : 0u;
                        materialChanged = true;
                    }
                    materialChanged |= ImGui::SliderFloat("Alpha Cutoff", &edited.alphaCutoff, 0.0f, 1.0f, "%.3f");
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
            ImGui::SeparatorText("Environment Light");
            if (ImGui::SmallButton("Remove Environment Light")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::EnvironmentLight};
            }
            const SceneDocument before = document;
            EnvironmentLight& component = *entity->environmentLight;
            bool changed = false;
            changed |= ImGui::Checkbox("Environment Enabled", &component.enabled);
            changed |= ImGui::DragFloat("Environment Intensity", &component.intensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            changed |= ImGui::DragFloat("Background Intensity", &component.backgroundIntensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            changed |= ImGui::DragFloat("Environment Rotation", &component.rotation, 0.01f, -6.28318f, 6.28318f, "%.3f");
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
            ImGui::SeparatorText("Sky Atmosphere");
            if (ImGui::SmallButton("Remove Sky Atmosphere")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::SkyAtmosphere};
            }
            const SceneDocument before = document;
            SkyAtmosphere& component = *entity->skyAtmosphere;
            bool changed = false;
            changed |= ImGui::Checkbox("Atmosphere Enabled", &component.enabled);
            changed |= ImGui::DragFloat("Rayleigh Scale Height", &component.rayleighScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
            changed |= ImGui::DragFloat("Mie Scale Height", &component.mieScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
            changed |= ImGui::SliderFloat("Mie Anisotropy", &component.mieAnisotropy, 0.0f, 0.99f, "%.3f");
            changed |= ImGui::SliderFloat("Ground Albedo", &component.groundAlbedo, 0.0f, 1.0f, "%.3f");
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
            ImGui::SeparatorText("Height Fog");
            if (ImGui::SmallButton("Remove Height Fog")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::HeightFog};
            }
            const SceneDocument before = document;
            HeightFog& component = *entity->heightFog;
            bool changed = false;
            changed |= ImGui::Checkbox("Fog Enabled", &component.enabled);
            changed |= ImGui::DragFloat("Density", &component.density, 0.001f, 0.0f, 10.0f, "%.4f");
            changed |= ImGui::DragFloat("Height Falloff", &component.heightFalloff, 0.01f, 0.0f, 10.0f, "%.3f");
            changed |= ImGui::ColorEdit3("Fog Color", glm::value_ptr(component.color));
            if (changed) {
                document.worldSettings().heightFog = entity->id;
                document.worldSettings().fogEnabled = component.enabled;
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::EnvironmentOnly, .label = "Edit Height Fog"};
            }
        }

        if (entity->volumetricCloud.has_value()) {
            ImGui::SeparatorText("Volumetric Cloud");
            if (ImGui::SmallButton("Remove Volumetric Cloud")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::VolumetricCloud};
            }
            const SceneDocument before = document;
            VolumetricCloud& component = *entity->volumetricCloud;
            bool changed = false;
            changed |= ImGui::Checkbox("Cloud Enabled", &component.enabled);
            changed |= ImGui::SliderFloat("Cloud Density", &component.density, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Coverage", &component.coverage, 0.0f, 1.0f, "%.3f");
            if (changed) {
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::EnvironmentOnly, .label = "Edit Volumetric Cloud"};
            }
        }

        if (entity->postProcessVolume.has_value()) {
            ImGui::SeparatorText("Post Process Volume");
            if (ImGui::SmallButton("Remove Post Process Volume")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::PostProcessVolume};
            }
            const SceneDocument before = document;
            PostProcessVolume& component = *entity->postProcessVolume;
            bool changed = false;
            changed |= ImGui::Checkbox("Post Process Enabled", &component.enabled);
            changed |= ImGui::Checkbox("Unbound", &component.unbound);
            changed |= ImGui::DragFloat("Priority", &component.priority, 0.1f, -100.0f, 100.0f, "%.1f");
            changed |= ImGui::DragFloat("Exposure Compensation", &component.exposureCompensation, 0.02f, -10.0f, 10.0f, "%.2f");
            changed |= ImGui::SliderFloat("PP Saturation", &component.saturation, 0.0f, 2.0f, "%.3f");
            changed |= ImGui::SliderFloat("PP Contrast", &component.contrast, 0.0f, 2.0f, "%.3f");
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
            ImGui::SeparatorText("Camera Post Process");
            if (ImGui::SmallButton("Remove Camera Post Process")) {
                requests.removeComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::CameraPostProcess};
            }
            const SceneDocument before = document;
            CameraPostProcess& component = *entity->cameraPostProcess;
            bool changed = false;
            changed |= ImGui::Checkbox("Camera PP Enabled", &component.enabled);
            changed |= ImGui::Checkbox("Override Exposure", &component.overrideExposure);
            changed |= ImGui::DragFloat("Camera Exposure Compensation", &component.exposureCompensation, 0.02f, -10.0f, 10.0f, "%.2f");
            changed |= ImGui::Checkbox("Override DOF", &component.overrideDepthOfField);
            changed |= ImGui::DragFloat("Camera DOF Aperture", &component.dofApertureRadius, 0.01f, 0.0f, 10.0f, "%.3f");
            changed |= ImGui::DragFloat("Camera Focus Distance", &component.dofFocusDistance, 0.05f, 0.01f, 10000.0f, "%.2f");
            if (changed) {
                RenderSettings& render = document.renderSettings();
                if (component.overrideExposure) {
                    render.physicalExposureCompensation = component.exposureCompensation;
                }
                if (component.overrideDepthOfField) {
                    render.dofApertureRadius = component.dofApertureRadius;
                    render.dofFocusDistance = component.dofFocusDistance;
                }
                requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::RendererSettingsOnly, .label = "Edit Camera Post Process"};
            }
        }

        ImGui::SeparatorText("Add Component");
        if (!entity->light.has_value()) {
            if (ImGui::Button("Light")) {
                requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::Light};
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
            ImGui::SameLine();
        }
        if (!entity->sun.has_value()) {
            if (ImGui::Button("Primary Sun")) {
                requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::Sun};
                requests.sceneUpdate = SceneUpdateKind::LightOnly;
            }
            ImGui::SameLine();
        }
        if (!entity->camera.has_value()) {
            if (ImGui::Button("Camera")) {
                requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::Camera};
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
            ImGui::SameLine();
        }
        if (entity->camera.has_value() && !entity->cameraPostProcess.has_value()) {
            if (ImGui::Button("Camera Post Process")) {
                requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::CameraPostProcess};
                requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
            }
            ImGui::SameLine();
        }
        if (!entity->meshRenderer.has_value()) {
            if (ImGui::Button("Mesh Renderer")) {
                requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::MeshRenderer};
                requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
            }
        }
        if (!entity->environmentLight.has_value() && ImGui::Button("Environment Light")) {
            requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::EnvironmentLight};
            requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
        }
        ImGui::SameLine();
        if (!entity->skyAtmosphere.has_value() && ImGui::Button("Sky Atmosphere")) {
            requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::SkyAtmosphere};
            requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
        }
        ImGui::SameLine();
        if (!entity->heightFog.has_value() && ImGui::Button("Height Fog")) {
            requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::HeightFog};
            requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
        }
        if (!entity->volumetricCloud.has_value() && ImGui::Button("Volumetric Cloud")) {
            requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::VolumetricCloud};
            requests.sceneUpdate = SceneUpdateKind::EnvironmentOnly;
        }
        ImGui::SameLine();
        if (!entity->postProcessVolume.has_value() && ImGui::Button("Post Process Volume")) {
            requests.addComponent = EditorComponentRequest{.entity = entity->id, .kind = EditorComponentKind::PostProcessVolume};
            requests.sceneUpdate = SceneUpdateKind::RendererSettingsOnly;
        }
        if (entity->light.has_value() || entity->sun.has_value() || entity->camera.has_value() || entity->meshRenderer.has_value() ||
            entity->environmentLight.has_value() || entity->skyAtmosphere.has_value() || entity->heightFog.has_value() ||
            entity->volumetricCloud.has_value() || entity->postProcessVolume.has_value() || entity->cameraPostProcess.has_value()) {
            ImGui::TextDisabled("All components attached");
        }

        ImGui::Separator();
        if (ImGui::Button("Duplicate Entity")) {
            requests.duplicateEntity = entity->id;
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Entity")) {
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
