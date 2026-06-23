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
#if RTV_RESTIR_DI_VALIDATION_FULL
    vec4 sampleDirection_pdf;
    vec4 sampleRadiance_target;
    vec4 sampleNormal_weightSum;
    vec4 contribution_confidence;
#endif
};
#endif

#ifndef RTV_RESTIR_DI_RECEIVER_DEFINED
#define RTV_RESTIR_DI_RECEIVER_DEFINED
struct RestirDiReceiver {
    vec4 worldPosition_depth;
    vec4 normal_roughness;
#if RTV_RESTIR_DI_VALIDATION_FULL
    vec4 tangent_materialId;
    vec4 bitangent_instanceId;
    vec4 viewDirection_hitDist;
    uvec4 primitive_mesh_flags;
#else
    uvec4 packedMaterialSurface;
#endif
};
#endif

vec2 restir_di_receiver_oct_encode(vec3 value) {
    vec3 n = normalize(value);
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
}

vec3 restir_di_receiver_oct_decode(vec2 value) {
    vec3 n = vec3(value, 1.0 - abs(value.x) - abs(value.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}

vec3 restir_di_receiver_base_color(RestirDiReceiver receiver) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(receiver.tangent_materialId.xyz, vec3(0.0));
#else
    return unpackUnorm4x8(receiver.packedMaterialSurface.x).xyz;
#endif
}

vec3 restir_di_receiver_specular_color(RestirDiReceiver receiver) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return clamp(receiver.bitangent_instanceId.xyz, vec3(0.0), vec3(1.0));
#else
    return unpackUnorm4x8(receiver.packedMaterialSurface.y).xyz;
#endif
}

float restir_di_receiver_metallic(RestirDiReceiver receiver) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return clamp(uintBitsToFloat(receiver.primitive_mesh_flags.w), 0.0, 1.0);
#else
    return unpackUnorm4x8(receiver.packedMaterialSurface.x).w;
#endif
}

uint restir_di_receiver_surface_flags(RestirDiReceiver receiver) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return receiver.primitive_mesh_flags.z;
#else
    return uint(round(unpackUnorm4x8(receiver.packedMaterialSurface.y).w * 255.0));
#endif
}

uint restir_di_receiver_surface_signature(RestirDiReceiver receiver) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    uint materialId = uint(clamp(floor(receiver.tangent_materialId.w + 0.5), 0.0, 4294967040.0));
    uint instanceId = uint(clamp(floor(receiver.bitangent_instanceId.w + 0.5), 0.0, 4294967040.0));
    return materialId * 747796405u ^ instanceId * 2891336453u;
#else
    return receiver.packedMaterialSurface.z;
#endif
}

vec3 restir_di_receiver_view_direction(RestirDiReceiver receiver) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return normalize(receiver.viewDirection_hitDist.xyz);
#else
    return restir_di_receiver_oct_decode(unpackSnorm2x16(receiver.packedMaterialSurface.w));
#endif
}

struct RestirDiLightRecord {
    uvec4 metadata;
    uvec4 identity;
    vec4 data0;
    vec4 data1;
    vec4 data2;
    vec4 data3;
};
layout(set = 0, binding = 12, std430) readonly buffer RestirDiSceneLightRecords {
    RestirDiLightRecord restir_di_light_records[];
};
layout(set = 0, binding = 13, std140) uniform RestirDiSceneParams {
    uint vertexCount;
    uint triangleCount;
    uint bvhNodeCount;
    uint materialCount;
    uint enabled;
    uint sphereCount;
    uint primitiveCount;
    uint instanceCount;
    uint lightCount;
    float totalLightWeight;
    uint meshCount;
    uint localVertexCount;
    uint localIndexCount;
    uint localBvhNodeCount;
    uint localTriangleCount;
    uint tlasNodeCount;
    uint tlasInstanceIndexCount;
    uint authoredLightOffset;
    uint authoredLightCount;
    uint padding4;
} restir_di_scene;

// Surface flags
#ifndef RTV_RESTIR_DI_SURFACE_CONSTANTS_DEFINED
#define RTV_RESTIR_DI_SURFACE_CONSTANTS_DEFINED
const uint RESTIR_DI_SURFACE_SKY       = 1u << 0u;
const uint RESTIR_DI_SURFACE_INVALID   = 1u << 1u;
const uint RESTIR_DI_SURFACE_DELTA     = 1u << 2u;
const uint RESTIR_DI_SURFACE_ALPHA     = 1u << 3u;
const uint RESTIR_DI_SURFACE_UNLIT     = 1u << 4u;
const uint RESTIR_DI_SURFACE_PBR       = 1u << 5u;
const uint RESTIR_DI_SURFACE_UNSUPPORTED = 1u << 6u;
#endif

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
#ifndef RTV_RESTIR_DI_VISIBILITY_CONSTANTS_DEFINED
#define RTV_RESTIR_DI_VISIBILITY_CONSTANTS_DEFINED
const uint RESTIR_DI_VISIBILITY_UNKNOWN  = 0u;
const uint RESTIR_DI_VISIBILITY_VISIBLE  = 1u;
const uint RESTIR_DI_VISIBILITY_OCCLUDED = 2u;
const uint RESTIR_DI_VISIBILITY_INVALID  = 3u;
#endif

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

