/*
 * RTXDI GI spatial-resampling bridge.
 *
 * Candidate generation, temporal reprojection, and final visibility remain in
 * the engine. This pass uses RTXDI's canonical GI reservoir combine/finalize
 * operations while preserving the production GI reservoir ABI.
 */

#include <Rtxdi/GI/ReSTIRGIParameters.h>

[[vk::binding(5, 0)]] RWStructuredBuffer<RTXDI_PackedGIReservoir> g_RtxdiReservoirStorage;
#define RTXDI_GI_RESERVOIR_BUFFER g_RtxdiReservoirStorage
#define RTXDI_ENABLE_STORE_RESERVOIR 0
#include <Rtxdi/GI/Reservoir.hlsli>

#define uint16_t uint
#include <Rtxdi/PT/ReSTIRPTParameters.h>
#undef uint16_t
[[vk::binding(6, 0)]] RWStructuredBuffer<RTXDI_PackedPTReservoir> g_RtxdiPtReservoirStorage;
#define RTXDI_PT_RESERVOIR_BUFFER g_RtxdiPtReservoirStorage
#include <Rtxdi/PT/Reservoir.hlsli>

struct EngineGiReservoir
{
    float4 x2PositionDistance;
    float4 x2NormalRoughness;
    float4 suffixRadianceSourcePdf;
    float4 sourceDirectionBsdfPdf;
    float4 selectedIntegrandTarget;
    float4 reservoirData;
};

struct EngineGiReceiver
{
    float4 positionDepth;
    float4 normalRoughness;
    float4 geometryNormalMetal;
    float4 albedoOcclusion;
    uint4 materialIds;
    uint4 motion;
};

[[vk::binding(0, 0)]] StructuredBuffer<EngineGiReservoir> g_TemporalReservoirs;
[[vk::binding(1, 0)]] RWStructuredBuffer<EngineGiReservoir> g_SpatialReservoirs;
[[vk::binding(2, 0)]] StructuredBuffer<EngineGiReceiver> g_Receivers;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_Counters;
[[vk::binding(4, 0)]] StructuredBuffer<uint> g_ActiveTileMask;
[[vk::binding(7, 0)]] StructuredBuffer<RTXDI_PackedPTReservoir> g_InitialPtReservoirs;
[[vk::binding(8, 0)]] StructuredBuffer<RTXDI_PackedPTReservoir> g_PreviousPtReservoirs;
[[vk::binding(9, 0)]] RWStructuredBuffer<RTXDI_PackedPTReservoir> g_CurrentPtReservoirs;
[[vk::binding(10, 0)]] StructuredBuffer<EngineGiReservoir> g_PreviousEngineReservoirs;
[[vk::binding(11, 0)]] StructuredBuffer<EngineGiReceiver> g_PreviousReceivers;

struct SpatialConstants
{
    uint width;
    uint height;
    uint frameIndex;
    uint enabled;
    uint spatialRounds;
    uint halfResolution;
    uint visibilityRayBudget;
    float spatialRadius;
    float depthThresholdScale;
    float compatibilityThreshold;
    float temporalMaxAge;
    float4 cameraPosition;
};

[[vk::push_constant]] ConstantBuffer<SpatialConstants> g_Const;

static const uint kPathClassDiffuse = 1u;
static const uint kPathClassEmissive = 4u;
static const uint kFlagValid = 1u << 0u;
static const uint kFlagVisible = 1u << 1u;
static const uint kFlagFinalized = 1u << 2u;
static const uint kFlagVisibilityKnown = 1u << 5u;
static const uint kFlagReconnectedVisibility = 1u << 6u;
static const uint kActiveTileSize = 16u;
static const uint kEnablePathSpaceResampling = 1u << 4u;
static const uint kEnablePathSpaceHistory = 1u << 5u;

