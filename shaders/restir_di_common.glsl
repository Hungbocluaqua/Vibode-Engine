#ifndef RTV_RESTIR_DI_COMMON_GLSL
#define RTV_RESTIR_DI_COMMON_GLSL

#ifndef RTV_RESTIR_DI_VALIDATION_FULL
#define RTV_RESTIR_DI_VALIDATION_FULL 0
#endif

// ---------------------------------------------------------------------------
// ReSTIR DI data model — defined here for compute passes, with include guards
// so rt_common.glsl (which also defines them for the raygen) doesn't conflict.
// ---------------------------------------------------------------------------
#ifndef RTV_RESTIR_DI_RESERVOIR_DEFINED
#define RTV_RESTIR_DI_RESERVOIR_DEFINED
struct RestirDiReservoir {
    uvec4 sampleMetadata;
    uvec4 reservoirMetadata;
    vec4 samplePosition_distance;
    vec4 sampleDirection_pdf;
    vec4 sampleRadiance_target;
    vec4 sampleNormal_weightSum;
    vec4 contribution_confidence;
};
#endif

#ifndef RTV_RESTIR_DI_RECEIVER_DEFINED
#define RTV_RESTIR_DI_RECEIVER_DEFINED
struct RestirDiReceiver {
    vec4 worldPosition_depth;
    vec4 normal_roughness;
    vec4 tangent_materialId;
    vec4 bitangent_instanceId;
    vec4 viewDirection_hitDist;
    uvec4 primitive_mesh_flags;
};
#endif

// Surface flags
const uint RESTIR_DI_SURFACE_SKY       = 1u << 0u;
const uint RESTIR_DI_SURFACE_INVALID   = 1u << 1u;
const uint RESTIR_DI_SURFACE_DELTA     = 1u << 2u;
const uint RESTIR_DI_SURFACE_ALPHA     = 1u << 3u;
const uint RESTIR_DI_SURFACE_UNLIT     = 1u << 4u;

// ---------------------------------------------------------------------------
// Reservoir metadata packing (ProductionPacked)
// ---------------------------------------------------------------------------

// reservoirMetadata.x: [age:8][M:8][visibility:2][valid:1][reserved:13]
uint restir_di_pack_reservoir_state(uint age, uint m, uint visibility, bool valid) {
    uint packed = min(age, 255u);
    packed |= (min(m, 255u) << 8u);
    packed |= ((visibility & 3u) << 16u);
    packed |= (valid ? (1u << 18u) : 0u);
    return packed;
}

// reservoirMetadata.y: [sourcePdf:fp16_hi][previousWeight:fp16_lo]
uint restir_di_pack_pdf_weight(float sourcePdf, float previousWeight) {
    return packHalf2x16(vec2(clamp(sourcePdf, 1.0e-6, 65504.0), clamp(previousWeight, 0.0, 1.0)));
}

// ---------------------------------------------------------------------------
// Visibility constants
// ---------------------------------------------------------------------------
const uint RESTIR_DI_VISIBILITY_UNKNOWN  = 0u;
const uint RESTIR_DI_VISIBILITY_VISIBLE  = 1u;
const uint RESTIR_DI_VISIBILITY_OCCLUDED = 2u;
const uint RESTIR_DI_VISIBILITY_INVALID  = 3u;

// Rejection flags (reservoirMetadata.w)
const uint RESTIR_DI_REJECT_NONE              = 0u;
const uint RESTIR_DI_REJECT_SURFACE_MISMATCH  = 1u << 0u;
const uint RESTIR_DI_REJECT_LIGHT_VERSION     = 1u << 1u;
const uint RESTIR_DI_REJECT_AGE               = 1u << 2u;
const uint RESTIR_DI_REJECT_TARGET_ZERO       = 1u << 3u;
const uint RESTIR_DI_REJECT_PDF_ZERO          = 1u << 4u;
const uint RESTIR_DI_REJECT_VISIBILITY        = 1u << 5u;
const uint RESTIR_DI_REJECT_NAN               = 1u << 6u;
const uint RESTIR_DI_REJECT_CLAMPED           = 1u << 7u;

// ---------------------------------------------------------------------------
// Reservoir accessors (ProductionPacked)
// ---------------------------------------------------------------------------

RestirDiReservoir restir_di_empty_reservoir() {
    RestirDiReservoir r;
    r.sampleMetadata = uvec4(0u);
    r.reservoirMetadata = uvec4(0u);
    r.samplePosition_distance = vec4(0.0);
    r.sampleDirection_pdf = vec4(0.0, 0.0, 1.0, 1.0e-6);
    r.sampleRadiance_target = vec4(0.0);
    r.sampleNormal_weightSum = vec4(0.0, 1.0, 0.0, 0.0);
    r.contribution_confidence = vec4(0.0);
    r.reservoirMetadata.y = restir_di_pack_pdf_weight(1.0e-6, 0.0);
    return r;
}