const uint RESTIR_DI_COUNTER_INITIAL_PIXELS = 0u;
const uint RESTIR_DI_COUNTER_INITIAL_VALID = 1u;
const uint RESTIR_DI_COUNTER_INITIAL_INVALID_SURFACE = 2u;
const uint RESTIR_DI_COUNTER_INITIAL_INVALID_TARGET = 3u;
const uint RESTIR_DI_COUNTER_INITIAL_NON_FINITE = 4u;
const uint RESTIR_DI_COUNTER_INITIAL_EMISSIVE = 5u;
const uint RESTIR_DI_COUNTER_INITIAL_DIRECTIONAL = 6u;
const uint RESTIR_DI_COUNTER_INITIAL_POINT = 7u;
const uint RESTIR_DI_COUNTER_INITIAL_AREA = 8u;
const uint RESTIR_DI_COUNTER_INITIAL_SPOT = 9u;
const uint RESTIR_DI_COUNTER_INITIAL_INVALID_PDF = 10u;
const uint RESTIR_DI_COUNTER_INITIAL_INVALID_IDENTITY = 11u;
const uint RESTIR_DI_COUNTER_INITIAL_WEIGHT_OVERFLOW = 12u;
const uint RESTIR_DI_COUNTER_TEMPORAL_NON_FINITE = 13u;
const uint RESTIR_DI_COUNTER_SUM_M_LOW = 14u;
const uint RESTIR_DI_COUNTER_SUM_M_HIGH = 15u;

const uint RESTIR_DI_COUNTER_TEMPORAL_PIXELS = 16u;
const uint RESTIR_DI_COUNTER_TEMPORAL_REPROJECT_VALID = 17u;
const uint RESTIR_DI_COUNTER_TEMPORAL_REPROJECT_INVALID = 18u;
const uint RESTIR_DI_COUNTER_TEMPORAL_PREVIOUS_VALID = 19u;
const uint RESTIR_DI_COUNTER_TEMPORAL_PREVIOUS_INVALID = 20u;
const uint RESTIR_DI_COUNTER_TEMPORAL_SURFACE_REJECTED = 21u;
const uint RESTIR_DI_COUNTER_TEMPORAL_AGE_REJECTED = 22u;
const uint RESTIR_DI_COUNTER_TEMPORAL_LIGHT_VERSION_REJECTED = 23u;
const uint RESTIR_DI_COUNTER_TEMPORAL_TARGET_REJECTED = 24u;
const uint RESTIR_DI_COUNTER_TEMPORAL_PDF_REJECTED = 25u;
const uint RESTIR_DI_COUNTER_TEMPORAL_VISIBILITY_SKIPPED = 26u;
const uint RESTIR_DI_COUNTER_TEMPORAL_VISIBILITY_PASSED = 27u;
const uint RESTIR_DI_COUNTER_TEMPORAL_VISIBILITY_FAILED = 28u;
const uint RESTIR_DI_COUNTER_TEMPORAL_ACCEPTED_PREVIOUS = 29u;
const uint RESTIR_DI_COUNTER_TEMPORAL_ACCEPTED_CURRENT = 30u;
const uint RESTIR_DI_COUNTER_TEMPORAL_CLAMPED = 31u;

const uint RESTIR_DI_COUNTER_SPATIAL_PIXELS = 32u;
const uint RESTIR_DI_COUNTER_SPATIAL_NEIGHBORS_TESTED = 33u;
const uint RESTIR_DI_COUNTER_SPATIAL_NEIGHBORS_ACCEPTED = 34u;
const uint RESTIR_DI_COUNTER_SPATIAL_SURFACE_REJECTED = 35u;
const uint RESTIR_DI_COUNTER_SPATIAL_TARGET_REJECTED = 36u;
const uint RESTIR_DI_COUNTER_SPATIAL_VISIBILITY_REJECTED = 37u;
const uint RESTIR_DI_COUNTER_SPATIAL_SELECTED_CENTER = 38u;
const uint RESTIR_DI_COUNTER_SPATIAL_SELECTED_NEIGHBOR = 39u;
const uint RESTIR_DI_COUNTER_SPATIAL_CLAMPED = 40u;
const uint RESTIR_DI_COUNTER_SPATIAL_ROUNDS = 41u;
const uint RESTIR_DI_COUNTER_SPATIAL_LIGHT_REJECTED = 42u;
const uint RESTIR_DI_COUNTER_SUM_AGE_LOW = 43u;
const uint RESTIR_DI_COUNTER_SUM_AGE_HIGH = 44u;
const uint RESTIR_DI_COUNTER_SUM_WEIGHT_LOW = 45u;
const uint RESTIR_DI_COUNTER_SUM_WEIGHT_HIGH = 46u;
const uint RESTIR_DI_COUNTER_SUM_LUMINANCE_LOW = 47u;

