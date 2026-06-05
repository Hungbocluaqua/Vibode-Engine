#include "rtv/SceneToGpuSceneBuilder.h"

#include "rtv/AssetManager.h"
#include "rtv/SceneRenderSettingsSync.h"
#include "rtv/SunController.h"

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

[[nodiscard]] MaterialAssetHandle effectivePrimitiveMaterial(
    const MeshRenderer& renderer,
    const MeshPrimitiveAsset& primitive,
    size_t primitiveIndex) {
    MaterialAssetHandle material = primitive.material;
    const bool hasSlot = primitiveIndex < renderer.materialSlots.size();
    const bool hasOverride = hasSlot && renderer.materialSlots[primitiveIndex].overrideMaterial.has_value();
    if (hasSlot) {
        material = renderer.materialSlots[primitiveIndex].resolvedMaterial();
    }
    if (!hasOverride && renderer.activeMaterialVariantIndex != UINT32_MAX) {
        const auto variantIt = std::find_if(
            primitive.materialVariants.begin(),
            primitive.materialVariants.end(),
            [&](const MeshPrimitiveAsset::MaterialVariant& variant) {
                return variant.variantIndex == renderer.activeMaterialVariantIndex;
            });
        if (variantIt != primitive.materialVariants.end() && variantIt->material.valid()) {
            material = variantIt->material;
        }
    }
    return material;
}

void appendSceneMaterial(SceneAsset& scene, const AssetManager* assets, MaterialAssetHandle material);

void applyRendererMaterialBindings(
    SceneGpuBuildResult& result,
    const std::vector<const Entity*>& entities,
    AssetManager* assets) {
    if (assets == nullptr) {
        return;
    }

    const size_t count = std::min(result.sceneAsset.nodes.size(), entities.size());
    for (size_t nodeIndex = 0; nodeIndex < count; ++nodeIndex) {
        const Entity* entity = entities[nodeIndex];
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            continue;
        }

        const MeshRenderer& renderer = *entity->meshRenderer;
        const MeshAsset* sourceMesh = assets->mesh(renderer.mesh);
        if (sourceMesh == nullptr || sourceMesh->primitives.empty()) {
            continue;
        }

        bool needsRuntimeMesh = false;
        std::vector<MaterialAssetHandle> effectiveMaterials;
        effectiveMaterials.reserve(sourceMesh->primitives.size());
        for (size_t primitiveIndex = 0; primitiveIndex < sourceMesh->primitives.size(); ++primitiveIndex) {
            const MeshPrimitiveAsset& primitive = sourceMesh->primitives[primitiveIndex];
            const MaterialAssetHandle material = effectivePrimitiveMaterial(renderer, primitive, primitiveIndex);
            effectiveMaterials.push_back(material);
            if (material.valid() && material.index != primitive.material.index) {
                needsRuntimeMesh = true;
            }
            appendSceneMaterial(result.sceneAsset, assets, material);
        }

        if (!needsRuntimeMesh) {
            continue;
        }

        MeshAsset runtimeMesh = *sourceMesh;
        runtimeMesh.name = sourceMesh->name.empty()
            ? "runtime material variant mesh"
            : sourceMesh->name + " (runtime material variant)";
        for (size_t primitiveIndex = 0; primitiveIndex < runtimeMesh.primitives.size(); ++primitiveIndex) {
            MeshPrimitiveAsset& primitive = runtimeMesh.primitives[primitiveIndex];
            const MaterialAssetHandle material = effectiveMaterials[primitiveIndex];
            if (material.valid()) {
                primitive.material = material;
                updatePrimitiveAlphaClassification(primitive, assets->material(material));
            }
        }

        const MeshAssetHandle runtimeHandle = assets->addMesh(std::move(runtimeMesh));
        result.sceneAsset.meshes.push_back(runtimeHandle);
        result.sceneAsset.nodes[nodeIndex].mesh = runtimeHandle;
    }
}

void appendSceneTexture(SceneAsset& scene, const AssetManager* assets, TextureAssetHandle texture) {
    if (texture.valid() && (assets == nullptr || assets->texture(texture) != nullptr)) {
        scene.textures.push_back(texture);
    }
}