static const uint kCounterSpatialPixels = 10u;
static const uint kCounterNeighborsTested = 11u;
static const uint kCounterAccepted = 12u;
static const uint kCounterRejectedInvalid = 13u;
static const uint kCounterRejectedSurface = 14u;
static const uint kCounterRejectedTarget = 15u;
static const uint kCounterSelectedNeighbor = 16u;
static const uint kCounterClamp = 27u;
static const uint kCounterCenterCurrentOnly = 37u;
static const uint kCounterVersionReject = 44u;
static const uint kCounterNonFinite = 45u;

uint Hash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float Random01(uint pixelIndex, uint dimension)
{
    const uint seed = pixelIndex ^ (g_Const.frameIndex * 0x9e3779b9u) ^ (dimension * 0x85ebca6bu);
    return float(Hash(seed) & 0x00ffffffu) * (1.0f / 16777216.0f);
}

void CounterAdd(uint counter, uint value)
{
    if ((g_Const.enabled & 4u) != 0u)
        InterlockedAdd(g_Counters[counter], value);
}

uint ReservoirMeta(EngineGiReservoir reservoir) { return asuint(reservoir.reservoirData.z); }
uint ReservoirAge(EngineGiReservoir reservoir) { return ReservoirMeta(reservoir) & 0xffu; }
uint ReservoirFlags(EngineGiReservoir reservoir) { return (ReservoirMeta(reservoir) >> 8u) & 0xffu; }
uint ReservoirPathClass(EngineGiReservoir reservoir) { return (ReservoirMeta(reservoir) >> 16u) & 0xffu; }
float ReservoirConfidence(EngineGiReservoir reservoir) { return float(ReservoirMeta(reservoir) >> 24u) * (1.0f / 255.0f); }
float ReservoirWeightSum(EngineGiReservoir reservoir) { return max(reservoir.reservoirData.x, 0.0f); }
float ReservoirM(EngineGiReservoir reservoir) { return max(reservoir.reservoirData.y, 1.0f); }
float ReservoirTarget(EngineGiReservoir reservoir) { return max(reservoir.selectedIntegrandTarget.w, 0.0f); }
uint ReservoirVersion(EngineGiReservoir reservoir) { return asuint(reservoir.reservoirData.w); }

uint PackMeta(uint age, uint flags, uint pathClass, float confidence)
{
    const uint packedConfidence = uint(round(saturate(confidence) * 255.0f));
    return (age & 0xffu) | ((flags & 0xffu) << 8u) |
        ((pathClass & 0xffu) << 16u) | (packedConfidence << 24u);
}

void SetMeta(inout EngineGiReservoir reservoir, uint age, uint flags, uint pathClass, float confidence)
{
    reservoir.reservoirData.z = asfloat(PackMeta(age, flags, pathClass, confidence));
}

bool ReusablePathClass(uint pathClass)
{
    return pathClass >= kPathClassDiffuse && pathClass <= kPathClassEmissive;
}

bool ReservoirReusable(EngineGiReservoir reservoir)
{
    return (ReservoirFlags(reservoir) & kFlagValid) != 0u &&
        ReusablePathClass(ReservoirPathClass(reservoir)) &&
        ReservoirWeightSum(reservoir) > 0.0f && ReservoirM(reservoir) >= 1.0f &&
        ReservoirTarget(reservoir) > 0.0f && reservoir.suffixRadianceSourcePdf.w > 0.0f &&
        all(isfinite(reservoir.reservoirData)) && all(isfinite(reservoir.selectedIntegrandTarget));
}

float3 ReceiverNormal(EngineGiReceiver receiver) { return normalize(receiver.normalRoughness.xyz); }
float3 ReceiverPosition(EngineGiReceiver receiver) { return receiver.positionDepth.xyz; }

bool ReceiverValid(EngineGiReceiver receiver)
{
    return receiver.materialIds.x != 0xffffffffu && receiver.positionDepth.w > 0.0f &&
        receiver.positionDepth.w < 65504.0f && isfinite(receiver.positionDepth.w) &&
        dot(receiver.normalRoughness.xyz, receiver.normalRoughness.xyz) > 1.0e-8f;
}

uint ReceiverVersion(EngineGiReceiver receiver)
{
    return Hash(receiver.motion.z ^ Hash(receiver.motion.w + 0x9e3779b9u));
}