const uint RESTIR_DI_COUNTER_FINAL_PIXELS = 48u;
const uint RESTIR_DI_COUNTER_FINAL_VALID = 49u;
const uint RESTIR_DI_COUNTER_FINAL_INVALID = 50u;
const uint RESTIR_DI_COUNTER_FINAL_FALLBACK = 51u;
const uint RESTIR_DI_COUNTER_FINAL_VISIBILITY_CHECKED = 52u;
const uint RESTIR_DI_COUNTER_FINAL_VISIBLE = 53u;
const uint RESTIR_DI_COUNTER_FINAL_OCCLUDED = 54u;
const uint RESTIR_DI_COUNTER_FINAL_CLAMPED = 55u;
const uint RESTIR_DI_COUNTER_FINAL_ESTIMATE = 56u;
const uint RESTIR_DI_COUNTER_FINAL_NON_FINITE = 57u;
const uint RESTIR_DI_COUNTER_FINAL_UNKNOWN_VISIBILITY = 58u;
const uint RESTIR_DI_COUNTER_FINAL_LIGHT_REJECTED = 59u;
const uint RESTIR_DI_COUNTER_SUM_LUMINANCE_HIGH = 60u;
const uint RESTIR_DI_COUNTER_CAPACITY = 64u;

const float RESTIR_DI_INVALID_ID_FLOAT = 4294967040.0;

uint restir_di_pack_radiance(vec3 value) {
    uvec3 encoded = uvec3(round(clamp(log2(vec3(1.0) + max(value, vec3(0.0))) / 16.0, 0.0, 1.0) * 1023.0));
    return encoded.x | (encoded.y << 10u) | (encoded.z << 20u);
}

vec3 restir_di_unpack_radiance(uint packed) {
    uvec3 encoded = uvec3(packed & 1023u, (packed >> 10u) & 1023u, (packed >> 20u) & 1023u);
    return exp2(vec3(encoded) * (16.0 / 1023.0)) - vec3(1.0);
}

// ---------------------------------------------------------------------------
// Reservoir accessors (ProductionPacked)
// ---------------------------------------------------------------------------

RestirDiReservoir restir_di_empty_reservoir() {
    RestirDiReservoir r;
    r.sampleMetadata = uvec4(0u);
    r.reservoirMetadata = uvec4(0u);
    r.samplePosition_distance = vec4(0.0);
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleDirection_pdf = vec4(0.0, 0.0, 1.0, 1.0e-6);
    r.sampleRadiance_target = vec4(0.0);
    r.sampleNormal_weightSum = vec4(0.0, 1.0, 0.0, 0.0);
    r.contribution_confidence = vec4(0.0);
#else
    r.sampleMetadata.w = restir_di_pack_radiance(vec3(0.0));
    r.reservoirMetadata.z = packSnorm2x16(vec2(0.0));
    r.reservoirMetadata.w = packHalf2x16(vec2(0.0));
#endif
    r.reservoirMetadata.y = restir_di_pack_pdf_weight(1.0e-6, 0.0);
    return r;
}

bool restir_di_reservoir_valid(RestirDiReservoir r) {
    bool stateValid = (r.reservoirMetadata.x & (1u << 18u)) != 0u;
    uint m = (r.reservoirMetadata.x >> 8u) & 0xffu;
#if RTV_RESTIR_DI_VALIDATION_FULL
    float target = r.sampleRadiance_target.w;
    float weightSum = r.sampleNormal_weightSum.w;
    float sourcePdf = r.sampleDirection_pdf.w;
    vec3 radiance = r.sampleRadiance_target.rgb;
#else
    vec2 targetWeight = unpackHalf2x16(r.reservoirMetadata.w);
    float target = targetWeight.x;
    float weightSum = targetWeight.y;
    float sourcePdf = unpackHalf2x16(r.reservoirMetadata.y).x;
    vec3 radiance = restir_di_unpack_radiance(r.sampleMetadata.w);
#endif
    return stateValid && m > 0u && target > 0.0 && weightSum > 0.0 && sourcePdf > 0.0 &&
        !isnan(target) && !isinf(target) && !isnan(weightSum) && !isinf(weightSum) &&
        !isnan(sourcePdf) && !isinf(sourcePdf) &&
        !any(isnan(r.samplePosition_distance)) && !any(isinf(r.samplePosition_distance)) &&
        !any(isnan(radiance)) && !any(isinf(radiance));
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
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleDirection_pdf.w, 1.0e-6);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.y).x, 1.0e-6);
#endif
}

float restir_di_previous_weight(RestirDiReservoir r) {
    return unpackHalf2x16(r.reservoirMetadata.y).y;
}

float restir_di_target(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleRadiance_target.w, 0.0);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.w).x, 0.0);
#endif
}

float restir_di_weight_sum(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleNormal_weightSum.w, 0.0);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.w).y, 0.0);
#endif
}

vec3 restir_di_sample_radiance(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleRadiance_target.rgb, vec3(0.0));
#else
    return max(restir_di_unpack_radiance(r.sampleMetadata.w), vec3(0.0));
#endif
}

float restir_di_confidence(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return clamp(r.contribution_confidence.w, 0.0, 1.0);
#else
    return float((r.reservoirMetadata.x >> 27u) & 0x1fu) / 31.0;
#endif
}

