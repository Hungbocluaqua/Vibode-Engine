/*
 * Engine bridge for NVIDIA RTXDI reservoir math.
 *
 * The primary ray pass still owns candidate generation and the existing DI
 * final pass still owns visibility/material shading. This kernel replaces the
 * engine's temporal and spatial reservoir merge with RTXDI's canonical
 * weighted-reservoir operations while preserving the engine's packed ABI.
 */

#include <Rtxdi/DI/Reservoir.hlsli>

struct EngineReservoir
{
    uint4 sampleMetadata;
    uint4 reservoirMetadata;
    float4 samplePositionDistance;
};

struct EngineReceiver
{
    float4 worldPositionDepth;
    float4 normalRoughness;
    uint4 packedMaterialSurface;
};

[[vk::binding(0, 0)]] StructuredBuffer<EngineReservoir> g_InitialReservoirs;
[[vk::binding(1, 0)]] StructuredBuffer<EngineReservoir> g_PreviousReservoirs;
[[vk::binding(2, 0)]] StructuredBuffer<EngineReceiver> g_Receivers;
[[vk::binding(3, 0)]] StructuredBuffer<uint> g_Velocity;
[[vk::binding(4, 0)]] RWStructuredBuffer<EngineReservoir> g_OutputReservoirs;
[[vk::binding(7, 0)]] StructuredBuffer<EngineReceiver> g_PreviousReceivers;
[[vk::binding(24, 0)]] RWStructuredBuffer<uint> g_SourcePixels;

struct ResamplingConstants
{
    uint width;
    uint height;
    uint frameIndex;
    uint enabled;
    uint temporalMaxAge;
    uint spatialRounds;
    uint spatialMaxM;
    uint visibilityPolicy;
    float spatialRadius;
    float normalThreshold;
    float depthThreshold;
    float temporalLuminanceLimitFactor;
    float confidenceDecay;
    float lumClampNeighborAvgFactor;
    float lumClampNeighborMaxFactor;
    float fireflyClamp;
    float productionClampLuminance;
    uint mode;
    uint spatialResultValid;
    uint visibilityRayBudget;
    uint historyValid;
    uint materialVisibilityFlags;
    uint counterEnabled;
    uint rawOutputIsCurrentSample;
    float shadowDistanceBias;
    uint lightVersion;
    uint environmentVersion;
    uint rtxdiPtEnabled;
    uint rtxdiPtReplayEnabled;
};

[[vk::binding(6, 0)]] ConstantBuffer<ResamplingConstants> g_Const;

static const uint kSurfaceSky = 1u << 0u;
static const uint kSurfaceInvalid = 1u << 1u;
static const uint kSurfaceDelta = 1u << 2u;
static const uint kSurfaceAlpha = 1u << 3u;
static const uint kSurfaceUnlit = 1u << 4u;
static const uint kSurfaceUnsupported = 1u << 6u;
static const uint kVisibilityUnknown = 0u;
static const uint kSourcePixelInvalid = 0xffffffffu;

