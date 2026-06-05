#include "rtv/SceneRenderSettingsSync.h"

#include "rtv/Entity.h"
#include "rtv/SceneDocument.h"

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

template <typename Component>
using ComponentMember = std::optional<Component> Entity::*;

template <typename Component>
const Component* componentFromEntity(const SceneDocument& document, EntityId id, ComponentMember<Component> member) {
    const Entity* entity = document.registry().entity(id);
    if (entity == nullptr) {
        return nullptr;
    }
    const std::optional<Component>& component = entity->*member;
    return component.has_value() ? &*component : nullptr;
}

template <typename Component>
const Component* firstComponent(const SceneDocument& document, ComponentMember<Component> member) {
    for (const Entity* entity : document.registry().entities()) {
        if (entity == nullptr) {
            continue;
        }
        const std::optional<Component>& component = entity->*member;
        if (component.has_value()) {
            return &*component;
        }
    }
    return nullptr;
}

const PostProcessVolume* selectPostProcessVolume(const SceneDocument& document) {
    if (const PostProcessVolume* component = componentFromEntity(document, document.worldSettings().postProcessVolume, &Entity::postProcessVolume)) {
        return component;
    }

    const PostProcessVolume* best = nullptr;
    for (const Entity* entity : document.registry().entities()) {
        if (entity == nullptr || !entity->postProcessVolume.has_value()) {
            continue;
        }
        const PostProcessVolume& candidate = *entity->postProcessVolume;
        if (!candidate.unbound && best != nullptr) {
            continue;
        }
        if (best == nullptr || candidate.priority >= best->priority) {
            best = &candidate;
        }
    }
    return best;
}

const CameraPostProcess* selectCameraPostProcess(const SceneDocument& document) {
    if (const CameraPostProcess* component = componentFromEntity(document, document.activeCamera(), &Entity::cameraPostProcess)) {
        return component;
    }
    return firstComponent(document, &Entity::cameraPostProcess);
}

bool anyWorldComponents(const SceneDocument& document) {
    for (const Entity* entity : document.registry().entities()) {
        if (entity == nullptr) {
            continue;
        }
        if (entity->environmentLight.has_value() ||
            entity->skyAtmosphere.has_value() ||
            entity->heightFog.has_value() ||
            entity->volumetricCloud.has_value() ||
            entity->postProcessVolume.has_value() ||
            entity->cameraPostProcess.has_value()) {
            return true;
        }
    }
    return false;
}

float safeNonNegative(float value) {
    return std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
}

void applyVolumetricCloudPrototype(const VolumetricCloud& component, RendererSettings& settings) {
    const float density = safeNonNegative(component.density);
    const float coverage = std::clamp(std::isfinite(component.coverage) ? component.coverage : 0.0f, 0.0f, 1.0f);
    settings.homogeneousVolumeEnabled = component.enabled && density > 0.0f && coverage > 0.0f;
    settings.homogeneousVolumeScattering = density * coverage * 0.02f;
    settings.homogeneousVolumeAbsorption = density * (1.0f - coverage) * 0.005f;
    settings.homogeneousVolumeAnisotropy = 0.35f;
}

void clearAbsentWorldEffects(RendererSettings& settings) {
    settings.environmentEnabled = false;
    settings.environmentIntensity = 0.0f;
    settings.environmentBackgroundIntensity = 1.0f;
    settings.skyIntensity = 0.0f;
    settings.heightFogEnabled = false;
    settings.heightFogDensity = 0.0f;
    settings.homogeneousVolumeEnabled = false;
    settings.homogeneousVolumeScattering = 0.0f;
    settings.homogeneousVolumeAbsorption = 0.0f;
    settings.homogeneousVolumeAnisotropy = 0.0f;
}

} // namespace

