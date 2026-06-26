#ifndef RTV_RESTIR_DI_REUSE_GLSL
#define RTV_RESTIR_DI_REUSE_GLSL

// Temporal/spatial compatibility, reservoir updates, shifting, and final estimate helpers.
uint restir_di_compat_signature(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return r.reservoirMetadata.z;
#else
    return r.sampleMetadata.x;
#endif
}

vec2 restir_di_oct_encode(vec3 value) {
    vec3 n = normalize(value);
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
}

vec3 restir_di_oct_decode(vec2 value) {
    vec3 n = vec3(value, 1.0 - abs(value.x) - abs(value.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}

vec3 restir_di_light_normal(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return normalize(r.sampleNormal_weightSum.xyz);
#else
    return restir_di_oct_decode(unpackSnorm2x16(r.reservoirMetadata.z));
#endif
}

uint restir_di_rejection_flags(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return r.reservoirMetadata.w;
#else
    return (r.reservoirMetadata.x >> 19u) & 0xffu;
#endif
}

bool restir_di_receiver_id_valid(float id) {
    return id >= 0.0 && id < RESTIR_DI_INVALID_ID_FLOAT;
}

uint restir_di_receiver_id(float id) {
    return uint(clamp(floor(id + 0.5), 0.0, RESTIR_DI_INVALID_ID_FLOAT - 1.0));
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
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleDirection_pdf.w = max(pdf, 1.0e-6);
#endif
    float prevWeight = restir_di_previous_weight(r);
    r.reservoirMetadata.y = restir_di_pack_pdf_weight(pdf, prevWeight);
}

void restir_di_set_previous_weight(inout RestirDiReservoir r, float w) {
    float pdf = restir_di_source_pdf(r);
    r.reservoirMetadata.y = restir_di_pack_pdf_weight(pdf, w);
}

void restir_di_set_weight_sum(inout RestirDiReservoir r, float ws) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleNormal_weightSum.w = max(ws, 0.0);
#else
    vec2 targetWeight = unpackHalf2x16(r.reservoirMetadata.w);
    r.reservoirMetadata.w = packHalf2x16(vec2(targetWeight.x, clamp(ws, 0.0, 65504.0)));
#endif
}

void restir_di_set_target(inout RestirDiReservoir r, float target) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleRadiance_target.w = max(target, 0.0);
#else
    vec2 targetWeight = unpackHalf2x16(r.reservoirMetadata.w);
    r.reservoirMetadata.w = packHalf2x16(vec2(clamp(target, 0.0, 65504.0), targetWeight.y));
#endif
}

void restir_di_set_sample_radiance(inout RestirDiReservoir r, vec3 radiance) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleRadiance_target.rgb = max(radiance, vec3(0.0));
#else
    r.sampleMetadata.w = restir_di_pack_radiance(radiance);
#endif
}

void restir_di_set_direction(inout RestirDiReservoir r, vec3 direction) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleDirection_pdf.xyz = normalize(direction);
#endif
}

void restir_di_set_light_normal(inout RestirDiReservoir r, vec3 normal) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleNormal_weightSum.xyz = normalize(normal);
#else
    r.reservoirMetadata.z = packSnorm2x16(restir_di_oct_encode(normal));
#endif
}

void restir_di_set_confidence(inout RestirDiReservoir r, float confidence) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.contribution_confidence.w = clamp(confidence, 0.0, 1.0);
#else
    uint packedConfidence = uint(round(clamp(confidence, 0.0, 1.0) * 31.0));
    r.reservoirMetadata.x = (r.reservoirMetadata.x & 0x07ffffffu) |
        (packedConfidence << 27u);
#endif
}

void restir_di_set_rejection_flags(inout RestirDiReservoir r, uint flags) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.reservoirMetadata.w = flags;
#else
    r.reservoirMetadata.x = (r.reservoirMetadata.x & ~(0xffu << 19u)) |
        ((flags & 0xffu) << 19u);
#endif
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
    const uint invalidMask = RESTIR_DI_SURFACE_SKY | RESTIR_DI_SURFACE_INVALID |
        RESTIR_DI_SURFACE_DELTA | RESTIR_DI_SURFACE_ALPHA |
        RESTIR_DI_SURFACE_UNLIT | RESTIR_DI_SURFACE_UNSUPPORTED;
    if ((restir_di_receiver_surface_flags(center) & invalidMask) != 0u) return false;
    if ((restir_di_receiver_surface_flags(candidate) & invalidMask) != 0u) return false;

    // Material/instance identity must agree across both packed and validation ABIs.
    if (restir_di_receiver_surface_signature(center) !=
        restir_di_receiver_surface_signature(candidate)) return false;

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
bool restir_di_reservoir_update(
    inout RestirDiReservoir reservoir,
    RestirDiReservoir candidate,
    float candidateWeight,
    float candidateM,
    float randomValue,
    inout uint rejectionFlags) {

    float currentWeight = restir_di_weight_sum(reservoir);
    uint currentM = restir_di_m(reservoir);
    uint mergedM = min(currentM + uint(candidateM), 255u);
    float totalWeight = currentWeight + candidateWeight;

    if (totalWeight <= 1.0e-8 || isnan(totalWeight) || isinf(totalWeight)) {
        rejectionFlags |= RESTIR_DI_REJECT_NAN;
        return false;
    }

    float selectProbability = candidateWeight / totalWeight;

    if (randomValue < selectProbability) {
        reservoir = candidate;
    }

    restir_di_set_weight_sum(reservoir, totalWeight);
    restir_di_set_m(reservoir, mergedM);
    return true;
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

#endif // RTV_RESTIR_DI_REUSE_GLSL
