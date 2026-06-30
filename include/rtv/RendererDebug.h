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
    SkinnedMotionVectors = 101,
    Metallic = 102,
    MaterialAlpha = 103,
    MaterialTransmission = 104,
    MaterialWorkflow = 105,
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
    RestirDiSelectedLight = 106,
    RestirDiTarget = 107,
    RestirDiSourcePdf = 108,
    RestirDiVisibility = 109,
    RestirDiRejectionReason = 110,
    RestirDiTemporalAcceptance = 111,
    RestirDiSpatialAcceptance = 112,
    RestirDiFinalContribution = 113,
    RestirDiReceiverPosition = 114,
    RestirDiReceiverNormal = 115,
    RestirDiLightVersion = 116,
    RestirDiInitialReservoir = 117,
    RestirDiTemporalReservoir = 118,
    RestirDiSpatialReservoir = 119,
    RestirDiFinalReservoir = 120,
    RestirDiWeightSum = 121,
    RestirDiM = 122,
    RestirDiLightClass = 123,
    RestirDiAge = 124,
    RestirDiConfidence = 125,
    RestirDiReferenceDiff = 126,
    RestirGiGrid = 127,
    RestirGiPathClass = 128,
    AdaptiveDensityMap = 129,
    AdaptiveSampleCount = 130,
    AdaptiveUnsampledPixels = 131,
    AdaptiveFilledImage = 132,
    AdaptiveDisocclusionMask = 133,
    RegirGridOccupancy = 134,
    RegirReservoirWeight = 135,
    RegirSelectedLight = 136,
    RegirQueryCount = 137,
    RegirMisWeight = 138,
    RegirEffectivePdf = 139,
    RegirCanonicalUsed = 140,
    RegirQueryCell = 141,
    RegirActiveCellOccupancy = 142,
    RegirHashCollisions = 143,
    RegirSpatialInputWeight = 144,
    RegirSpatialOutputWeight = 145,
    RegirSpatialNeighborCount = 146,
    RegirEnvironmentSource = 147,
    RegirEnvironmentPdf = 148,
    RegirEnvironmentDirection = 149,
    RegirEnvironmentWeight = 150,
    RegirEnvironmentGeneration = 151,
};

enum class RestirMode : uint32_t {
    ClassicNee = 0,
    RestirOnly = 1,
    HybridCompare = 2,
};

enum class RestirDiMode : uint32_t {
    Off = 0,
    Legacy = 1,
    Production = 2,
    ReferenceValidation = 3,
    HybridCompare = 4,
};

enum class RestirDiReservoirLayout : uint32_t {
    Legacy = 0,
    ProductionPacked = 1,
    ValidationFull = 2,
};

enum class RestirGiMode : uint32_t {
    Off = 0,
    LegacyCache = 1,
    Production = 2,
    ReferenceValidation = 3,
};

enum class RestirGiReservoirLayout : uint32_t {
    LegacyCachePacked = 0,
    ProductionPacked = 1,
    ValidationFull = 2,
};

enum class RestirGiActiveTileMaskMode : uint32_t {
    Off = 0,
    On = 1,
    Auto = 2,
};

enum class RestirHistoryCopyMode : uint32_t {
    Copy = 0,
    PingPong = 1,
};

enum class ReservoirLayout : uint32_t {
    LegacyDI = 0,
    LegacyGI = 1,
    PathSpace = 2,
    PathSpaceCompressed = 3,
};

enum class LightingReuseMode : uint32_t {
    LegacyRestirDiGi = 0,
    LegacyRestirDiGiPlusReGIR = 1,
    ExperimentalRestirPT = 2,
    ValidateRestirPTAgainstLegacy = 3,
};

enum class AdaptiveSamplingMode : uint32_t {
    Disabled = 0,
    Heuristic = 1,
    Neural = 2,
};

enum class MixedSidedSplitMode : uint32_t {
    Off = 0,
    Compact = 1,
};

enum class PathTraceKernelMode : uint32_t {
    Generic = 0,
    Native2B = 1,
};

enum class BlendedDecalShadowMode : uint32_t {
    Exact = 0,
    OpaqueShadow = 1,
    AlphaCutoutProxy = 2,
};