bool restir_di_reservoir_valid(RestirDiReservoir r) {
    return (r.reservoirMetadata.x & (1u << 18u)) != 0u;
}

uint restir_di_age(RestirDiReservoir r) {
    return r.reservoirMetadata.x & 0xffu;
}

uint restir_di_m(RestirDiReservoir r) {
    return (r.reservoirMetadata.x >> 8u) & 0xffu;
}

uint restir_di_visibility(RestirDiReservoir r) {
    return (r.reservoirMetadata.x >> 16u) & 3u;
}

float restir_di_source_pdf(RestirDiReservoir r) {
    return max(unpackHalf2x16(r.reservoirMetadata.y).x, 1.0e-6);
}

float restir_di_previous_weight(RestirDiReservoir r) {
    return unpackHalf2x16(r.reservoirMetadata.y).y;
}

float restir_di_target(RestirDiReservoir r) {
    return max(r.sampleRadiance_target.w, 0.0);
}

float restir_di_weight_sum(RestirDiReservoir r) {
    return max(r.sampleNormal_weightSum.w, 0.0);
}

float restir_di_confidence(RestirDiReservoir r) {
    return clamp(r.contribution_confidence.w, 0.0, 1.0);
}

uint restir_di_light_id(RestirDiReservoir r) {
    return r.sampleMetadata.x;
}

uint restir_di_light_kind(RestirDiReservoir r) {
    return r.sampleMetadata.y;
}

uint restir_di_light_version(RestirDiReservoir r) {
    return r.sampleMetadata.z;
}

uint restir_di_compat_signature(RestirDiReservoir r) {
    return r.reservoirMetadata.z;
}

uint restir_di_rejection_flags(RestirDiReservoir r) {
    return r.reservoirMetadata.w;
}

// ---------------------------------------------------------------------------
// Reservoir setters (ProductionPacked)
// ---------------------------------------------------------------------------

void restir_di_set_valid(inout RestirDiReservoir r, bool valid) {
    if (valid) {
        r.reservoirMetadata.x |= (1u << 18u);
    } else {
        r.reservoirMetadata.x &= ~(1u << 18u);
    }
}

void restir_di_set_age(inout RestirDiReservoir r, uint age) {
    r.reservoirMetadata.x = (r.reservoirMetadata.x & ~0xffu) | min(age, 255u);
}

void restir_di_set_m(inout RestirDiReservoir r, uint m) {
    r.reservoirMetadata.x = (r.reservoirMetadata.x & ~0xff00u) | (min(m, 255u) << 8u);
}

void restir_di_set_visibility(inout RestirDiReservoir r, uint vis) {
    r.reservoirMetadata.x = (r.reservoirMetadata.x & ~0x30000u) | ((vis & 3u) << 16u);
}

void restir_di_set_source_pdf(inout RestirDiReservoir r, float pdf) {
    float prevWeight = restir_di_previous_weight(r);
    r.reservoirMetadata.y = restir_di_pack_pdf_weight(pdf, prevWeight);
}

void restir_di_set_previous_weight(inout RestirDiReservoir r, float w) {
    float pdf = restir_di_source_pdf(r);
    r.reservoirMetadata.y = restir_di_pack_pdf_weight(pdf, w);
}

void restir_di_set_weight_sum(inout RestirDiReservoir r, float ws) {
    r.sampleNormal_weightSum.w = clamp(ws, 0.0, 65504.0);
}

void restir_di_set_rejection_flags(inout RestirDiReservoir r, uint flags) {
    r.reservoirMetadata.w = flags;
}

// ---------------------------------------------------------------------------
// Target function: p_hat = max(luminance(radiance), epsilon)
// This is the scalar target used for reservoir resampling.
// ---------------------------------------------------------------------------
float restir_di_target_function(vec3 radiance) {
    return max(dot(radiance, vec3(0.2126, 0.7152, 0.0722)), 1.0e-6);
}

