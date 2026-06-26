#pragma once

#include "rtv/OpacityMicromapPreprocess.h"
#include "rtv/RendererDebug.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace rtv {

inline constexpr uint32_t kMaxSamplesPerPixel = 64u;

enum class RestirCounterMode : uint32_t {
    Auto = 0,
    On = 1,
    Off = 2,
};

struct RendererSettings {
    RenderPreset renderPreset = RenderPreset::Balanced;
    bool pathTracingEnabled = true;
    bool cameraJitterEnabled = true;
    bool denoiserEnabled = true;
    DenoiserBackend denoiserBackend = DenoiserBackend::Engine;
    bool denoiseWhileMoving = true;
    bool taaEnabled = true;
    TemporalUpscaler temporalUpscaler = TemporalUpscaler::TaaTsr;
    bool dlssFrameGenerationEnabled = false;
    bool dlssRayReconstructionEnabled = false;
    bool streamlineReflexEnabled = false;
    bool streamlineNvPerfEnabled = false;
    float dlssSharpeningStrength = 0.0f;
    float taaFeedback = 0.06f;
    float taaMotionFeedback = 0.90f;
    float taaReactiveFeedback = 0.98f;
    float taaSharpeningStrength = 0.05f;
    bool sunlightEnabled = true;
    bool directLightingEnabled = true;
    bool secondaryDirectLightingEnabled = true;
    bool environmentEnabled = true;
    uint32_t maxBounces = 5;
    uint32_t samplesPerPixel = 1;
    bool limitSamplesPerPixel = true;
    uint32_t atrousIterations = 3;
    uint32_t environmentDirectSamples = 1;
    float denoiserStrength = 1.05f;
    uint32_t denoiserMaxHistoryLength = 48;
    float momentValidityThreshold = 0.20f;
    float sunIntensity = 1.0f;
    float sunIlluminanceLux = 100000.0f;
    float sunColorTemperatureKelvin = 5778.0f;
    glm::vec3 sunColor{1.0f};
    glm::vec3 sunDirection{0.0f, 0.8240f, -0.5661f};
    float skyIntensity = 0.8f;
    float sunElevation = 0.97f;
    float sunAzimuth = 3.14159265358979323846f;
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
    float maxExposure = 64.0f;
    float adaptationSpeed = 2.0f;
    float histogramMinLogLuminance = -10.0f;
    float histogramMaxLogLuminance = 10.0f;
    float histogramLowPercentile = 0.05f;
    float histogramHighPercentile = 0.95f;
    float histogramTargetPercentile = 0.60f;
    float sunAngularRadius = 0.00465f;
    float rayleighScaleHeight = 8000.0f;
    float mieScaleHeight = 1200.0f;
    float mieAnisotropy = 0.8f;
    float groundAlbedo = 0.3f;
    bool heightFogEnabled = false;
    float heightFogDensity = 0.0f;
    float heightFogHeightFalloff = 5.0f;
    glm::vec3 heightFogColor{0.65f, 0.72f, 0.85f};
    float indirectStrength = 1.0f;
    RestirMode restirMode = RestirMode::RestirOnly;
    RestirDiMode restirDiMode = RestirDiMode::Production;
    bool restirDiTemporalEnabled = true;
    bool restirDiSpatialEnabled = true;
    bool restirDiFinalVisibilityEnabled = false;
    uint32_t restirDiSpatialRounds = 4;
    float restirDiSpatialRadius = 3.0f;
    uint32_t restirDiTemporalMaxAge = 32;
    uint32_t restirDiMaxM = 64;
    uint32_t restirDiVisibilityRayBudget = 1;
    bool restirDiProductionStabilizationEnabled = true;
    float restirDiClampLuminance = 8.0f;
    bool restirDiIncludeSun = false;
    bool restirDiIncludeEnvironment = false;
    RestirDiReservoirLayout restirDiReservoirLayout = RestirDiReservoirLayout::ProductionPacked;
    RestirGiMode restirGiMode = RestirGiMode::Production;
    RestirGiReservoirLayout restirGiReservoirLayout = RestirGiReservoirLayout::ProductionPacked;
    bool restirGiEnabled = true;
    float environmentIntensity = 1.0f;
    float environmentRotation = 0.0f;
    float environmentBackgroundIntensity = 0.35f;
    float renderResolutionScale = 0.65f;
    float materialTextureAnisotropy = 4.0f;
    bool specularAaEnabled = true;
    bool opacityMicromapsEnabled = true;
    bool opacityMicromapBlendEnabled = false;
    bool hardwareBackfaceCullingEnabled = true;
    MixedSidedSplitMode mixedSidedSplitMode = MixedSidedSplitMode::Off;
    PathTraceKernelMode pathTraceKernelMode = PathTraceKernelMode::Generic;
    bool finalBounceFastPathEnabled = false;
    float native2BTerminalDirectSampleProbability = 1.0f;
    BlendedDecalShadowMode blendedDecalShadowMode = BlendedDecalShadowMode::Exact;
    Native2BDirectReuseMode native2BDirectReuseMode = Native2BDirectReuseMode::Off;
    bool forceOpaqueCameraRays = false;
    uint32_t opacityMicromapSubdivisionLevel = kDefaultOpacityMicromapSubdivisionLevel;
    bool wavefrontQueuesEnabled = false;
    bool wavefrontPrimaryGenerateEnabled = false;
    bool wavefrontTraceEnabled = false;
    bool wavefrontShadeEnabled = false;
    bool wavefrontShadowTraceEnabled = false;
    bool wavefrontCompactEnabled = false;
    bool wavefrontSortEnabled = false;
    bool wavefrontFinalOutputEnabled = false;
    bool shaderExecutionReorderingEnabled = true;
    uint32_t accumulationLimit = 0;
    RendererDebugView debugView = RendererDebugView::Beauty;
    float debugScale = 1.0f;
    float shadowRayBias = 0.001f;
    float shadowDistanceBias = 0.002f;
    float fireflyClamp = 8.0f;
    float maxFrameDeltaSeconds = 1.0f / 30.0f;
    float russianRouletteMinSurvival = 0.05f;
    uint32_t restirGiTemporalMaxAge = 12;
    uint32_t restirGiSpatialRounds = 2;
    float restirGiSpatialRadius = 3.0f;
    float restirGiDepthThresholdScale = 0.85f;
    float restirGiSpatialCompatibilityThreshold = 0.08f;
    bool restirGiHalfResolution = false;
    uint32_t restirGiVisibilityRayBudget = 1;
    bool restirGiFinalStabilizationEnabled = true;
    float restirGiMinFinalBlendStrength = 0.01f;
    RestirGiActiveTileMaskMode restirGiActiveTileMaskMode = RestirGiActiveTileMaskMode::Off;
    RestirHistoryCopyMode restirHistoryCopyMode = RestirHistoryCopyMode::Copy;
    RestirCounterMode restirCounterMode = RestirCounterMode::Auto;
    AdaptiveQualityMode adaptiveQualityMode = AdaptiveQualityMode::Balanced;
    float adaptiveGpuFrameTargetMs = 16.6f;