SceneWorldComponentSelection selectSceneWorldComponents(const SceneDocument& document) {
    SceneWorldComponentSelection selection;
    selection.hasWorldComponents = anyWorldComponents(document);
    if (!selection.hasWorldComponents) {
        return selection;
    }

    selection.environmentLight = componentFromEntity(document, document.worldSettings().activeEnvironment, &Entity::environmentLight);
    if (selection.environmentLight == nullptr) {
        selection.environmentLight = firstComponent(document, &Entity::environmentLight);
    }

    selection.skyAtmosphere = componentFromEntity(document, document.worldSettings().skyAtmosphere, &Entity::skyAtmosphere);
    if (selection.skyAtmosphere == nullptr) {
        selection.skyAtmosphere = firstComponent(document, &Entity::skyAtmosphere);
    }

    selection.heightFog = componentFromEntity(document, document.worldSettings().heightFog, &Entity::heightFog);
    if (selection.heightFog == nullptr) {
        selection.heightFog = firstComponent(document, &Entity::heightFog);
    }

    selection.volumetricCloud = firstComponent(document, &Entity::volumetricCloud);
    selection.postProcessVolume = selectPostProcessVolume(document);
    selection.cameraPostProcess = selectCameraPostProcess(document);
    return selection;
}

void applySceneWorldComponentsToRendererSettings(const SceneDocument& document, RendererSettings& settings) {
    const SceneWorldComponentSelection selection = selectSceneWorldComponents(document);
    if (!selection.hasWorldComponents) {
        clearAbsentWorldEffects(settings);
        return;
    }

    if (selection.environmentLight != nullptr) {
        settings.environmentEnabled = selection.environmentLight->enabled;
        settings.environmentIntensity = safeNonNegative(selection.environmentLight->intensity);
        settings.environmentBackgroundIntensity = safeNonNegative(selection.environmentLight->backgroundIntensity);
        settings.environmentRotation = selection.environmentLight->rotation;
    } else {
        settings.environmentEnabled = false;
        settings.environmentIntensity = 0.0f;
        settings.environmentBackgroundIntensity = 1.0f;
    }

    if (selection.skyAtmosphere != nullptr) {
        settings.skyIntensity = selection.skyAtmosphere->enabled ? safeNonNegative(selection.skyAtmosphere->skyIntensity) : 0.0f;
        settings.rayleighScaleHeight = safeNonNegative(selection.skyAtmosphere->rayleighScaleHeight);
        settings.mieScaleHeight = safeNonNegative(selection.skyAtmosphere->mieScaleHeight);
        settings.mieAnisotropy = std::clamp(selection.skyAtmosphere->mieAnisotropy, 0.0f, 0.99f);
        settings.groundAlbedo = std::clamp(selection.skyAtmosphere->groundAlbedo, 0.0f, 1.0f);
    } else {
        settings.skyIntensity = 0.0f;
    }

    if (selection.heightFog != nullptr) {
        settings.heightFogEnabled = selection.heightFog->enabled && selection.heightFog->density > 0.0f;
        settings.heightFogDensity = safeNonNegative(selection.heightFog->density);
        settings.heightFogHeightFalloff = std::max(0.001f, safeNonNegative(selection.heightFog->heightFalloff));
        settings.heightFogColor = glm::max(selection.heightFog->color, glm::vec3{0.0f});
    } else {
        settings.heightFogEnabled = false;
        settings.heightFogDensity = 0.0f;
    }

    if (selection.volumetricCloud != nullptr) {
        applyVolumetricCloudPrototype(*selection.volumetricCloud, settings);
    } else {
        settings.homogeneousVolumeEnabled = false;
        settings.homogeneousVolumeScattering = 0.0f;
        settings.homogeneousVolumeAbsorption = 0.0f;
        settings.homogeneousVolumeAnisotropy = 0.0f;
    }

    if (selection.postProcessVolume != nullptr && selection.postProcessVolume->enabled) {
        settings.physicalExposureCompensation = selection.postProcessVolume->exposureCompensation;
        settings.saturation = selection.postProcessVolume->saturation;
        settings.contrast = selection.postProcessVolume->contrast;
    }

    if (selection.cameraPostProcess != nullptr && selection.cameraPostProcess->enabled) {
        if (selection.cameraPostProcess->overrideExposure) {
            settings.physicalExposureCompensation = selection.cameraPostProcess->exposureCompensation;
        }
        if (selection.cameraPostProcess->overrideDepthOfField) {
            settings.dofApertureRadius = selection.cameraPostProcess->dofApertureRadius;
            settings.dofFocusDistance = selection.cameraPostProcess->dofFocusDistance;
        }
        if (selection.cameraPostProcess->colorCorrectionEnabled) {
            settings.saturation = selection.cameraPostProcess->colorCorrectionSaturation;
            settings.contrast = selection.cameraPostProcess->colorCorrectionContrast;
        }
    }
}