float2 DecodeVelocity(uint packed)
{
    int2 encoded = int2(int(packed & 0xffffu), int((packed >> 16u) & 0xffffu));
    encoded.x = encoded.x >= 32768 ? encoded.x - 65536 : encoded.x;
    encoded.y = encoded.y >= 32768 ? encoded.y - 65536 : encoded.y;
    return float2(encoded) * (512.0f / 32767.0f);
}

bool ReceiversCompatible(EngineGiReceiver center, EngineGiReceiver neighbor)
{
    if (!ReceiverValid(center) || !ReceiverValid(neighbor))
        return false;
    const float normalDot = dot(ReceiverNormal(center), ReceiverNormal(neighbor));
    if (normalDot < 1.0f - g_Const.compatibilityThreshold)
        return false;
    const float depthScale = max(abs(center.positionDepth.w), 1.0e-3f);
    return abs(center.positionDepth.w - neighbor.positionDepth.w) <=
        g_Const.depthThresholdScale * depthScale;
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    const float m = pow(saturate(1.0f - cosTheta), 5.0f);
    return f0 + (1.0f - f0) * m;
}

float GgxD(float nDotH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265359f * d * d, 1.0e-8f);
}

float SmithG1(float nDotV, float alpha)
{
    const float a2 = alpha * alpha;
    return 2.0f * nDotV /
        max(nDotV + sqrt(a2 + (1.0f - a2) * nDotV * nDotV), 1.0e-6f);
}

float3 ReceiverFactor(EngineGiReceiver receiver, float3 wi)
{
    const float3 normal = ReceiverNormal(receiver);
    const float3 wo = normalize(g_Const.cameraPosition.xyz - ReceiverPosition(receiver));
    const float nDotL = max(dot(normal, wi), 0.0f);
    const float nDotV = max(dot(normal, wo), 0.0f);
    if (nDotL <= 1.0e-5f || nDotV <= 1.0e-5f)
        return 0.0f;

    const float3 albedo = max(receiver.albedoOcclusion.rgb, 0.0f);
    const float metallic = saturate(receiver.geometryNormalMetal.w);
    const float roughness = max(saturate(receiver.normalRoughness.w), 0.2f);
    const float3 halfVector = normalize(wi + wo);
    const float nDotH = max(dot(normal, halfVector), 0.0f);
    const float vDotH = max(dot(wo, halfVector), 0.0f);
    const float alpha = roughness * roughness;
    const float3 f0 = lerp(0.04f, albedo, metallic);
    const float3 fresnel = FresnelSchlick(vDotH, f0);
    const float distribution = GgxD(nDotH, alpha);
    const float geometry = SmithG1(nDotV, alpha) * SmithG1(nDotL, alpha);
    const float3 specular = fresnel * (distribution * geometry / max(4.0f * nDotV * nDotL, 1.0e-6f));
    float3 diffuse = (1.0f - fresnel) * (1.0f - metallic) * albedo * (1.0f / 3.14159265359f);
    diffuse *= saturate(receiver.albedoOcclusion.a);
    return max((diffuse + specular) * nDotL, 0.0f);
}

