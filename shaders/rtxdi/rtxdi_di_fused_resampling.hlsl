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
    uint rtxdiPtMaxReservoirAge;
    uint rtxdiPtMaxBounceDepth;
    uint rtxdiPtMaxRcVertexLength;
    uint rtxdiCheckerboardField;
    uint rtxdiDiLocalLightSamples;
    uint rtxdiDiBrdfSamples;
    uint rtxdiDiInfiniteLightSamples;
    uint rtxdiDiEnvironmentSamples;
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

float4 DecodeUnorm4x8(uint value)
{
    return float4(
        value & 0xffu,
        (value >> 8u) & 0xffu,
        (value >> 16u) & 0xffu,
        (value >> 24u) & 0xffu) * (1.0f / 255.0f);
}

float3 ReceiverBaseColor(EngineReceiver receiver)
{
    return DecodeUnorm4x8(receiver.packedMaterialSurface.x).xyz;
}

float3 ReceiverSpecularColor(EngineReceiver receiver)
{
    return DecodeUnorm4x8(receiver.packedMaterialSurface.y).xyz;
}

float ReceiverMetallic(EngineReceiver receiver)
{
    return DecodeUnorm4x8(receiver.packedMaterialSurface.x).w;
}

float3 DecodeOctahedral(float2 encoded)
{
    float3 normal = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0.0f)
        normal.xy = (1.0f - abs(normal.yx)) * sign(normal.xy);
    return normalize(normal);
}

float3 ReceiverViewDirection(EngineReceiver receiver)
{
    const uint packed = receiver.packedMaterialSurface.w;
    int2 value = int2(int(packed & 0xffffu), int((packed >> 16u) & 0xffffu));
    value.x = value.x >= 32768 ? value.x - 65536 : value.x;
    value.y = value.y >= 32768 ? value.y - 65536 : value.y;
    return DecodeOctahedral(float2(value) / 32767.0f);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float GgxDistribution(float nDotH, float alpha)
{
    const float a2 = alpha * alpha;
    const float denominator = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265359f * denominator * denominator, 1.0e-7f);
}

float SmithG1(float nDotV, float alpha)
{
    const float a2 = alpha * alpha;
    return 2.0f * nDotV /
        max(nDotV + sqrt(a2 + (1.0f - a2) * nDotV * nDotV), 1.0e-6f);
}