uint restir_di_light_id(RestirDiReservoir r) {
    return r.sampleMetadata.x;
}

uint restir_di_light_kind(RestirDiReservoir r) {
    return r.sampleMetadata.z & 0xffu;
}

uint restir_di_light_index(RestirDiReservoir r) {
    return r.sampleMetadata.z >> 8u;
}

uint restir_di_light_version(RestirDiReservoir r) {
    return r.sampleMetadata.y;
}

uint restir_di_identity_hash(uvec2 identity) {
    uint h = identity.x ^ (identity.y + 0x9e3779b9u + (identity.x << 6u) + (identity.x >> 2u));
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    return h ^ (h >> 16u);
}

bool restir_di_light_identity_matches(RestirDiReservoir reservoir, RestirDiLightRecord light) {
    return reservoir.sampleMetadata.x == restir_di_identity_hash(light.identity.xy) &&
        reservoir.sampleMetadata.y == light.identity.z &&
        restir_di_light_kind(reservoir) == light.metadata.x;
}


bool restir_di_resolve_light(
    RestirDiReservoir reservoir,
    out RestirDiLightRecord light,
    out uint resolvedIndex) {
    uint cachedIndex = restir_di_light_index(reservoir);
    if (cachedIndex < restir_di_scene.lightCount &&
        restir_di_light_identity_matches(reservoir, restir_di_light_records[cachedIndex])) {
        resolvedIndex = cachedIndex;
        light = restir_di_light_records[cachedIndex];
        return true;
    }
    // Light/distribution changes invalidate DI history on the CPU. Avoid an
    // unbounded per-pixel scan when a stale cached index does not match.
    resolvedIndex = 0u;
    light.metadata = uvec4(0u);
    light.identity = uvec4(0u);
    light.data0 = vec4(0.0);
    light.data1 = vec4(0.0);
    light.data2 = vec4(0.0);
    light.data3 = vec4(0.0);
    return false;
}

float restir_di_light_selection_pdf(uint lightIndex, RestirDiLightRecord light) {
    if (restir_di_scene.lightCount == 0u || restir_di_scene.totalLightWeight <= 1.0e-8) return 0.0;
    float authoredProbability = restir_di_scene.authoredLightCount == 0u
        ? 0.0
        : (restir_di_scene.lightCount > restir_di_scene.authoredLightCount ? 0.5 : 1.0);
    float pdf = (1.0 - authoredProbability) * max(light.data0.x, 0.0) /
        max(restir_di_scene.totalLightWeight, 1.0e-6);
    if (light.metadata.x >= 2u && restir_di_scene.authoredLightCount > 0u) {
        pdf += authoredProbability / float(restir_di_scene.authoredLightCount);
    }
    return pdf;
}

float restir_di_pdf_at_receiver(
    uint lightIndex,
    RestirDiLightRecord light,
    vec3 receiverPosition,
    vec3 samplePosition,
    vec3 lightNormal) {
    float selectionPdf = restir_di_light_selection_pdf(lightIndex, light);
    uint kind = light.metadata.x;
    if (kind == 2u || kind == 3u || kind == 5u) return selectionPdf;
    vec3 toLight = samplePosition - receiverPosition;
    float distanceSquared = dot(toLight, toLight);
    vec3 direction = toLight * inversesqrt(max(distanceSquared, 1.0e-8));
    float cosineAtLight = max(dot(-direction, normalize(lightNormal)), 0.0);
    float area = max(light.data0.w, 1.0e-6);
    return cosineAtLight > 1.0e-6 ? selectionPdf * distanceSquared / (cosineAtLight * area) : 0.0;
}

vec3 restir_di_light_normal(RestirDiReservoir r);