uint Hash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float Random01(uint2 pixel, uint dimension)
{
    uint seed = pixel.x * 0x9e3779b9u ^ pixel.y * 0x85ebca6bu;
    seed ^= g_Const.frameIndex * 0xc2b2ae35u ^ dimension * 0x27d4eb2fu;
    return float(Hash(seed) & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float2 DecodeVelocity(uint packed)
{
    int2 value = int2(int(packed & 0xffffu), int((packed >> 16u) & 0xffffu));
    value.x = value.x >= 32768 ? value.x - 65536 : value.x;
    value.y = value.y >= 32768 ? value.y - 65536 : value.y;
    return float2(value) * (512.0f / 32767.0f);
}

uint SurfaceFlags(EngineReceiver receiver)
{
    return (receiver.packedMaterialSurface.y >> 24u) & 0xffu;
}

bool IsReusableSurface(EngineReceiver receiver)
{
    const uint rejected = kSurfaceSky | kSurfaceInvalid | kSurfaceDelta |
        kSurfaceAlpha | kSurfaceUnlit | kSurfaceUnsupported;
    return (SurfaceFlags(receiver) & rejected) == 0u &&
        all(isfinite(receiver.worldPositionDepth)) &&
        all(isfinite(receiver.normalRoughness));
}

uint ReservoirAge(EngineReservoir reservoir)
{
    return reservoir.reservoirMetadata.x & 0xffu;
}

uint ReservoirM(EngineReservoir reservoir)
{
    return (reservoir.reservoirMetadata.x >> 8u) & 0xffu;
}

bool ReservoirValid(EngineReservoir reservoir)
{
    return (reservoir.reservoirMetadata.x & (1u << 18u)) != 0u && ReservoirM(reservoir) > 0u;
}

float2 ReservoirTargetWeight(EngineReservoir reservoir)
{
    return float2(
        f16tof32(reservoir.reservoirMetadata.w & 0xffffu),
        f16tof32(reservoir.reservoirMetadata.w >> 16u));
}

uint PackHalf2(float2 value)
{
    const uint low = f32tof16(clamp(value.x, 0.0f, 65504.0f));
    const uint high = f32tof16(clamp(value.y, 0.0f, 65504.0f));
    return low | (high << 16u);
}

RTXDI_DIReservoir ToRtxdi(EngineReservoir source, float targetAtReceiver)
{
    RTXDI_DIReservoir result = RTXDI_EmptyDIReservoir();
    if (!ReservoirValid(source))
        return result;

    const float2 targetWeight = ReservoirTargetWeight(source);
    const float sourceTarget = max(targetWeight.x, 1e-6f);
    const float sourceM = float(max(ReservoirM(source), 1u));
    result.lightData = (source.sampleMetadata.z >> 8u) | RTXDI_DIReservoir_LightValidBit;
    result.targetPdf = max(targetAtReceiver, 0.0f);
    result.M = sourceM;
    result.weightSum = targetWeight.y / max(sourceTarget * sourceM, 1e-6f);
    result.age = ReservoirAge(source);
    return result;
}

float TargetAtReceiver(
    EngineReservoir reservoir,
    EngineReceiver sourceReceiver,
    EngineReceiver targetReceiver)
{
    const float sourceTarget = ReservoirTargetWeight(reservoir).x;
    if (!(sourceTarget > 0.0f))
        return 0.0f;

    const uint kind = reservoir.sampleMetadata.z & 0xffu;
    float3 direction;
    if (kind == 2u || kind == 6u || kind == 7u)
    {
        direction = normalize(reservoir.samplePositionDistance.xyz);
    }
    else
    {
        const float3 toLight = reservoir.samplePositionDistance.xyz - targetReceiver.worldPositionDepth.xyz;
        if (dot(toLight, toLight) <= 1e-10f)
            return 0.0f;
        direction = normalize(toLight);
    }

    float3 sourceDirection = direction;
    if (kind != 2u && kind != 6u && kind != 7u)
    {
        const float3 sourceToLight = reservoir.samplePositionDistance.xyz - sourceReceiver.worldPositionDepth.xyz;
        if (dot(sourceToLight, sourceToLight) <= 1e-10f)
            return 0.0f;
        sourceDirection = normalize(sourceToLight);
    }

    const float sourceCosine = max(dot(normalize(sourceReceiver.normalRoughness.xyz), sourceDirection), 1e-4f);
    const float targetCosine = max(dot(normalize(targetReceiver.normalRoughness.xyz), direction), 0.0f);
    return min(sourceTarget * targetCosine / sourceCosine, sourceTarget * 16.0f);
}

bool Compatible(EngineReceiver a, EngineReceiver b)
{
    if (!IsReusableSurface(a) || !IsReusableSurface(b))
        return false;
    if (a.packedMaterialSurface.z != b.packedMaterialSurface.z)
        return false;
    const float normalAgreement = dot(normalize(a.normalRoughness.xyz), normalize(b.normalRoughness.xyz));
    const float depthLimit = max(0.05f, max(abs(a.worldPositionDepth.w), 1.0f) * g_Const.depthThreshold);
    return normalAgreement >= g_Const.normalThreshold &&
        abs(a.worldPositionDepth.w - b.worldPositionDepth.w) <= depthLimit;
}

void StoreResult(
    uint pixelIndex,
    EngineReservoir selected,
    RTXDI_DIReservoir reservoir,
    uint age,
    uint sourcePixel)
{
    if (!RTXDI_IsValidDIReservoir(reservoir) || !(reservoir.targetPdf > 0.0f))
    {
        selected.reservoirMetadata.x &= ~(1u << 18u);
        g_OutputReservoirs[pixelIndex] = selected;
        g_SourcePixels[pixelIndex] = sourcePixel;
        return;
    }

    const uint cappedM = clamp(uint(round(reservoir.M)), 1u, max(g_Const.spatialMaxM, 1u));
    const float customWeightSum = reservoir.weightSum * reservoir.targetPdf * float(cappedM);
    selected.reservoirMetadata.x &= ~0x0007ffffu;
    selected.reservoirMetadata.x |= min(age, 255u);
    selected.reservoirMetadata.x |= min(cappedM, 255u) << 8u;
    selected.reservoirMetadata.x |= kVisibilityUnknown << 16u;
    selected.reservoirMetadata.x |= 1u << 18u;
    selected.reservoirMetadata.w = PackHalf2(float2(reservoir.targetPdf, customWeightSum));
    g_OutputReservoirs[pixelIndex] = selected;
    g_SourcePixels[pixelIndex] = sourcePixel;
}

[numthreads(16, 16, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= g_Const.width || pixel.y >= g_Const.height)
        return;

    const uint pixelIndex = pixel.y * g_Const.width + pixel.x;
    EngineReservoir selected = g_InitialReservoirs[pixelIndex];
    const EngineReceiver receiver = g_Receivers[pixelIndex];
    uint selectedAge = 0u;
    uint sourcePixel = pixel.x | (pixel.y << 16u);

    if (g_Const.enabled == 0u || !ReservoirValid(selected) || !IsReusableSurface(receiver))
    {
        g_OutputReservoirs[pixelIndex] = selected;
        g_SourcePixels[pixelIndex] = sourcePixel;
        return;
    }

    RTXDI_DIReservoir current = ToRtxdi(selected, ReservoirTargetWeight(selected).x);
    RTXDI_DIReservoir merged = RTXDI_EmptyDIReservoir();
    RTXDI_CombineDIReservoirs(merged, current, Random01(pixel, 0u), current.targetPdf);
    float representedM = current.M;

    if (g_Const.historyValid != 0u && g_Const.temporalMaxAge > 0u)
    {
        const float2 velocity = DecodeVelocity(g_Velocity[pixelIndex]);
        const int2 previousPixel = int2(pixel) - int2(round(velocity));
        if (all(previousPixel >= 0) && previousPixel.x < int(g_Const.width) && previousPixel.y < int(g_Const.height))
        {
            const uint previousIndex = uint(previousPixel.y) * g_Const.width + uint(previousPixel.x);
            const EngineReservoir previous = g_PreviousReservoirs[previousIndex];
            const EngineReceiver previousReceiver = g_PreviousReceivers[previousIndex];
            if (ReservoirValid(previous) && ReservoirAge(previous) < g_Const.temporalMaxAge && Compatible(receiver, previousReceiver))
            {
                const float shiftedTarget = TargetAtReceiver(previous, previousReceiver, receiver);
                RTXDI_DIReservoir temporal = ToRtxdi(previous, shiftedTarget);
                const bool choseTemporal = RTXDI_CombineDIReservoirs(
                    merged, temporal, Random01(pixel, 1u), shiftedTarget);
                representedM += temporal.M;
                if (choseTemporal)
                {
                    selected = previous;
                    selectedAge = min(ReservoirAge(previous) + 1u, g_Const.temporalMaxAge);
                    sourcePixel = uint(previousPixel.x) | (uint(previousPixel.y) << 16u);
                }
            }
        }
    }

    const uint spatialSamples = g_Const.spatialResultValid != 0u
        ? min(max(g_Const.spatialRounds, 1u), 8u)
        : 0u;
    const int radius = max(int(round(g_Const.spatialRadius)), 1);
    static const int2 offsets[8] = {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
        int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1)
    };

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < spatialSamples; ++sampleIndex)
    {
        const uint rotation = Hash(g_Const.frameIndex + pixelIndex) & 7u;
        const int2 neighborPixel = int2(pixel) + offsets[(sampleIndex + rotation) & 7u] * radius;
        if (any(neighborPixel < 0) || neighborPixel.x >= int(g_Const.width) || neighborPixel.y >= int(g_Const.height))
            continue;

        const uint neighborIndex = uint(neighborPixel.y) * g_Const.width + uint(neighborPixel.x);
        const EngineReceiver neighborReceiver = g_Receivers[neighborIndex];
        const EngineReservoir neighbor = g_InitialReservoirs[neighborIndex];
        if (!ReservoirValid(neighbor) || !Compatible(receiver, neighborReceiver))
            continue;

        const float shiftedTarget = TargetAtReceiver(neighbor, neighborReceiver, receiver);
        RTXDI_DIReservoir spatial = ToRtxdi(neighbor, shiftedTarget);
        const bool choseSpatial = RTXDI_CombineDIReservoirs(
            merged, spatial, Random01(pixel, 2u + sampleIndex), shiftedTarget);
        representedM += spatial.M;
        if (choseSpatial)
        {
            selected = neighbor;
            selectedAge = 0u;
            sourcePixel = uint(neighborPixel.x) | (uint(neighborPixel.y) << 16u);
        }
    }

    merged.M = min(representedM, float(max(g_Const.spatialMaxM, 1u)));
    RTXDI_FinalizeResampling(merged, 1.0f, max(merged.M, 1.0f));
    StoreResult(pixelIndex, selected, merged, selectedAge, sourcePixel);
}
