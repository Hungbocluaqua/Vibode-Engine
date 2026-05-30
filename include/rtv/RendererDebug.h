#pragma once

#include <cstdint>
#include <string_view>

namespace rtv {

enum class ToneMapper : uint32_t {
    Linear = 0,
    Reinhard = 1,
    ReinhardWhite = 2,
    ACES = 3,
    PBRNeutral = 4,
    AgX = 5,
};

enum class RendererDebugView : uint32_t {
    Beauty = 0,
    Variance = 1,
    Normals = 2,
    ReprojectionConfidence = 3,
    DenoiserRejection = 4,
    Depth = 5,
    Roughness = 6,
    DirectLighting = 7,
    IndirectLighting = 8,
    EmissiveContribution = 9,
    EnvironmentContribution = 10,
    TraversalSteps = 11,
    BvhDepth = 12,
    InstanceId = 13,
    MeshId = 14,
    TlasSteps = 15,
    TraversalMismatch = 16,
    LightPdf = 17,
    BsdfPdf = 18,
    MisWeight = 19,
    DirectSampleType = 20,
    Albedo = 21,
    ClayMaterial = 22,
    FirstBounceThroughput = 23,
    SecondaryEnvironmentMiss = 24,
    BounceCount = 25,
    SecondaryEnvironmentRadiance = 26,
    WhiteEnvironmentTransport = 27,
    MotionVectors = 28,
    AtmosphereSkyView = 29,
    AtmosphereTransmittance = 30,
    AtmosphereAerialPerspective = 31,
    AtmosphereMultiScatter = 32,
    TemporalReactiveMask = 33,
    TemporalHistoryWeight = 34,
    RestirReservoirAge = 35,
    RestirReservoirConfidence = 36,
    RestirReservoirM = 37,
    EmissiveContinuation = 38,
    SunMisWeight = 39,
    SunLightPdf = 40,
    SunPreviousBsdfPdf = 41,
    RisRawLightPdf = 42,
    RisEffectiveLightPdf = 43,
    RisPdfRatio = 44,
    SampleDimension = 45,
    SampleScramble = 46,
    PathDirectDiffuse = 47,
    PathDirectSpecular = 48,
    PathIndirectDiffuse = 49,
    PathIndirectSpecular = 50,
    PathDataAlbedo = 51,
    PathDataMetrics = 52,
    DenoiserKernelRadius = 53,
    DenoiserHitDistance = 54,
    DenoiserVirtualMotion = 55,
    DenoiserDiffuseDebug = 56,
    DenoiserSpecularDebug = 57,
    DenoiserEmissiveClamp = 58,
    DenoiserVarianceConfidence = 59,
    DenoiserDiffuseChannelConfidence = 60,
    DenoiserFrameBlend = 61,
    DenoiserMaxHitDistanceDelta = 62,
    DenoiserDiffuseOnScreen = 63,
    DenoiserBaseDisocclusion = 64,
    DenoiserSpecularChannelConfidence = 65,
    DenoiserSpecularHistoryWeight = 66,
    RestirPairwiseMis = 67,
    RestirGiValidity = 68,
    RestirGiAge = 69,
    RestirGiInitial = 70,
    RestirGiTemporal = 71,
    RestirGiSpatial = 72,
    RestirGiFinal = 73,
    RestirGiNormal = 90,
    RestirGiHitDistance = 91,
    WavefrontQueueOccupancy = 92,
    WavefrontPathDepth = 93,
    WavefrontLiveRays = 94,
    WavefrontTerminatedRays = 95,
    WavefrontMaterialBucket = 96,
    WavefrontRestirDi = 97,
    WavefrontDirectLighting = 98,
    WavefrontRestirGi = 99,
    CausticVisibility = 100,
    DenoiserDirectDiffuseVariance = 74,
    DenoiserDirectSpecularVariance = 75,
    DenoiserIndirectDiffuseVariance = 76,
    DenoiserIndirectSpecularVariance = 77,
    DenoiserDiffuseHistoryLength = 78,
    DenoiserSpecularHistoryLength = 79,
    MomentUpdateValidity = 80,
    MomentDisocclusionConfidence = 81,
    MomentNormalCone = 82,
    MomentDepthDelta = 83,
    MomentHistoryKindValid = 84,
    DenoiserDiffuseRawVariance = 87,
    DenoiserSpecularRawVariance = 88,
    MaterialOcclusion = 89,
};

enum class RestirMode : uint32_t {
    ClassicNee = 0,
    RestirOnly = 1,
    HybridCompare = 2,
};

enum class AdaptiveQualityMode : uint32_t {
    Off = 0,
    Conservative = 1,
    Balanced = 2,
    Aggressive = 3,
};

enum class RenderPreset : uint32_t {
    Custom = 0,
    Low = 1,
    Balanced = 2,
    Ultra = 3,
};

struct RendererDebugParams {
    uint32_t view = static_cast<uint32_t>(RendererDebugView::Beauty);
    uint32_t flags = 0;
    uint32_t selectedInstance = UINT32_MAX;
    float scale = 1.0f;
};

inline constexpr uint32_t rendererDebugFlagRayTracingCounters = 1u << 0u;

[[nodiscard]] const char* toneMapperName(ToneMapper toneMapper);
[[nodiscard]] const char* restirModeName(RestirMode mode);
[[nodiscard]] const char* renderPresetName(RenderPreset preset);
[[nodiscard]] RenderPreset parseRenderPreset(std::string_view value);
[[nodiscard]] RendererDebugView parseRendererDebugView(std::string_view value);
[[nodiscard]] const char* rendererDebugViewName(RendererDebugView view);

} // namespace rtv