vec3 restir_di_evaluate_integrand(
    RestirDiReservoir reservoir,
    RestirDiReceiver current,
    RestirDiLightRecord light,
    float lightPdf,
    out vec3 currentDirection,
    out float currentDistance,
    out vec3 currentLightNormal) {
    uint kind = restir_di_light_kind(reservoir);
    vec3 samplePosition = reservoir.samplePosition_distance.xyz;
    if (kind == 2u) {
        currentDirection = normalize(light.data1.xyz);
        currentDistance = 10000.0;
    } else {
        vec3 toLight = samplePosition - current.worldPosition_depth.xyz;
        currentDistance = length(toLight);
        currentDirection = toLight / max(currentDistance, 1.0e-6);
    }
    currentLightNormal = restir_di_light_normal(reservoir);
    if (kind == 2u || kind == 3u) currentLightNormal = -currentDirection;
    if (kind == 4u || kind == 5u) {
        currentLightNormal = normalize(vec3(light.data1.w, light.data2.w, light.data3.x));
    }

    // Emissive triangle/sphere samples retain their sampled radiance. This is
    // required for textured triangle emission and avoids replacing it with a
    // light-record average during reuse.
    vec3 incidentRadiance = restir_di_sample_radiance(reservoir);
    if (kind == 2u || kind == 4u) {
        incidentRadiance = max(light.data2.xyz, vec3(0.0));
    } else if (kind == 3u) {
        incidentRadiance = max(light.data2.xyz, vec3(0.0)) /
            max(currentDistance * currentDistance, 1.0e-4);
    } else if (kind == 5u) {
        vec3 spotDirection = currentLightNormal;
        float cosInner = cos(clamp(light.data3.y, 0.0, 3.14159265));
        float cosOuter = cos(clamp(max(light.data3.z, light.data3.y), 0.0, 3.14159265));
        float cone = clamp((dot(-currentDirection, spotDirection) - cosOuter) /
            max(cosInner - cosOuter, 1.0e-4), 0.0, 1.0);
        incidentRadiance = max(light.data2.xyz, vec3(0.0)) * (cone * cone) /
            max(currentDistance * currentDistance, 1.0e-4);
    }

    vec3 normal = normalize(current.normal_roughness.xyz);
    vec3 view = restir_di_receiver_view_direction(current);
    float currentCosine = max(dot(normalize(current.normal_roughness.xyz), currentDirection), 0.0);
    if (currentCosine <= 0.0 || dot(incidentRadiance, vec3(0.2126, 0.7152, 0.0722)) <= 0.0) return vec3(0.0);

    vec3 baseColor = restir_di_receiver_base_color(current);
    vec3 f0 = restir_di_receiver_specular_color(current);
    float metallic = restir_di_receiver_metallic(current);
    float roughness = clamp(current.normal_roughness.w, 0.001, 1.0);
    vec3 halfVectorSum = view + currentDirection;
    if (dot(halfVectorSum, halfVectorSum) <= 1.0e-12) return vec3(0.0);
    vec3 halfVector = normalize(halfVectorSum);
    float nDotV = max(dot(normal, view), 1.0e-4);
    float nDotL = currentCosine;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float vDotH = max(dot(view, halfVector), 0.0);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / max(3.14159265 * denom * denom, 1.0e-6);
    float lambdaV = 0.5 * (sqrt(1.0 + alpha2 * max(1.0 - nDotV * nDotV, 0.0) /
        max(nDotV * nDotV, 1.0e-8)) - 1.0);
    float lambdaL = 0.5 * (sqrt(1.0 + alpha2 * max(1.0 - nDotL * nDotL, 0.0) /
        max(nDotL * nDotL, 1.0e-8)) - 1.0);
    float geometry = 1.0 / max(1.0 + lambdaV + lambdaL, 1.0e-8);
    vec3 fresnel = f0 + (vec3(1.0) - f0) * pow(1.0 - vDotH, 5.0);
    vec3 specular = fresnel * distribution * geometry /
        max(4.0 * nDotV * nDotL, 1.0e-5);

    float sigma = roughness * 1.2217304764;
    float sigma2 = sigma * sigma;
    float orenA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    float orenB = 0.45 * sigma2 / (sigma2 + 0.09);
    vec3 viewTangent = view - normal * nDotV;
    vec3 lightTangent = currentDirection - normal * nDotL;
    float viewLen2 = dot(viewTangent, viewTangent);
    float lightLen2 = dot(lightTangent, lightTangent);
    float cosPhiDiff = viewLen2 > 1.0e-8 && lightLen2 > 1.0e-8
        ? dot(viewTangent, lightTangent) * inversesqrt(viewLen2 * lightLen2) : 0.0;
    float sinAlpha = sqrt(max(1.0 - max(nDotV, nDotL) * max(nDotV, nDotL), 0.0));
    float tanBeta = sqrt(max(1.0 - min(nDotV, nDotL) * min(nDotV, nDotL), 0.0)) /
        max(min(nDotV, nDotL), 1.0e-4);
    float orenFactor = roughness <= 0.08 ? 1.0 :
        clamp(orenA + orenB * max(cosPhiDiff, 0.0) * sinAlpha * tanBeta, 0.0, 1.0);
    vec3 averageFresnel = f0 + (vec3(1.0) - f0) * (1.0 / 21.0);
    vec3 diffuseEnergy = clamp(vec3(1.0) - averageFresnel, vec3(0.0), vec3(1.0)) * (1.0 - metallic);
    vec3 diffuse = diffuseEnergy * baseColor * (orenFactor / 3.14159265);

    float eV = (nDotV * (1.0 + alpha2)) /
        max(nDotV * (1.0 + alpha2) + alpha * (1.0 - nDotV), 1.0e-8);
    float eL = (nDotL * (1.0 + alpha2)) /
        max(nDotL * (1.0 + alpha2) + alpha * (1.0 - nDotL), 1.0e-8);
    float eAvg = clamp(1.0 / (1.0 + alpha * 0.66), 0.0, 1.0);
    vec3 fMs = averageFresnel * averageFresnel /
        max(vec3(1.0) - averageFresnel * (1.0 - eAvg), vec3(1.0e-4));
    specular += fMs * (1.0 - eV) * (1.0 - eL) /
        max(3.14159265 * eV * eL, 1.0e-8);

    bool pbrClosure = (restir_di_receiver_surface_flags(current) & RESTIR_DI_SURFACE_PBR) != 0u;
    vec3 bsdf = pbrClosure ? diffuse + specular : baseColor * (orenFactor / 3.14159265);

    float diffusePdf = nDotL / 3.14159265;
    float smithG1V = 2.0 * nDotV /
        max(nDotV + sqrt(alpha2 + (1.0 - alpha2) * nDotV * nDotV), 1.0e-8);
    float specularPdf = distribution * smithG1V / max(4.0 * nDotV, 1.0e-8);
    float fresnelLum = dot(f0 + (vec3(1.0) - f0) * pow(1.0 - nDotV, 5.0),
        vec3(0.2126, 0.7152, 0.0722));
    float diffuseLum = dot(diffuseEnergy * baseColor * (1.0 / 3.14159265),
        vec3(0.2126, 0.7152, 0.0722));
    float specularProbability = clamp(fresnelLum / max(fresnelLum + diffuseLum, 1.0e-6), 0.05, 0.95);
    float bsdfPdf = pbrClosure ? mix(diffusePdf, specularPdf, specularProbability) : diffusePdf;
    float lightPdf2 = lightPdf * lightPdf;
    float misWeight = lightPdf2 / max(lightPdf2 + bsdfPdf * bsdfPdf, 1.0e-8);
    return max(bsdf * incidentRadiance * nDotL * misWeight, vec3(0.0));
}

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
    uint mode;
    uint spatialResultValid;
    uint visibilityRayBudget;
    uint historyValid;
    uint materialVisibilityFlags;
    uint counterEnabled;
    uint padding1;
    uint padding2;
};

