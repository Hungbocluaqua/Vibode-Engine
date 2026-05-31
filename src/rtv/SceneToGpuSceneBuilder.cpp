#include "rtv/SceneToGpuSceneBuilder.h"

#include "rtv/AssetManager.h"
#include "rtv/SunController.h"

#include <algorithm>
#include <cmath>

namespace rtv {

SceneGpuBuildResult SceneToGpuSceneBuilder::build(
    const SceneDocument& document,
    const AssetManager*,
    const RendererSettings& currentSettings) const {
    SceneGpuBuildResult result;
    result.updateKind = document.pendingUpdate();
    result.sceneAsset = document.toSceneAsset();
    result.rendererSettings = currentSettings;

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
    result.rendererSettings.denoiseWhileMoving = render.denoiseWhileMoving;
    result.rendererSettings.samplesPerPixel = render.samplesPerPixel;
    result.rendererSettings.limitSamplesPerPixel = render.limitSamplesPerPixel;
    result.rendererSettings.atrousIterations = render.atrousIterations;
    result.rendererSettings.denoiserStrength = render.denoiserStrength;
    result.rendererSettings.denoiserMaxHistoryLength = render.denoiserMaxHistoryLength;
    result.rendererSettings.momentValidityThreshold = render.momentValidityThreshold;
    result.rendererSettings.taaEnabled = render.taaEnabled;
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

    const std::vector<const Entity*> entities = document.registry().entities();
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
