#pragma once

#include "rtv/RendererSettings.h"
#include "rtv/SceneComponents.h"

namespace rtv {

class SceneDocument;

struct SceneWorldComponentSelection {
    const EnvironmentLight* environmentLight = nullptr;
    const SkyAtmosphere* skyAtmosphere = nullptr;
    const HeightFog* heightFog = nullptr;
    const VolumetricCloud* volumetricCloud = nullptr;
    const PostProcessVolume* postProcessVolume = nullptr;
    const CameraPostProcess* cameraPostProcess = nullptr;
    bool hasWorldComponents = false;
};

[[nodiscard]] SceneWorldComponentSelection selectSceneWorldComponents(const SceneDocument& document);
void applySceneWorldComponentsToRendererSettings(const SceneDocument& document, RendererSettings& settings);
void applySceneWorldComponentsToDocumentSettings(SceneDocument& document);

} // namespace rtv