const float RESTIR_DI_VISIBILITY_EPSILON = 0.001;

#if defined(RTV_RESTIR_DI_ENABLE_RAY_QUERY) && RTV_RESTIR_DI_ENABLE_RAY_QUERY
// Ray query visibility check. The caller must enable GL_EXT_ray_query and
// declare topLevelAS before calling this helper.
#ifndef RTV_DI_RAY_MASK_SHADOW
#define RTV_DI_RAY_MASK_SHADOW 0x02u
#endif

const uint RESTIR_DI_MATERIAL_STRIDE = 52u;
const uint RESTIR_DI_MATERIAL_TEXTURE_LIMIT = 1024u;
const uint RESTIR_DI_ALPHA_MODE_OPAQUE = 0u;
const uint RESTIR_DI_ALPHA_MODE_MASK = 1u;
const uint RESTIR_DI_ALPHA_MODE_BLEND = 2u;
const uint RESTIR_DI_TEXTURE_TRANSFORM_BASE_COLOR = 0u;
const uint RESTIR_DI_TEXTURE_TRANSFORM_TRANSMISSION = 12u;
const uint RESTIR_DI_TEXTURE_TRANSFORM_COUNT = 17u;
const uint RESTIR_DI_TEXTURE_TRANSFORM_BASE = 18u;
const uint RESTIR_DI_MATERIAL_VISIBILITY_TRUST_OMM = 1u << 0u;

struct RestirDiInstanceRecord {
    mat4 transform;
    mat4 inverse_transform;
    mat4 normal_transform;
    mat4 prev_transform;
    uvec4 metadata;
};

struct RestirDiMeshRecord {
    uvec4 vertex_index_data;
    uvec4 primitive_data;
    uvec4 bvh_data;
    uvec4 flags;
};

struct RestirDiLocalVertex {
    vec4 position_uv_x;
    vec4 normal_uv_y;
    vec4 tangent;
    vec4 color;
    vec4 texcoord1;
};

struct RestirDiMaterialVisibility {
    uint dataIndex;
    vec3 color;
    float alphaFactor;
    int baseColorTexture;
    float alphaCutoff;
    uint alphaMode;
    uint doubleSided;
    float transmissionFactor;
    int transmissionTexture;
    int opacityTexture;
};

layout(set = 0, binding = 15, std430) readonly buffer RestirDiMeshMaterials {
    vec4 restir_di_mesh_materials[];
};
layout(set = 0, binding = 16, std430) readonly buffer RestirDiTriangleMaterialIds {
    uint restir_di_triangle_material_ids[];
};
layout(set = 0, binding = 17, std430) readonly buffer RestirDiInstanceRecords {
    RestirDiInstanceRecord restir_di_instance_records[];
};
layout(set = 0, binding = 18, std430) readonly buffer RestirDiMeshRecords {
    RestirDiMeshRecord restir_di_mesh_records[];
};
layout(set = 0, binding = 19, std430) readonly buffer RestirDiLocalVertices {
    RestirDiLocalVertex restir_di_local_vertices[];
};
layout(set = 0, binding = 20, std430) readonly buffer RestirDiLocalIndices {
    uint restir_di_local_indices[];
};
layout(set = 0, binding = 21, std430) readonly buffer RestirDiGeometryTriangleOffsets {
    uint restir_di_geometry_triangle_offsets[];
};
layout(set = 0, binding = 22, std430) readonly buffer RestirDiMeshGeometryRanges {
    uvec2 restir_di_mesh_geometry_ranges[];
};
layout(set = 0, binding = 41) uniform sampler2D restir_di_material_textures[];

