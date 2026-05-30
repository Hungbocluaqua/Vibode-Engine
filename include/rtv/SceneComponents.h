#pragma once

#include "rtv/MeshAsset.h"
#include "rtv/RendererDebug.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rtv {

struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotationEuler{0.0f};
    glm::vec3 scale{1.0f};
    bool dirty = true;

    [[nodiscard]] glm::quat rotation() const {
        return glm::quat(rotationEuler);
    }

    [[nodiscard]] glm::mat4 localMatrix() const {
        const glm::mat4 translation = glm::translate(glm::mat4{1.0f}, position);
        const glm::mat4 rotationMatrix = glm::mat4_cast(rotation());
        const glm::mat4 scaleMatrix = glm::scale(glm::mat4{1.0f}, scale);
        return translation * rotationMatrix * scaleMatrix;
    }

    [[nodiscard]] glm::mat4 worldMatrix(const glm::mat4& parent = glm::mat4{1.0f}) const {
        return parent * localMatrix();
    }

    void markClean() {
        dirty = false;
    }
};

struct MaterialSlot {
    std::string name;
    MaterialAssetHandle material{};
    std::optional<MaterialAssetHandle> overrideMaterial;
    TextureAssetHandle textureReference{};

    [[nodiscard]] MaterialAssetHandle resolvedMaterial() const {
        return overrideMaterial.value_or(material);
    }
};

struct MeshRenderer {
    MeshAssetHandle mesh{};
    std::vector<MaterialSlot> materialSlots;
    bool visible = true;
    bool castShadow = true;
    bool visibleToCamera = true;
    uint32_t rendererInstanceIndex = UINT32_MAX;
};

enum class LightType : uint32_t {
    Directional,
    Point,
    Area,
};

struct Light {
    LightType type = LightType::Point;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float sizeOrRadius = 1.0f;
    bool enabled = true;
};

struct Sun {
    bool enabled = true;
    float illuminanceLux = 100000.0f;
    float angularRadiusRadians = 0.00465f;
    float colorTemperatureKelvin = 5778.0f;
};

struct Camera {
    float verticalFovRadians = 60.0f * 0.017453292519943295f;
    float nearPlane = 0.01f;
    float farPlane = 1000.0f;
    bool active = false;
    bool useRenderSettingsExposure = true;
};

struct Environment {
    std::filesystem::path hdrPath;
    float intensity = 1.0f;
    float rotation = 0.0f;
    float backgroundIntensity = 0.35f;
    bool enabled = true;
};

struct RenderSettings {
    RenderPreset renderPreset = RenderPreset::Balanced;
    bool pathTracingEnabled = true;
    bool cameraJitterEnabled = true;
    bool directLightingEnabled = true;
    uint32_t maxBounces = 5;
    uint32_t environmentDirectSamples = 1;
    ToneMapper toneMapper = ToneMapper::ACES;
    float exposure = 2.0f;
    float gamma = 2.2f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float brightness = 0.0f;
    float whitePoint = 4.0f;
    bool autoExposureEnabled = false;
    float targetLuminance = 0.18f;
    float minExposure = 0.25f;
    float maxExposure = 8.0f;
    float adaptationSpeed = 2.0f;
    float histogramMinLogLuminance = -10.0f;
    float histogramMaxLogLuminance = 10.0f;
    float histogramLowPercentile = 0.05f;
    float histogramHighPercentile = 0.95f;
    float histogramTargetPercentile = 0.60f;
    bool sunlightEnabled = true;
    float sunIntensity = 1.0f;
    float skyIntensity = 0.8f;
    float sunElevation = 0.97f;
    float sunAzimuth = 3.14159265358979323846f;
    float sunAngularRadius = 0.00465f;
    float rayleighScaleHeight = 8000.0f;
    float mieScaleHeight = 1200.0f;
    float mieAnisotropy = 0.8f;
    float groundAlbedo = 0.3f;
    float indirectStrength = 1.0f;
    RestirMode restirMode = RestirMode::RestirOnly;
    bool restirGiEnabled = true;
    bool denoiserEnabled = true;
    bool denoiseWhileMoving = true;
    uint32_t samplesPerPixel = 1;
    bool limitSamplesPerPixel = true;
    uint32_t atrousIterations = 3;
    float denoiserStrength = 1.05f;
    uint32_t denoiserMaxHistoryLength = 48;
    float momentValidityThreshold = 0.20f;
    bool taaEnabled = true;
    float taaFeedback = 0.06f;
    float taaMotionFeedback = 0.90f;
    float taaReactiveFeedback = 0.98f;
    float taaSharpeningStrength = 0.05f;
    RendererDebugView debugView = RendererDebugView::Beauty;
    bool accumulate = true;
    uint32_t accumulationLimit = 0;
    float resolutionScale = 0.65f;
    float materialTextureAnisotropy = 4.0f;
    bool specularAaEnabled = true;
    bool opacityMicromapsEnabled = true;
    float shadowRayBias = 0.001f;
    float shadowDistanceBias = 0.002f;
    float fireflyClamp = 8.0f;
    uint32_t restirGiTemporalMaxAge = 18;
    uint32_t restirGiSpatialRounds = 3;
    float restirGiSpatialRadius = 3.75f;
    float restirGiDepthThresholdScale = 0.85f;
    float restirGiSpatialCompatibilityThreshold = 0.06f;
    bool restirGiHalfResolution = false;
    uint32_t restirGiVisibilityRayBudget = 0;
    AdaptiveQualityMode adaptiveQualityMode = AdaptiveQualityMode::Balanced;
    float adaptiveGpuFrameTargetMs = 16.6f;

    bool usePhysicalCamera = false;
    float physicalAperture = 16.0f;
    float physicalShutterSeconds = 1.0f / 125.0f;
    float physicalIso = 100.0f;
    float physicalExposureCompensation = 0.0f;
    float dofApertureRadius = 0.0f;
    float dofFocusDistance = 10.0f;
    uint32_t dofBladeCount = 0;
    float dofBokehRotation = 0.0f;
    bool motionBlurEnabled = false;
    float motionBlurShutterOpen = 0.0f;
    float motionBlurShutterClose = 1.0f;
    bool homogeneousVolumeEnabled = false;
    float homogeneousVolumeScattering = 0.0f;
    float homogeneousVolumeAbsorption = 0.0f;
    float homogeneousVolumeAnisotropy = 0.0f;
    bool mneeCausticsEnabled = false;
};

enum class SceneUpdateKind : uint32_t {
    None,
    MaterialOnly,
    TransformOnly,
    LightOnly,
    EnvironmentOnly,
    CameraOnly,
    VisibilityOnly,
    TopologyChanged,
    RendererSettingsOnly,
};

[[nodiscard]] const char* sceneUpdateKindName(SceneUpdateKind kind);

} // namespace rtv