void appendMaterialTextures(SceneAsset& scene, const AssetManager* assets, const MaterialAsset& material) {
    appendSceneTexture(scene, assets, material.baseColorTexture);
    appendSceneTexture(scene, assets, material.normalTexture);
    appendSceneTexture(scene, assets, material.metallicRoughnessTexture);
    appendSceneTexture(scene, assets, material.emissiveTexture);
    appendSceneTexture(scene, assets, material.clearcoatTexture);
    appendSceneTexture(scene, assets, material.clearcoatRoughnessTexture);
    appendSceneTexture(scene, assets, material.clearcoatNormalTexture);
    appendSceneTexture(scene, assets, material.transmissionTexture);
    appendSceneTexture(scene, assets, material.volumeThicknessTexture);
    appendSceneTexture(scene, assets, material.specularTexture);
    appendSceneTexture(scene, assets, material.specularColorTexture);
    appendSceneTexture(scene, assets, material.sheenColorTexture);
    appendSceneTexture(scene, assets, material.sheenRoughnessTexture);
    appendSceneTexture(scene, assets, material.iridescenceTexture);
    appendSceneTexture(scene, assets, material.iridescenceThicknessTexture);
    appendSceneTexture(scene, assets, material.anisotropyTexture);
    appendSceneTexture(scene, assets, material.occlusionTexture);
}

void appendSceneMaterial(SceneAsset& scene, const AssetManager* assets, MaterialAssetHandle material) {
    if (!material.valid() || (assets != nullptr && assets->material(material) == nullptr)) {
        return;
    }
    scene.materials.push_back(material);
}

void deduplicateSceneHandles(SceneAsset& scene) {
    std::sort(scene.textures.begin(), scene.textures.end(), [](TextureAssetHandle a, TextureAssetHandle b) { return a.index < b.index; });
    scene.textures.erase(
        std::unique(scene.textures.begin(), scene.textures.end(), [](TextureAssetHandle a, TextureAssetHandle b) { return a.index == b.index; }),
        scene.textures.end());

    std::sort(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index < b.index; });
    scene.materials.erase(
        std::unique(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index == b.index; }),
        scene.materials.end());

    std::sort(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index < b.index; });
    scene.meshes.erase(
        std::unique(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index == b.index; }),
        scene.meshes.end());
}

void collectSceneMaterialTextures(SceneAsset& scene, const AssetManager* assets) {
    if (assets == nullptr) {
        deduplicateSceneHandles(scene);
        return;
    }
    for (MaterialAssetHandle material : scene.materials) {
        if (const MaterialAsset* asset = assets->material(material)) {
            appendMaterialTextures(scene, assets, *asset);
        }
    }
    deduplicateSceneHandles(scene);
}

} // namespace