// ---------------------------------------------------------------------------
// Surface compatibility check between two receivers
// ---------------------------------------------------------------------------
bool restir_di_surface_compatible(
    RestirDiReceiver center, RestirDiReceiver candidate,
    float normalThreshold, float depthThreshold) {
    // Reject sky/invalid surfaces
    if ((center.primitive_mesh_flags.z & (RESTIR_DI_SURFACE_SKY | RESTIR_DI_SURFACE_INVALID)) != 0u) return false;
    if ((candidate.primitive_mesh_flags.z & (RESTIR_DI_SURFACE_SKY | RESTIR_DI_SURFACE_INVALID)) != 0u) return false;

    // Material ID match
    if (center.tangent_materialId.w != 0xffffffffu &&
        candidate.tangent_materialId.w != 0xffffffffu &&
        center.tangent_materialId.w != candidate.tangent_materialId.w) return false;

    // Instance ID match
    if (center.bitangent_instanceId.w != 0xffffffffu &&
        candidate.bitangent_instanceId.w != 0xffffffffu &&
        center.bitangent_instanceId.w != candidate.bitangent_instanceId.w) return false;

    // Depth check
    float depthDelta = abs(candidate.worldPosition_depth.w - center.worldPosition_depth.w);
    float maxDepth = max(abs(center.worldPosition_depth.w), 1.0e-3);
    if (depthDelta / maxDepth > depthThreshold) return false;

    // Normal check
    float normalDot = dot(normalize(center.normal_roughness.xyz), normalize(candidate.normal_roughness.xyz));
    if (normalDot < normalThreshold) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Temporal signature: compact surface/material identity for matching
// ---------------------------------------------------------------------------
uint restir_di_compute_temporal_signature(uint lightKind, float roughness, uint materialId, uint instanceId) {
    uint r = uint(clamp(round(clamp(roughness, 0.0, 1.0) * 255.0), 0.0, 255.0));
    return lightKind | (r << 8u) | ((materialId & 0xffu) << 16u) | ((instanceId & 0xffu) << 24u);
}

// ---------------------------------------------------------------------------
// Reservoir update (core resampling logic)
// Implements the weighted reservoir sampling update:
//   W' = W + w_candidate
//   select with probability w_candidate / W'
//   M' = M + M_candidate
// ---------------------------------------------------------------------------
void restir_di_reservoir_update(
    inout RestirDiReservoir reservoir,
    RestirDiReservoir candidate,
    float candidateWeight,
    float candidateM,
    float randomValue,
    inout uint rejectionFlags) {

    float currentWeight = restir_di_weight_sum(reservoir);
    float totalWeight = currentWeight + candidateWeight;

    if (totalWeight <= 1.0e-8 || isnan(totalWeight) || isinf(totalWeight)) {
        rejectionFlags |= RESTIR_DI_REJECT_NAN;
        return;
    }

    float selectProbability = candidateWeight / totalWeight;

    if (randomValue < selectProbability) {
        reservoir = candidate;
    }

    restir_di_set_weight_sum(reservoir, min(totalWeight, 65504.0));
    restir_di_set_m(reservoir, min(restir_di_m(reservoir) + uint(candidateM), 255u));
}

// ---------------------------------------------------------------------------
// Shift a reservoir sample from source receiver to center receiver
// Returns the shifted weight using:
//   w_shift = p_hat(center, y) * source.weightSum / max(p_hat(source, y), epsilon)
// ---------------------------------------------------------------------------
float restir_di_shift_weight(
    float targetAtCenter,
    float targetAtSource,
    float sourceWeightSum) {
    if (targetAtSource <= 1.0e-6) return 0.0;
    return targetAtCenter * sourceWeightSum / targetAtSource;
}

// ---------------------------------------------------------------------------
// Production stabilization: temporal luminance limit
// ---------------------------------------------------------------------------
float restir_di_temporal_luminance_limit(vec3 currentDirect, float maxFactor) {
    float currentLum = dot(currentDirect, vec3(0.2126, 0.7152, 0.0722));
    if (currentLum <= 0.0) return 1.0;
    float limit = max(currentLum * maxFactor, 0.01);
    return limit;
}

// ---------------------------------------------------------------------------
// Final estimate computation:
//   L = f(x, selected) * W / max(M * p_hat(x, selected), epsilon)
// ---------------------------------------------------------------------------
vec3 restir_di_final_estimate(
    vec3 radiance,
    float weightSum,
    float m,
    float target) {
    float denom = max(m * target, 1.0e-6);
    if (isnan(denom) || isinf(denom) || denom <= 0.0) return vec3(0.0);
    return max(radiance, vec3(0.0)) * (weightSum / denom);
}

// ---------------------------------------------------------------------------
// Shared Params UBO — must match C++ RestirDiParams exactly (std140)
// ---------------------------------------------------------------------------
struct RestirDiUniforms {
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
    uint useFallbackInitial;
    uint spatialResultValid;
    uint padding0;
    uint padding1;
};

#endif // RTV_RESTIR_DI_COMMON_GLSL