    std::optional<uint32_t> fixedSeed;

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
    bool compactImportedEmissiveTriangleSampling = false;
};

inline void applyRenderPreset(RendererSettings& settings, RenderPreset preset) {
    settings.renderPreset = preset;
    settings.pathTracingEnabled = true;
    settings.cameraJitterEnabled = true;
    settings.denoiserEnabled = true;
    settings.denoiserBackend = DenoiserBackend::Engine;
    settings.denoiseWhileMoving = true;
    settings.taaEnabled = true;
    settings.temporalUpscaler = TemporalUpscaler::TaaTsr;
    settings.dlssFrameGenerationEnabled = false;
    settings.dlssRayReconstructionEnabled = false;
    settings.streamlineReflexEnabled = false;
    settings.streamlineNvPerfEnabled = false;
    settings.dlssSharpeningStrength = 0.0f;
    settings.restirMode = RestirMode::RestirOnly;
    settings.restirDiMode = RestirDiMode::Production;
    settings.restirDiTemporalEnabled = true;
    settings.restirDiSpatialEnabled = true;
    settings.restirDiFinalVisibilityEnabled = false;
    settings.restirDiSpatialRounds = 4;
    settings.restirDiSpatialRadius = 3.0f;
    settings.restirDiTemporalMaxAge = 32;
    settings.restirDiMaxM = 64;
    settings.restirDiVisibilityRayBudget = 1;
    settings.restirDiProductionStabilizationEnabled = true;
    settings.restirDiClampLuminance = 8.0f;
    settings.restirDiIncludeSun = false;
    settings.restirDiIncludeEnvironment = false;
    settings.restirDiReservoirLayout = RestirDiReservoirLayout::ProductionPacked;
    settings.secondaryDirectLightingEnabled = true;
    settings.restirGiMode = RestirGiMode::Production;
    settings.restirGiReservoirLayout = RestirGiReservoirLayout::ProductionPacked;
    settings.restirGiEnabled = true;
    settings.restirGiFinalStabilizationEnabled = true;
    settings.restirGiActiveTileMaskMode = RestirGiActiveTileMaskMode::Off;
    settings.restirHistoryCopyMode = RestirHistoryCopyMode::Copy;
    settings.compactImportedEmissiveTriangleSampling = false;
    settings.finalBounceFastPathEnabled = false;
    settings.native2BTerminalDirectSampleProbability = 1.0f;
    settings.blendedDecalShadowMode = BlendedDecalShadowMode::Exact;
    settings.native2BDirectReuseMode = Native2BDirectReuseMode::Off;
    settings.forceOpaqueCameraRays = false;
    settings.mixedSidedSplitMode = MixedSidedSplitMode::Off;
    settings.pathTraceKernelMode = PathTraceKernelMode::Generic;

    switch (preset) {
    case RenderPreset::Low:
        settings.renderResolutionScale = 0.50f;
        settings.maxBounces = 3;
        settings.samplesPerPixel = 1;
        settings.limitSamplesPerPixel = true;
        settings.environmentDirectSamples = 1;
        settings.atrousIterations = 2;
        settings.denoiserStrength = 1.15f;
        settings.denoiserMaxHistoryLength = 32;
        settings.momentValidityThreshold = 0.22f;
        settings.taaFeedback = 0.08f;
        settings.taaMotionFeedback = 0.78f;
        settings.taaReactiveFeedback = 0.92f;
        settings.taaSharpeningStrength = 0.08f;
        settings.materialTextureAnisotropy = 2.0f;
        settings.specularAaEnabled = false;
        settings.fireflyClamp = 7.0f;
        settings.restirGiTemporalMaxAge = 12;
        settings.restirGiSpatialRounds = 2;
        settings.restirGiSpatialRadius = 3.0f;
        settings.restirGiDepthThresholdScale = 0.95f;
        settings.restirGiSpatialCompatibilityThreshold = 0.08f;
        settings.restirGiHalfResolution = true;
        settings.restirGiVisibilityRayBudget = 1;
        // DI quality scaling for Low
        settings.restirDiSpatialRounds = 2;
        settings.restirDiSpatialRadius = 2.0f;
        settings.restirDiTemporalMaxAge = 16;
        settings.restirDiMaxM = 32;
        settings.adaptiveQualityMode = AdaptiveQualityMode::Balanced;
        break;
    case RenderPreset::Balanced:
        settings.renderResolutionScale = 0.65f;
        settings.maxBounces = 5;
        settings.samplesPerPixel = 1;
        settings.limitSamplesPerPixel = true;
        settings.environmentDirectSamples = 1;
        settings.atrousIterations = 3;
        settings.denoiserStrength = 1.05f;
        settings.denoiserMaxHistoryLength = 48;
        settings.momentValidityThreshold = 0.20f;
        settings.taaFeedback = 0.06f;
        settings.taaMotionFeedback = 0.90f;
        settings.taaReactiveFeedback = 0.98f;
        settings.taaSharpeningStrength = 0.05f;
        settings.materialTextureAnisotropy = 4.0f;
        settings.specularAaEnabled = true;
        settings.fireflyClamp = 8.0f;
        settings.restirGiTemporalMaxAge = 12;
        settings.restirGiSpatialRounds = 2;
        settings.restirGiSpatialRadius = 3.0f;
        settings.restirGiDepthThresholdScale = 0.85f;
        settings.restirGiSpatialCompatibilityThreshold = 0.08f;
        settings.restirGiHalfResolution = false;
        settings.restirGiVisibilityRayBudget = 1;
        // DI quality scaling for Balanced
        settings.restirDiSpatialRounds = 4;
        settings.restirDiSpatialRadius = 3.0f;
        settings.restirDiTemporalMaxAge = 32;
        settings.restirDiMaxM = 64;
        settings.adaptiveQualityMode = AdaptiveQualityMode::Balanced;
        break;
    case RenderPreset::Ultra:
        settings.renderResolutionScale = 1.0f;
        settings.maxBounces = 8;
        settings.samplesPerPixel = 2;
        settings.limitSamplesPerPixel = false;
        settings.environmentDirectSamples = 2;
        settings.atrousIterations = 4;
        settings.denoiserStrength = 0.95f;
        settings.denoiserMaxHistoryLength = 64;
        settings.momentValidityThreshold = 0.18f;
        settings.taaFeedback = 0.05f;
        settings.taaMotionFeedback = 0.80f;
        settings.taaReactiveFeedback = 0.94f;
        settings.taaSharpeningStrength = 0.04f;
        settings.materialTextureAnisotropy = 8.0f;
        settings.specularAaEnabled = true;
        settings.fireflyClamp = 10.0f;
        settings.restirGiTemporalMaxAge = 24;
        settings.restirGiSpatialRounds = 4;
        settings.restirGiSpatialRadius = 4.25f;
        settings.restirGiDepthThresholdScale = 0.85f;
        settings.restirGiSpatialCompatibilityThreshold = 0.05f;
        settings.restirGiHalfResolution = false;
        settings.restirGiVisibilityRayBudget = 4;
        // DI quality scaling for Ultra
        settings.restirDiSpatialRounds = 6;
        settings.restirDiSpatialRadius = 4.0f;
        settings.restirDiTemporalMaxAge = 48;
        settings.restirDiMaxM = 96;
        settings.adaptiveQualityMode = AdaptiveQualityMode::Off;
        break;
    case RenderPreset::Native30:
        settings.renderResolutionScale = 1.0f;
        settings.maxBounces = 2;
        settings.samplesPerPixel = 1;
        settings.limitSamplesPerPixel = true;
        settings.environmentDirectSamples = 1;
        settings.atrousIterations = 1;
        settings.denoiserStrength = 1.05f;
        settings.denoiserMaxHistoryLength = 32;
        settings.momentValidityThreshold = 0.20f;
        settings.taaFeedback = 0.06f;
        settings.taaMotionFeedback = 0.88f;
        settings.taaReactiveFeedback = 0.96f;
        settings.taaSharpeningStrength = 0.05f;
        settings.materialTextureAnisotropy = 1.0f;
        settings.specularAaEnabled = true;
        settings.fireflyClamp = 8.0f;
        settings.restirGiMode = RestirGiMode::Off;
        settings.restirGiEnabled = false;
        settings.restirGiTemporalMaxAge = 12;
        settings.restirGiSpatialRounds = 0;
        settings.restirGiSpatialRadius = 2.75f;
        settings.restirGiDepthThresholdScale = 0.90f;
        settings.restirGiSpatialCompatibilityThreshold = 0.09f;
        settings.restirGiHalfResolution = true;
        settings.restirGiVisibilityRayBudget = 1;
        settings.restirGiActiveTileMaskMode = RestirGiActiveTileMaskMode::Off;
        settings.restirDiSpatialRounds = 2;
        settings.restirDiSpatialRadius = 2.5f;
        settings.restirDiTemporalMaxAge = 24;
        settings.restirDiMaxM = 32;
        settings.restirHistoryCopyMode = RestirHistoryCopyMode::PingPong;
        settings.opacityMicromapsEnabled = true;
        settings.shaderExecutionReorderingEnabled = true;
        settings.secondaryDirectLightingEnabled = true;
        settings.compactImportedEmissiveTriangleSampling = true;
        settings.finalBounceFastPathEnabled = true;
        settings.native2BTerminalDirectSampleProbability = 1.0f;
        settings.blendedDecalShadowMode = BlendedDecalShadowMode::OpaqueShadow;
        settings.adaptiveQualityMode = AdaptiveQualityMode::Off;
        settings.adaptiveGpuFrameTargetMs = 33.3f;
        break;
    case RenderPreset::Custom:
        break;
    }
}

} // namespace rtv