void applySceneWorldComponentsToDocumentSettings(SceneDocument& document) {
    const SceneWorldComponentSelection selection = selectSceneWorldComponents(document);
    RendererSettings settings;
    settings.environmentEnabled = document.environment().enabled;
    settings.environmentIntensity = document.environment().intensity;
    settings.environmentRotation = document.environment().rotation;
    settings.environmentBackgroundIntensity = document.environment().backgroundIntensity;
    settings.skyIntensity = document.renderSettings().skyIntensity;
    settings.rayleighScaleHeight = document.renderSettings().rayleighScaleHeight;
    settings.mieScaleHeight = document.renderSettings().mieScaleHeight;
    settings.mieAnisotropy = document.renderSettings().mieAnisotropy;
    settings.groundAlbedo = document.renderSettings().groundAlbedo;
    settings.homogeneousVolumeEnabled = document.renderSettings().homogeneousVolumeEnabled;
    settings.homogeneousVolumeScattering = document.renderSettings().homogeneousVolumeScattering;
    settings.homogeneousVolumeAbsorption = document.renderSettings().homogeneousVolumeAbsorption;
    settings.homogeneousVolumeAnisotropy = document.renderSettings().homogeneousVolumeAnisotropy;
    settings.physicalExposureCompensation = document.renderSettings().physicalExposureCompensation;
    settings.dofApertureRadius = document.renderSettings().dofApertureRadius;
    settings.dofFocusDistance = document.renderSettings().dofFocusDistance;
    settings.saturation = document.renderSettings().saturation;
    settings.contrast = document.renderSettings().contrast;

    applySceneWorldComponentsToRendererSettings(document, settings);

    Environment& environment = document.environment();
    environment.enabled = settings.environmentEnabled;
    environment.intensity = settings.environmentIntensity;
    environment.rotation = settings.environmentRotation;
    environment.backgroundIntensity = settings.environmentBackgroundIntensity;

    RenderSettings& render = document.renderSettings();
    render.skyIntensity = settings.skyIntensity;
    render.rayleighScaleHeight = settings.rayleighScaleHeight;
    render.mieScaleHeight = settings.mieScaleHeight;
    render.mieAnisotropy = settings.mieAnisotropy;
    render.groundAlbedo = settings.groundAlbedo;
    render.homogeneousVolumeEnabled = settings.homogeneousVolumeEnabled;
    render.homogeneousVolumeScattering = settings.homogeneousVolumeScattering;
    render.homogeneousVolumeAbsorption = settings.homogeneousVolumeAbsorption;
    render.homogeneousVolumeAnisotropy = settings.homogeneousVolumeAnisotropy;
    render.physicalExposureCompensation = settings.physicalExposureCompensation;
    render.dofApertureRadius = settings.dofApertureRadius;
    render.dofFocusDistance = settings.dofFocusDistance;
    render.saturation = settings.saturation;
    render.contrast = settings.contrast;

    WorldSettings& world = document.worldSettings();
    world.atmosphereEnabled = selection.skyAtmosphere != nullptr && selection.skyAtmosphere->enabled;
    world.fogEnabled = selection.heightFog != nullptr && selection.heightFog->enabled && selection.heightFog->density > 0.0f;
}

} // namespace rtv