SceneGpuBuildResult SceneToGpuSceneBuilder::build(
    const SceneDocument& document,
    AssetManager* assets,
    const RendererSettings& currentSettings) const {
    SceneGpuBuildResult result;
    result.updateKind = document.pendingUpdate();
    result.sceneAsset = document.toSceneAsset();
    result.rendererSettings = currentSettings;

    const std::vector<const Entity*> entities = document.registry().entities();
    applyRendererMaterialBindings(result, entities, assets);
    collectSceneMaterialTextures(result.sceneAsset, assets);

    const RenderSettings& render = document.renderSettings();
    const Environment& environment = document.environment();
    result.rendererSettings.renderPreset = render.renderPreset;
    result.rendererSettings.pathTracingEnabled = render.pathTracingEnabled;
    result.rendererSettings.cameraJitterEnabled = render.cameraJitterEnabled;
    result.rendererSettings.directLightingEnabled = render.directLightingEnabled;
    result.rendererSettings.maxBounces = render.maxBounces;
    result.rendererSettings.environmentDirectSamples = render.environmentDirectSamples;
    result.rendererSettings.toneMapper = render.toneMapper;
    result.rendererSettings.exposure = render.exposure;
    result.rendererSettings.gamma = render.gamma;
    result.rendererSettings.contrast = render.contrast;
    result.rendererSettings.saturation = render.saturation;
    result.rendererSettings.brightness = render.brightness;
    result.rendererSettings.whitePoint = render.whitePoint;
    result.rendererSettings.autoExposureEnabled = render.autoExposureEnabled;
    result.rendererSettings.targetLuminance = render.targetLuminance;
    result.rendererSettings.minExposure = render.minExposure;
    result.rendererSettings.maxExposure = render.maxExposure;
    result.rendererSettings.adaptationSpeed = render.adaptationSpeed;
    result.rendererSettings.histogramMinLogLuminance = render.histogramMinLogLuminance;
    result.rendererSettings.histogramMaxLogLuminance = render.histogramMaxLogLuminance;
    result.rendererSettings.histogramLowPercentile = render.histogramLowPercentile;
    result.rendererSettings.histogramHighPercentile = render.histogramHighPercentile;
    result.rendererSettings.histogramTargetPercentile = render.histogramTargetPercentile;
    result.rendererSettings.skyIntensity = render.skyIntensity;
    result.rendererSettings.rayleighScaleHeight = render.rayleighScaleHeight;
    result.rendererSettings.mieScaleHeight = render.mieScaleHeight;
    result.rendererSettings.mieAnisotropy = render.mieAnisotropy;
    result.rendererSettings.groundAlbedo = render.groundAlbedo;
    result.rendererSettings.indirectStrength = render.indirectStrength;
    result.rendererSettings.restirMode = render.restirMode;
    result.rendererSettings.restirGiEnabled = render.restirGiEnabled;
    result.rendererSettings.denoiserEnabled = render.denoiserEnabled;
    result.rendererSettings.denoiserBackend = render.denoiserBackend;
    result.rendererSettings.denoiseWhileMoving = render.denoiseWhileMoving;
    result.rendererSettings.samplesPerPixel = render.samplesPerPixel;
    result.rendererSettings.limitSamplesPerPixel = render.limitSamplesPerPixel;
    result.rendererSettings.atrousIterations = render.atrousIterations;
    result.rendererSettings.denoiserStrength = render.denoiserStrength;
    result.rendererSettings.denoiserMaxHistoryLength = render.denoiserMaxHistoryLength;
    result.rendererSettings.momentValidityThreshold = render.momentValidityThreshold;
    result.rendererSettings.taaEnabled = render.taaEnabled;
    result.rendererSettings.temporalUpscaler = render.temporalUpscaler;
    result.rendererSettings.dlssFrameGenerationEnabled = render.dlssFrameGenerationEnabled;
    result.rendererSettings.dlssRayReconstructionEnabled = render.dlssRayReconstructionEnabled;
    result.rendererSettings.dlssSharpeningStrength = render.dlssSharpeningStrength;
    result.rendererSettings.taaFeedback = render.taaFeedback;
    result.rendererSettings.taaMotionFeedback = render.taaMotionFeedback;
    result.rendererSettings.taaReactiveFeedback = render.taaReactiveFeedback;
    result.rendererSettings.taaSharpeningStrength = render.taaSharpeningStrength;
    result.rendererSettings.debugView = render.debugView;
    result.rendererSettings.shadowRayBias = render.shadowRayBias;
    result.rendererSettings.shadowDistanceBias = render.shadowDistanceBias;
    result.rendererSettings.fireflyClamp = render.fireflyClamp;
    result.rendererSettings.restirGiTemporalMaxAge = render.restirGiTemporalMaxAge;
    result.rendererSettings.restirGiSpatialRounds = render.restirGiSpatialRounds;
    result.rendererSettings.restirGiSpatialRadius = render.restirGiSpatialRadius;
    result.rendererSettings.restirGiDepthThresholdScale = render.restirGiDepthThresholdScale;
    result.rendererSettings.restirGiSpatialCompatibilityThreshold = render.restirGiSpatialCompatibilityThreshold;
    result.rendererSettings.restirGiHalfResolution = render.restirGiHalfResolution;
    result.rendererSettings.restirGiVisibilityRayBudget = render.restirGiVisibilityRayBudget;
    result.rendererSettings.restirGiFinalStabilizationEnabled = render.restirGiFinalStabilizationEnabled;
    result.rendererSettings.adaptiveQualityMode = render.adaptiveQualityMode;
    result.rendererSettings.adaptiveGpuFrameTargetMs = render.adaptiveGpuFrameTargetMs;
    result.rendererSettings.materialTextureAnisotropy = render.materialTextureAnisotropy;
    result.rendererSettings.specularAaEnabled = render.specularAaEnabled;
    result.rendererSettings.opacityMicromapsEnabled = render.opacityMicromapsEnabled;
    result.rendererSettings.usePhysicalCamera = render.usePhysicalCamera;
    result.rendererSettings.physicalAperture = render.physicalAperture;
    result.rendererSettings.physicalShutterSeconds = render.physicalShutterSeconds;
    result.rendererSettings.physicalIso = render.physicalIso;
    result.rendererSettings.physicalExposureCompensation = render.physicalExposureCompensation;
    result.rendererSettings.dofApertureRadius = render.dofApertureRadius;
    result.rendererSettings.dofFocusDistance = render.dofFocusDistance;
    result.rendererSettings.dofBladeCount = render.dofBladeCount;
    result.rendererSettings.dofBokehRotation = render.dofBokehRotation;
    result.rendererSettings.motionBlurEnabled = render.motionBlurEnabled;
    result.rendererSettings.motionBlurShutterOpen = render.motionBlurShutterOpen;
    result.rendererSettings.motionBlurShutterClose = render.motionBlurShutterClose;
    result.rendererSettings.homogeneousVolumeEnabled = render.homogeneousVolumeEnabled;
    result.rendererSettings.homogeneousVolumeScattering = render.homogeneousVolumeScattering;
    result.rendererSettings.homogeneousVolumeAbsorption = render.homogeneousVolumeAbsorption;
    result.rendererSettings.homogeneousVolumeAnisotropy = render.homogeneousVolumeAnisotropy;
    result.rendererSettings.mneeCausticsEnabled = render.mneeCausticsEnabled;
    result.rendererSettings.environmentEnabled = environment.enabled;
    result.rendererSettings.environmentIntensity = environment.intensity;
    result.rendererSettings.environmentRotation = environment.rotation;
    result.rendererSettings.environmentBackgroundIntensity = environment.backgroundIntensity;
    result.rendererSettings.renderResolutionScale = render.resolutionScale;
    SunController::applyToRendererSettings(document, result.rendererSettings);
    applySceneWorldComponentsToRendererSettings(document, result.rendererSettings);

    auto appendInstanceEntity = [&](uint32_t nodeIndex) {
        if (nodeIndex < result.sceneAsset.nodes.size() &&
            nodeIndex < entities.size() &&
            result.sceneAsset.nodes[nodeIndex].mesh.valid()) {
            result.instanceEntities.push_back(entities[nodeIndex]->id);
        }
    };
    auto visitNode = [&](auto&& self, uint32_t nodeIndex) -> void {
        if (nodeIndex >= result.sceneAsset.nodes.size()) {
            return;
        }
        appendInstanceEntity(nodeIndex);
        for (uint32_t child : result.sceneAsset.nodes[nodeIndex].children) {
            self(self, child);
        }
    };
    if (!result.sceneAsset.rootNodes.empty()) {
        for (uint32_t root : result.sceneAsset.rootNodes) {
            visitNode(visitNode, root);
        }
    } else {
        for (uint32_t i = 0; i < result.sceneAsset.nodes.size(); ++i) {
            if (result.sceneAsset.nodes[i].parent < 0) {
                visitNode(visitNode, i);
            }
        }
    }

    result.accumulationReason = accumulationReasonFor(result.updateKind);
    result.requiresRendererRebuild = result.updateKind == SceneUpdateKind::TopologyChanged;
    return result;
}

AccumulationResetReason SceneToGpuSceneBuilder::accumulationReasonFor(SceneUpdateKind kind) {
    switch (kind) {
    case SceneUpdateKind::None: return AccumulationResetReason::Manual;
    case SceneUpdateKind::MaterialOnly: return AccumulationResetReason::MaterialChanged;
    case SceneUpdateKind::TransformOnly: return AccumulationResetReason::SceneChanged;
    case SceneUpdateKind::LightOnly: return AccumulationResetReason::LightingChanged;
    case SceneUpdateKind::EnvironmentOnly: return AccumulationResetReason::EnvironmentChanged;
    case SceneUpdateKind::CameraOnly: return AccumulationResetReason::CameraMoved;
    case SceneUpdateKind::VisibilityOnly: return AccumulationResetReason::Manual;
    case SceneUpdateKind::TopologyChanged: return AccumulationResetReason::SceneChanged;
    case SceneUpdateKind::RendererSettingsOnly: return AccumulationResetReason::RenderSettingsChanged;
    }
    return AccumulationResetReason::Manual;
}

} // namespace rtv