float Luminance(float3 value)
{
    return dot(max(value, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}

bool ReconnectSample(inout EngineGiReservoir reservoir, EngineGiReceiver receiver)
{
    if (!ReceiverValid(receiver) || ReservoirVersion(reservoir) != ReceiverVersion(receiver))
        return false;

    const uint pathClass = ReservoirPathClass(reservoir);
    const bool environment = pathClass == 3u || (ReservoirFlags(reservoir) & (1u << 4u)) != 0u;
    float3 wi;
    float measureFactor = 1.0f;
    if (environment)
    {
        wi = normalize(reservoir.x2PositionDistance.xyz);
    }
    else
    {
        const float3 delta = reservoir.x2PositionDistance.xyz - ReceiverPosition(receiver);
        const float distanceSquared = dot(delta, delta);
        if (distanceSquared <= 1.0e-8f || distanceSquared >= 65504.0f * 65504.0f)
            return false;
        const float distance = sqrt(distanceSquared);
        wi = delta / distance;
        const float x2Cosine = max(dot(normalize(reservoir.x2NormalRoughness.xyz), -wi), 0.0f);
        if (x2Cosine <= 1.0e-5f)
            return false;
        measureFactor = x2Cosine / distanceSquared;

        const float x2Roughness = saturate(reservoir.x2NormalRoughness.w);
        if (x2Roughness < 0.65f)
        {
            const float agreement = dot(normalize(reservoir.sourceDirectionBsdfPdf.xyz), wi);
            const float minimumAgreement = lerp(0.9995f, 0.90f, smoothstep(0.25f, 0.65f, x2Roughness));
            if (agreement < minimumAgreement)
                return false;
        }
    }

    const float3 integrand = max(reservoir.suffixRadianceSourcePdf.rgb, 0.0f) *
        ReceiverFactor(receiver, wi) * measureFactor;
    const float target = Luminance(integrand);
    if (!all(isfinite(integrand)) || !isfinite(target) || target <= 1.0e-10f)
        return false;
    reservoir.selectedIntegrandTarget = float4(integrand, target);
    return true;
}

RTXDI_GIReservoir ToRtxdi(EngineGiReservoir reservoir, float sourceTarget)
{
    RTXDI_GIReservoir result = RTXDI_EmptyGIReservoir();
    if (!ReservoirReusable(reservoir))
        return result;
    result.position = reservoir.x2PositionDistance.xyz;
    result.normal = reservoir.x2NormalRoughness.xyz;
    result.radiance = reservoir.suffixRadianceSourcePdf.rgb;
    result.M = max(uint(round(ReservoirM(reservoir))), 1u);
    result.age = ReservoirAge(reservoir);
    result.weightSum = ReservoirWeightSum(reservoir) /
        max(float(result.M) * sourceTarget, 1.0e-10f);
    return result;
}

RTXDI_PTReservoir ToRtxdiPt(EngineGiReservoir reservoir, float sourceTarget)
{
    RTXDI_PTReservoir result = RTXDI_EmptyPTReservoir();
    if (!ReservoirReusable(reservoir))
        return result;
    result.TranslatedWorldPosition = reservoir.x2PositionDistance.xyz;
    result.WorldNormal = reservoir.x2NormalRoughness.xyz;
    result.Radiance = reservoir.suffixRadianceSourcePdf.rgb;
    result.M = max(round(ReservoirM(reservoir)), 1.0f);
    result.Age = ReservoirAge(reservoir);
    result.WeightSum = ReservoirWeightSum(reservoir) /
        max(result.M * sourceTarget, 1.0e-10f);
    result.TargetFunction = reservoir.selectedIntegrandTarget.rgb;
    result.RcWiPdf = max(reservoir.sourceDirectionBsdfPdf.w, 1.0e-10f);
    result.PartialJacobian = 1.0f;
    result.RcVertexLength = 1u;
    result.PathLength = 2u;
    result.RandomSeed = Hash(asuint(reservoir.reservoirData.w));
    result.RandomIndex = ReservoirAge(reservoir);
    return result;
}

uint2 RepresentativeFullPixel(uint2 reusePixel)
{
    const uint2 phase = uint2(g_Const.frameIndex & 1u, (g_Const.frameIndex >> 1u) & 1u);
    return g_Const.halfResolution != 0u
        ? min(reusePixel * 2u + phase, uint2(g_Const.width - 1u, g_Const.height - 1u))
        : reusePixel;
}

bool ActiveTileContains(uint2 fullPixel)
{
    if ((g_Const.enabled & 8u) == 0u)
        return true;
    const uint tileColumns = (g_Const.width + kActiveTileSize - 1u) / kActiveTileSize;
    const uint tileIndex = fullPixel.y / kActiveTileSize * tileColumns + fullPixel.x / kActiveTileSize;
    return g_ActiveTileMask[tileIndex] != 0u;
}

static const uint kReservoirBlockSize = 16u;

uint ReservoirIndex(uint2 pixel, uint width)
{
    const uint2 block = pixel / kReservoirBlockSize;
    const uint2 inBlock = pixel % kReservoirBlockSize;
    const uint blockRowPitch =
        ((width + kReservoirBlockSize - 1u) / kReservoirBlockSize) *
        kReservoirBlockSize * kReservoirBlockSize;
    return block.y * blockRowPitch + block.x * kReservoirBlockSize * kReservoirBlockSize +
        inBlock.y * kReservoirBlockSize + inBlock.x;
}

void StoreFinal(
    uint pixelIndex,
    EngineGiReservoir selected,
    float normalizedWeight,
    float representedM)
{
    const float selectedTarget = ReservoirTarget(selected);
    if (!(representedM > 0.0f) || !(selectedTarget > 0.0f) || !isfinite(normalizedWeight))
    {
        selected.reservoirData.x = 0.0f;
        selected.reservoirData.y = 1.0f;
        SetMeta(selected, ReservoirAge(selected), ReservoirFlags(selected) & ~kFlagValid,
            ReservoirPathClass(selected), ReservoirConfidence(selected));
        g_SpatialReservoirs[pixelIndex] = selected;
        return;
    }

    const uint cappedM = min(max(uint(round(representedM)), 1u), 255u);
    if (representedM > 255.0f)
        CounterAdd(kCounterClamp, 1u);
    selected.reservoirData.x = max(normalizedWeight * selectedTarget * float(cappedM), 0.0f);
    selected.reservoirData.y = float(cappedM);
    SetMeta(selected, ReservoirAge(selected), ReservoirFlags(selected) | kFlagValid | kFlagFinalized,
        ReservoirPathClass(selected), ReservoirConfidence(selected));
    g_SpatialReservoirs[pixelIndex] = selected;
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint outputWidth = g_Const.halfResolution != 0u ? (g_Const.width + 1u) / 2u : g_Const.width;
    const uint outputHeight = g_Const.halfResolution != 0u ? (g_Const.height + 1u) / 2u : g_Const.height;
    if (pixel.x >= outputWidth || pixel.y >= outputHeight)
        return;

    const uint pixelIndex = pixel.y * outputWidth + pixel.x;
    const uint reservoirIndex = ReservoirIndex(pixel, outputWidth);
    EngineGiReservoir selected = g_TemporalReservoirs[reservoirIndex];
    const uint2 fullPixel = RepresentativeFullPixel(pixel);
    const uint fullPixelIndex = fullPixel.y * g_Const.width + fullPixel.x;
    const uint ptReservoirIndex = ReservoirIndex(fullPixel, g_Const.width);
    const bool pathSpaceResampling = (g_Const.enabled & kEnablePathSpaceResampling) != 0u;
    if (!ActiveTileContains(fullPixel))
    {
        g_SpatialReservoirs[reservoirIndex] = selected;
        if (pathSpaceResampling)
            g_CurrentPtReservoirs[ptReservoirIndex] = g_InitialPtReservoirs[ptReservoirIndex];
        return;
    }
    CounterAdd(kCounterSpatialPixels, 1u);

    const EngineGiReceiver center = g_Receivers[fullPixelIndex];
    if (!ReceiverValid(center) || !ReservoirReusable(selected))
    {
        if (!ReservoirReusable(selected))
            CounterAdd(kCounterCenterCurrentOnly, 1u);
        g_SpatialReservoirs[reservoirIndex] = selected;
        if (pathSpaceResampling)
            g_CurrentPtReservoirs[ptReservoirIndex] = g_InitialPtReservoirs[ptReservoirIndex];
        return;
    }

    RTXDI_GIReservoir merged = RTXDI_EmptyGIReservoir();
    RTXDI_PTReservoir mergedPt = RTXDI_EmptyPTReservoir();
    if (pathSpaceResampling)
    {
        RTXDI_PTReservoir currentPt = RTXDI_UnpackPTReservoir(g_InitialPtReservoirs[ptReservoirIndex]);
        if (!RTXDI_IsValidPTReservoir(currentPt) || !isfinite(currentPt.WeightSum))
            currentPt = ToRtxdiPt(selected, ReservoirTarget(selected));
        CombineReservoirs(
            mergedPt, currentPt, Random01(pixelIndex, 0u), selected.selectedIntegrandTarget.rgb);

        if ((g_Const.enabled & kEnablePathSpaceHistory) != 0u && center.motion.y == 0u)
        {
            const float2 velocity = DecodeVelocity(center.motion.x);
            const int2 previousFullPixel = int2(fullPixel) - int2(round(velocity));
            if (all(previousFullPixel >= 0) && previousFullPixel.x < int(g_Const.width) &&
                previousFullPixel.y < int(g_Const.height))
            {
                const uint previousFullIndex = uint(previousFullPixel.y) * g_Const.width + uint(previousFullPixel.x);
                const uint previousPtReservoirIndex =
                    ReservoirIndex(uint2(previousFullPixel), g_Const.width);
                const uint2 previousReusePixel = g_Const.halfResolution != 0u
                    ? uint2(previousFullPixel) / 2u
                    : uint2(previousFullPixel);
                const uint previousReuseIndex = ReservoirIndex(previousReusePixel, outputWidth);
                EngineGiReservoir previousEngine = g_PreviousEngineReservoirs[previousReuseIndex];
                RTXDI_PTReservoir previousPt =
                    RTXDI_UnpackPTReservoir(g_PreviousPtReservoirs[previousPtReservoirIndex]);
                const EngineGiReceiver previousReceiver = g_PreviousReceivers[previousFullIndex];
                const uint maxAge = max(uint(round(g_Const.temporalMaxAge)), 1u);
                if (RTXDI_IsValidPTReservoir(previousPt) && previousPt.Age < maxAge &&
                    ReservoirReusable(previousEngine) && ReceiversCompatible(center, previousReceiver))
                {
                    if (ReconnectSample(previousEngine, center))
                    {
                        previousPt.Age = min(previousPt.Age + 1u, 31u);
                        if (CombineReservoirs(
                            mergedPt,
                            previousPt,
                            Random01(pixelIndex, 31u),
                            previousEngine.selectedIntegrandTarget.rgb))
                        {
                            selected = previousEngine;
                            uint flags = ReservoirFlags(selected);
                            flags &= ~(kFlagVisible | kFlagVisibilityKnown | kFlagReconnectedVisibility);
                            SetMeta(selected, previousPt.Age, flags, ReservoirPathClass(selected), ReservoirConfidence(selected));
                        }
                    }
                }
            }
        }
    }
    else
    {
        RTXDI_GIReservoir current = ToRtxdi(selected, ReservoirTarget(selected));
        RTXDI_CombineGIReservoirs(merged, current, Random01(pixelIndex, 0u), ReservoirTarget(selected));
    }

    if ((g_Const.enabled & 1u) != 0u && g_Const.spatialRounds > 0u)
    {
        [loop]
        for (uint sampleIndex = 0u; sampleIndex < min(g_Const.spatialRounds, 8u); ++sampleIndex)
        {
            const float angle = Random01(pixelIndex, sampleIndex * 2u + 1u) * 6.28318530718f;
            const float radius = max(g_Const.spatialRadius, 1.0f) *
                sqrt(Random01(pixelIndex, sampleIndex * 2u + 2u));
            const int2 offset = int2(round(float2(cos(angle), sin(angle)) * radius));
            const int2 neighborPixel = clamp(int2(pixel) + offset, int2(0, 0),
                int2(int(outputWidth) - 1, int(outputHeight) - 1));
            const uint neighborLinearIndex = uint(neighborPixel.y) * outputWidth + uint(neighborPixel.x);
            const uint neighborIndex = ReservoirIndex(uint2(neighborPixel), outputWidth);
            if (neighborLinearIndex == pixelIndex)
                continue;
            CounterAdd(kCounterNeighborsTested, 1u);

            const uint2 neighborFullPixel = RepresentativeFullPixel(uint2(neighborPixel));
            EngineGiReservoir candidate = g_TemporalReservoirs[neighborIndex];
            if (!ReservoirReusable(candidate))
            {
                CounterAdd(kCounterRejectedInvalid, 1u);
                continue;
            }
            const EngineGiReceiver neighbor = g_Receivers[neighborFullPixel.y * g_Const.width + neighborFullPixel.x];
            if (!ReceiversCompatible(center, neighbor))
            {
                CounterAdd(kCounterRejectedSurface, 1u);
                continue;
            }
            if (ReservoirVersion(candidate) != ReceiverVersion(center))
            {
                CounterAdd(kCounterVersionReject, 1u);
                continue;
            }

            const float sourceTarget = ReservoirTarget(candidate);
            if (!ReconnectSample(candidate, center))
            {
                CounterAdd(kCounterRejectedTarget, 1u);
                continue;
            }
            const float shiftedTarget = ReservoirTarget(candidate);
            bool selectedCandidate = false;
            if (pathSpaceResampling)
            {
                const uint neighborPtReservoirIndex = ReservoirIndex(neighborFullPixel, g_Const.width);
                RTXDI_PTReservoir neighborReservoir =
                    RTXDI_UnpackPTReservoir(g_InitialPtReservoirs[neighborPtReservoirIndex]);
                if (!RTXDI_IsValidPTReservoir(neighborReservoir))
                    neighborReservoir = ToRtxdiPt(candidate, sourceTarget);
                if (!isfinite(neighborReservoir.WeightSum))
                {
                    CounterAdd(kCounterNonFinite, 1u);
                    continue;
                }
                if (neighborReservoir.WeightSum <= 0.0f)
                {
                    CounterAdd(kCounterRejectedInvalid, 1u);
                    continue;
                }
                selectedCandidate = CombineReservoirs(
                    mergedPt,
                    neighborReservoir,
                    Random01(pixelIndex, sampleIndex + 32u),
                    candidate.selectedIntegrandTarget.rgb);
            }
            else
            {
                RTXDI_GIReservoir neighborReservoir = ToRtxdi(candidate, sourceTarget);
                if (!isfinite(neighborReservoir.weightSum))
                {
                    CounterAdd(kCounterNonFinite, 1u);
                    continue;
                }
                if (neighborReservoir.weightSum <= 0.0f)
                {
                    CounterAdd(kCounterRejectedTarget, 1u);
                    continue;
                }
                selectedCandidate = RTXDI_CombineGIReservoirs(
                    merged,
                    neighborReservoir,
                    Random01(pixelIndex, sampleIndex + 32u),
                    shiftedTarget);
            }

            CounterAdd(kCounterAccepted, 1u);
            if (selectedCandidate)
            {
                selected = candidate;
                uint flags = ReservoirFlags(selected);
                flags &= ~(kFlagVisible | kFlagVisibilityKnown | kFlagReconnectedVisibility);
                SetMeta(selected, ReservoirAge(selected), flags, ReservoirPathClass(selected), ReservoirConfidence(selected));
                CounterAdd(kCounterSelectedNeighbor, 1u);
            }
        }
    }

    const float selectedTarget = ReservoirTarget(selected);
    if (pathSpaceResampling)
    {
        RTXDI_FinalizeResampling(
            mergedPt,
            1.0f,
            max(mergedPt.M, 1.0f) * max(selectedTarget, 1.0e-10f));
        g_CurrentPtReservoirs[ptReservoirIndex] = RTXDI_PackPTReservoir(mergedPt);
        StoreFinal(reservoirIndex, selected, mergedPt.WeightSum, mergedPt.M);
    }
    else
    {
        RTXDI_FinalizeGIResampling(
            merged,
            1.0f,
            float(max(merged.M, 1u)) * max(selectedTarget, 1.0e-10f));
        StoreFinal(reservoirIndex, selected, merged.weightSum, float(merged.M));
    }
}