enum class Native2BDirectReuseMode : uint32_t {
    Off = 0,
    Ris = 1,
    Temporal = 2,
};

enum class RegirQueryMode : uint32_t {
    Deterministic = 0,
    Stochastic = 1,
};

enum class RegirGridMode : uint32_t {
    Dense = 0,
    Active = 1,
    Hash = 2,
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
    Native30 = 4,
};

enum class DenoiserBackend : uint32_t {
    Engine = 0,
    Nrd = 1,
};

enum class TemporalUpscaler : uint32_t {
    TaaTsr = 0,
    Dlss = 1,
    Nis = 2,
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
[[nodiscard]] const char* restirDiModeName(RestirDiMode mode);
[[nodiscard]] RestirDiMode parseRestirDiMode(std::string_view value);
[[nodiscard]] const char* restirDiReservoirLayoutName(RestirDiReservoirLayout layout);
[[nodiscard]] RestirDiReservoirLayout parseRestirDiReservoirLayout(std::string_view value);
[[nodiscard]] const char* restirGiModeName(RestirGiMode mode);
[[nodiscard]] RestirGiMode parseRestirGiMode(std::string_view value);
[[nodiscard]] const char* restirGiReservoirLayoutName(RestirGiReservoirLayout layout);
[[nodiscard]] RestirGiReservoirLayout parseRestirGiReservoirLayout(std::string_view value);
[[nodiscard]] const char* restirGiActiveTileMaskModeName(RestirGiActiveTileMaskMode mode);
[[nodiscard]] RestirGiActiveTileMaskMode parseRestirGiActiveTileMaskMode(std::string_view value);
[[nodiscard]] const char* restirHistoryCopyModeName(RestirHistoryCopyMode mode);
[[nodiscard]] RestirHistoryCopyMode parseRestirHistoryCopyMode(std::string_view value);
[[nodiscard]] const char* reservoirLayoutName(ReservoirLayout layout);
[[nodiscard]] ReservoirLayout parseReservoirLayout(std::string_view value);
[[nodiscard]] const char* lightingReuseModeName(LightingReuseMode mode);
[[nodiscard]] LightingReuseMode parseLightingReuseMode(std::string_view value);
[[nodiscard]] const char* regirQueryModeName(RegirQueryMode mode);
[[nodiscard]] RegirQueryMode parseRegirQueryMode(std::string_view value);
[[nodiscard]] const char* regirGridModeName(RegirGridMode mode);
[[nodiscard]] RegirGridMode parseRegirGridMode(std::string_view value);
[[nodiscard]] const char* adaptiveSamplingModeName(AdaptiveSamplingMode mode);
[[nodiscard]] AdaptiveSamplingMode parseAdaptiveSamplingMode(std::string_view value);
[[nodiscard]] const char* mixedSidedSplitModeName(MixedSidedSplitMode mode);
[[nodiscard]] MixedSidedSplitMode parseMixedSidedSplitMode(std::string_view value);
[[nodiscard]] const char* pathTraceKernelModeName(PathTraceKernelMode mode);
[[nodiscard]] PathTraceKernelMode parsePathTraceKernelMode(std::string_view value);
[[nodiscard]] const char* blendedDecalShadowModeName(BlendedDecalShadowMode mode);
[[nodiscard]] BlendedDecalShadowMode parseBlendedDecalShadowMode(std::string_view value);
[[nodiscard]] const char* native2BDirectReuseModeName(Native2BDirectReuseMode mode);
[[nodiscard]] Native2BDirectReuseMode parseNative2BDirectReuseMode(std::string_view value);
[[nodiscard]] const char* renderPresetName(RenderPreset preset);
[[nodiscard]] RenderPreset parseRenderPreset(std::string_view value);
[[nodiscard]] const char* denoiserBackendName(DenoiserBackend backend);
[[nodiscard]] DenoiserBackend parseDenoiserBackend(std::string_view value);
[[nodiscard]] const char* temporalUpscalerName(TemporalUpscaler upscaler);
[[nodiscard]] TemporalUpscaler parseTemporalUpscaler(std::string_view value);
[[nodiscard]] RendererDebugView parseRendererDebugView(std::string_view value);
[[nodiscard]] const char* rendererDebugViewName(RendererDebugView view);

} // namespace rtv