RestirDiMaterialVisibility restir_di_decode_material_visibility(uint materialIndex) {
    uint materialCount = max(restir_di_scene.materialCount, 1u);
    uint idx = min(materialIndex, materialCount - 1u) * RESTIR_DI_MATERIAL_STRIDE;
    vec4 d0 = restir_di_mesh_materials[idx + 0u];
    vec4 d2 = restir_di_mesh_materials[idx + 2u];
    vec4 d3 = restir_di_mesh_materials[idx + 3u];
    vec4 d4 = restir_di_mesh_materials[idx + 4u];
    vec4 d12 = restir_di_mesh_materials[idx + 12u];
    vec4 d17 = restir_di_mesh_materials[idx + 17u];
    RestirDiMaterialVisibility material;
    material.dataIndex = idx;
    material.color = d0.xyz;
    material.alphaFactor = clamp(d2.w, 0.0, 1.0);
    material.baseColorTexture = int(round(d3.x));
    material.alphaCutoff = d4.x;
    material.alphaMode = uint(round(d4.y));
    material.doubleSided = uint(round(d4.z));
    material.transmissionFactor = clamp(d12.y, 0.0, 1.0);
    material.transmissionTexture = int(round(d12.z));
    material.opacityTexture = int(round(d17.x));
    return material;
}

uint restir_di_geometry_triangle_offset(uint meshIndex, uint geometryIndex, uint meshFirstIndex) {
    if (meshIndex >= restir_di_scene.meshCount) {
        return meshFirstIndex / 3u;
    }
    uvec2 range = restir_di_mesh_geometry_ranges[meshIndex];
    if (range.y == 0u) {
        return meshFirstIndex / 3u;
    }
    return restir_di_geometry_triangle_offsets[range.x + min(geometryIndex, range.y - 1u)];
}

uint restir_di_material_for_triangle_index(uint triangleIndex) {
    uint triangleCount = restir_di_scene.localIndexCount / 3u;
    if (triangleIndex >= triangleCount) {
        return 0u;
    }
    return restir_di_triangle_material_ids[triangleIndex];
}

vec2 restir_di_material_texture_uv(RestirDiMaterialVisibility material, uint slot, vec2 uv0, vec2 uv1) {
    if (slot >= RESTIR_DI_TEXTURE_TRANSFORM_COUNT) {
        return uv0;
    }
    uint transformBase = material.dataIndex + RESTIR_DI_TEXTURE_TRANSFORM_BASE + slot * 2u;
    vec4 transform1 = restir_di_mesh_materials[transformBase + 1u];
    return uint(round(transform1.z)) == 1u ? uv1 : uv0;
}

vec2 restir_di_apply_material_texture_transform(RestirDiMaterialVisibility material, uint slot, vec2 uv0, vec2 uv1) {
    vec2 uv = restir_di_material_texture_uv(material, slot, uv0, uv1);
    if (slot >= RESTIR_DI_TEXTURE_TRANSFORM_COUNT) {
        return uv;
    }
    uint transformBase = material.dataIndex + RESTIR_DI_TEXTURE_TRANSFORM_BASE + slot * 2u;
    vec4 transform0 = restir_di_mesh_materials[transformBase + 0u];
    vec4 transform1 = restir_di_mesh_materials[transformBase + 1u];
    if (uint(round(transform1.y)) == 0u) {
        return uv;
    }
    vec2 scaled = uv * transform0.zw;
    float c = cos(transform1.x);
    float s = sin(transform1.x);
    vec2 rotated = vec2(c * scaled.x - s * scaled.y, s * scaled.x + c * scaled.y);
    return transform0.xy + rotated;
}

void restir_di_apply_visibility_textures(inout RestirDiMaterialVisibility material, vec2 uv0, vec2 uv1) {
    if (material.baseColorTexture >= 0 && material.baseColorTexture < int(RESTIR_DI_MATERIAL_TEXTURE_LIMIT)) {
        int textureIndex = material.baseColorTexture;
        vec2 sampleUv = restir_di_apply_material_texture_transform(
            material, RESTIR_DI_TEXTURE_TRANSFORM_BASE_COLOR, uv0, uv1);
        vec4 base = texture(restir_di_material_textures[nonuniformEXT(textureIndex)], sampleUv);
        material.color *= max(base.rgb, vec3(0.0));
        material.alphaFactor *= clamp(base.a, 0.0, 1.0);
    }
    if (material.transmissionTexture >= 0 && material.transmissionTexture < int(RESTIR_DI_MATERIAL_TEXTURE_LIMIT)) {
        int textureIndex = material.transmissionTexture;
        vec2 sampleUv = restir_di_apply_material_texture_transform(
            material, RESTIR_DI_TEXTURE_TRANSFORM_TRANSMISSION, uv0, uv1);
        material.transmissionFactor *= clamp(
            texture(restir_di_material_textures[nonuniformEXT(textureIndex)], sampleUv).r,
            0.0,
            1.0);
    }
    if (material.opacityTexture >= 0 && material.opacityTexture < int(RESTIR_DI_MATERIAL_TEXTURE_LIMIT)) {
        material.alphaFactor *= clamp(
            texture(restir_di_material_textures[nonuniformEXT(material.opacityTexture)], uv0).r,
            0.0,
            1.0);
    }
}