float EvaluateReceiverTarget(EngineReceiver receiver, float3 wi, float3 wo)
{
    const float3 normal = normalize(receiver.normalRoughness.xyz);
    const float nDotL = max(dot(normal, wi), 0.0f);
    const float nDotV = max(dot(normal, wo), 0.0f);
    if (nDotL <= 1.0e-5f || nDotV <= 1.0e-5f)
        return 0.0f;

    const float3 halfVector = normalize(wi + wo);
    const float nDotH = max(dot(normal, halfVector), 0.0f);
    const float vDotH = max(dot(wo, halfVector), 0.0f);
    const float roughness = max(saturate(receiver.normalRoughness.w), 0.045f);
    const float alpha = roughness * roughness;
    const float metallic = saturate(ReceiverMetallic(receiver));
    const float3 baseColor = max(ReceiverBaseColor(receiver), 0.0f);
    const float3 explicitSpecular = max(ReceiverSpecularColor(receiver), 0.0f);
    const float3 f0 = max(lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic), explicitSpecular);
    const float3 fresnel = FresnelSchlick(vDotH, f0);
    const float distribution = GgxDistribution(nDotH, alpha);
    const float geometry = SmithG1(nDotV, alpha) * SmithG1(nDotL, alpha);
    const float specular = dot(max(fresnel * (distribution * geometry /
        max(4.0f * nDotV * nDotL, 1.0e-6f)), 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
    const float diffuse = dot(max((1.0f - fresnel) * (1.0f - metallic) * baseColor, 0.0f),
        float3(0.2126f, 0.7152f, 0.0722f)) / 3.14159265359f;
    return max((specular + diffuse) * nDotL, 0.0f);
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

    const float sourceBrdf = EvaluateReceiverTarget(sourceReceiver, sourceDirection, ReceiverViewDirection(sourceReceiver));
    const float targetBrdf = EvaluateReceiverTarget(targetReceiver, direction, ReceiverViewDirection(targetReceiver));
    if (!(sourceBrdf > 1.0e-6f) || !(targetBrdf > 0.0f))
        return 0.0f;

    float geometryRatio = 1.0f;
    if (kind != 2u && kind != 6u && kind != 7u)
    {
        const float3 sourceDelta = reservoir.samplePositionDistance.xyz - sourceReceiver.worldPositionDepth.xyz;
        const float3 targetDelta = reservoir.samplePositionDistance.xyz - targetReceiver.worldPositionDepth.xyz;
        geometryRatio = sqrt(max(dot(sourceDelta, sourceDelta), 1.0e-6f) /
            max(dot(targetDelta, targetDelta), 1.0e-6f));
    }

    return min(sourceTarget * targetBrdf / sourceBrdf * geometryRatio, sourceTarget * 32.0f);
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

uint ReservoirWidth()
{
    return g_Const.rtxdiCheckerboardField != 0u
        ? (g_Const.width + 1u) / 2u
        : g_Const.width;
}

uint ReservoirIndex(uint2 fullPixel)
{
    if (g_Const.rtxdiCheckerboardField == 0u)
    {
        return fullPixel.y * g_Const.width + fullPixel.x;
    }
    const uint blockSize = RTXDI_RESERVOIR_BLOCK_SIZE;
    const uint reservoirWidth = (g_Const.width + 1u) / 2u;
    const uint widthBlocks = (reservoirWidth + blockSize - 1u) / blockSize;
    const uint blockRowPitch = widthBlocks * blockSize * blockSize;
    const uint2 reservoirPosition = uint2(fullPixel.x >> 1u, fullPixel.y);
    const uint2 block = reservoirPosition / blockSize;
    const uint2 inBlock = reservoirPosition % blockSize;
    return block.y * blockRowPitch +
        block.x * blockSize * blockSize + inBlock.y * blockSize + inBlock.x;
}

[numthreads(8, 8, 1)]
void main(uint2 dispatchPixel : SV_DispatchThreadID)
{
    // Map the half-width checkerboard dispatch to the active full-resolution
    // field. Reservoir/receiver arrays use RTXDI's 16x16 block-linear layout.
    uint2 pixel = dispatchPixel;
    if (g_Const.rtxdiCheckerboardField != 0u)
    {
        pixel.x = dispatchPixel.x * 2u + ((dispatchPixel.y + g_Const.rtxdiCheckerboardField) & 1u);
    }
    if (pixel.x >= g_Const.width || pixel.y >= g_Const.height)
        return;

    const uint pixelIndex = pixel.y * g_Const.width + pixel.x;
    const uint reservoirIndex = ReservoirIndex(pixel);
    EngineReservoir selected = g_InitialReservoirs[reservoirIndex];
    const EngineReceiver receiver = g_Receivers[reservoirIndex];
    uint selectedAge = 0u;
    uint sourcePixel = pixel.x | (pixel.y << 16u);

    if (g_Const.enabled == 0u || !ReservoirValid(selected) || !IsReusableSurface(receiver))
    {
        g_OutputReservoirs[reservoirIndex] = selected;
        g_SourcePixels[reservoirIndex] = sourcePixel;
        return;
    }

    RTXDI_DIReservoir current = ToRtxdi(selected, ReservoirTargetWeight(selected).x);
    RTXDI_DIReservoir merged = RTXDI_EmptyDIReservoir();
    RTXDI_CombineDIReservoirs(merged, current, Random01(pixel, 0u), current.targetPdf);
    float representedM = current.M;

    if (g_Const.historyValid != 0u && g_Const.temporalMaxAge > 0u)
    {
        const float2 velocity = DecodeVelocity(g_Velocity[pixelIndex]);
        static const int2 temporalOffsets[5] = {
            int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
        };
        [unroll]
        for (uint temporalSample = 0u; temporalSample < 5u; ++temporalSample)
        {
            const int2 previousPixel = int2(pixel) - int2(round(velocity)) + temporalOffsets[temporalSample];
            if (any(previousPixel < 0) || previousPixel.x >= int(g_Const.width) || previousPixel.y >= int(g_Const.height))
                continue;
            const uint previousIndex = ReservoirIndex(uint2(previousPixel));
            const EngineReservoir previous = g_PreviousReservoirs[previousIndex];
            const EngineReceiver previousReceiver = g_PreviousReceivers[previousIndex];
            if (ReservoirValid(previous) && ReservoirAge(previous) < g_Const.temporalMaxAge && Compatible(receiver, previousReceiver))
            {
                const float shiftedTarget = TargetAtReceiver(previous, previousReceiver, receiver);
                if (!(shiftedTarget > 0.0f))
                    continue;
                RTXDI_DIReservoir temporal = ToRtxdi(previous, shiftedTarget);
                const bool choseTemporal = RTXDI_CombineDIReservoirs(
                    merged, temporal, Random01(pixel, 1u + temporalSample), shiftedTarget);
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

        const uint neighborIndex = ReservoirIndex(uint2(neighborPixel));
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
    StoreResult(reservoirIndex, selected, merged, selectedAge, sourcePixel);
}