bool restir_di_accept_material_alpha(RestirDiMaterialVisibility material) {
    if (material.alphaMode == RESTIR_DI_ALPHA_MODE_MASK) {
        return material.alphaFactor >= material.alphaCutoff;
    }
    if (material.alphaMode == RESTIR_DI_ALPHA_MODE_BLEND) {
        return material.alphaFactor >= 0.10;
    }
    return true;
}

float restir_di_material_effective_transmission(RestirDiMaterialVisibility material) {
    return clamp(material.transmissionFactor, 0.0, 1.0);
}

bool restir_di_candidate_visibility_blocks(rayQueryEXT rayQuery, vec3 rayDirection, uint materialVisibilityFlags) {
    uint instanceIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
    if (instanceIndex >= restir_di_scene.instanceCount) {
        return false;
    }
    RestirDiInstanceRecord instance = restir_di_instance_records[instanceIndex];
    uint meshIndex = instance.metadata.x;
    if (meshIndex >= restir_di_scene.meshCount) {
        return true;
    }
    RestirDiMeshRecord mesh = restir_di_mesh_records[meshIndex];
    uint firstIndex = mesh.vertex_index_data.z;
    uint geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
    uint primitiveIndex = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
    uint globalTriangleIndex = restir_di_geometry_triangle_offset(meshIndex, geometryIndex, firstIndex) + primitiveIndex;
    uint triIndex = globalTriangleIndex * 3u;
    if (triIndex + 2u >= restir_di_scene.localIndexCount) {
        return true;
    }
    uint i0 = restir_di_local_indices[triIndex + 0u];
    uint i1 = restir_di_local_indices[triIndex + 1u];
    uint i2 = restir_di_local_indices[triIndex + 2u];
    if (i0 >= restir_di_scene.localVertexCount ||
        i1 >= restir_di_scene.localVertexCount ||
        i2 >= restir_di_scene.localVertexCount) {
        return true;
    }

    RestirDiLocalVertex v0 = restir_di_local_vertices[i0];
    RestirDiLocalVertex v1 = restir_di_local_vertices[i1];
    RestirDiLocalVertex v2 = restir_di_local_vertices[i2];
    vec3 p0 = v0.position_uv_x.xyz;
    vec3 p1 = v1.position_uv_x.xyz;
    vec3 p2 = v2.position_uv_x.xyz;
    vec3 localGeomNormal = normalize(cross(p1 - p0, p2 - p0));
    vec3 worldGeomNormal = normalize(mat3(instance.normal_transform) * localGeomNormal);
    bool frontFace = dot(worldGeomNormal, rayDirection) < 0.0;

    uint materialIndex = restir_di_material_for_triangle_index(globalTriangleIndex);
    RestirDiMaterialVisibility material = restir_di_decode_material_visibility(materialIndex);
    if (!frontFace && material.doubleSided == 0u) {
        return false;
    }

    bool trustHardwareOpacity = (materialVisibilityFlags & RESTIR_DI_MATERIAL_VISIBILITY_TRUST_OMM) != 0u &&
        material.alphaMode == RESTIR_DI_ALPHA_MODE_MASK;
    if (material.alphaMode == RESTIR_DI_ALPHA_MODE_OPAQUE || trustHardwareOpacity) {
        return restir_di_material_effective_transmission(material) <= 1.0e-5;
    }

    vec2 bary2 = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
    vec3 bary = vec3(1.0 - bary2.x - bary2.y, bary2.x, bary2.y);
    vec2 uv0 = vec2(
        v0.position_uv_x.w * bary.x + v1.position_uv_x.w * bary.y + v2.position_uv_x.w * bary.z,
        v0.normal_uv_y.w * bary.x + v1.normal_uv_y.w * bary.y + v2.normal_uv_y.w * bary.z);
    vec2 uv1 = v0.texcoord1.xy * bary.x + v1.texcoord1.xy * bary.y + v2.texcoord1.xy * bary.z;
    restir_di_apply_visibility_textures(material, uv0, uv1);
    material.alphaFactor *= clamp((v0.color * bary.x + v1.color * bary.y + v2.color * bary.z).a, 0.0, 1.0);

    if (!restir_di_accept_material_alpha(material)) {
        return false;
    }
    return restir_di_material_effective_transmission(material) <= 1.0e-5;
}

uint restir_di_ray_query_visible(vec3 origin, vec3 direction, float tmax, uint materialVisibilityFlags) {
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery, topLevelAS, 0u, RTV_DI_RAY_MASK_SHADOW,
                           origin, 0.0, direction, tmax);
    uint candidateCount = 0u;
    while (rayQueryProceedEXT(rayQuery)) {
        if (rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            ++candidateCount;
            if (candidateCount > 8u ||
                restir_di_candidate_visibility_blocks(rayQuery, direction, materialVisibilityFlags)) {
                rayQueryConfirmIntersectionEXT(rayQuery);
                break;
            }
        }
    }
    bool hit = rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT;
    return hit ? RESTIR_DI_VISIBILITY_OCCLUDED : RESTIR_DI_VISIBILITY_VISIBLE;
}
#endif

#endif // RTV_RESTIR_DI_COMMON_GLSL
