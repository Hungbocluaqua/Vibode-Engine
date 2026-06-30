#ifndef RTV_RESTIR_DI_TYPES_ACCESSORS_GLSL
#define RTV_RESTIR_DI_TYPES_ACCESSORS_GLSL

// Shared ReSTIR DI ABI and stage-selected accessors. Define RTV_RESTIR_DI_COMPUTE_ACCESSORS=1 for compute passes before including.
#ifndef RTV_RESTIR_DI_VALIDATION_FULL
#define RTV_RESTIR_DI_VALIDATION_FULL 0
#endif

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

#ifndef RTV_RESTIR_DI_LIGHT_KIND_CONSTANTS_DEFINED
#define RTV_RESTIR_DI_LIGHT_KIND_CONSTANTS_DEFINED
const uint RESTIR_DI_LIGHT_EMISSIVE_TRIANGLE = 0u;
const uint RESTIR_DI_LIGHT_EMISSIVE_SPHERE   = 1u;
const uint RESTIR_DI_LIGHT_DIRECTIONAL       = 2u;
const uint RESTIR_DI_LIGHT_POINT             = 3u;
const uint RESTIR_DI_LIGHT_AREA              = 4u;
const uint RESTIR_DI_LIGHT_SPOT              = 5u;
const uint RESTIR_DI_LIGHT_ENVIRONMENT       = 6u;
const uint RESTIR_DI_LIGHT_SUN               = 7u;
const uint RESTIR_DI_PSEUDO_LIGHT_INDEX      = 0u;
const uint RESTIR_DI_ENVIRONMENT_ID_HASH     = 0x7e8f1a3du;
const uint RESTIR_DI_ENVIRONMENT_VERSION     = 0x454e5631u;
const uint RESTIR_DI_SUN_ID_HASH             = 0x51f5a91du;
const uint RESTIR_DI_SUN_VERSION             = 0x53554e31u;

bool restir_di_light_kind_infinite(uint kind) {
    return kind == RESTIR_DI_LIGHT_ENVIRONMENT || kind == RESTIR_DI_LIGHT_SUN;
}
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

// reservoirMetadata.y: packHalf2x16(vec2(sourcePdf, previousWeight))
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
const uint RESTIR_DI_COUNTER_INITIAL_ENVIRONMENT = 61u;
const uint RESTIR_DI_COUNTER_INITIAL_SUN = 62u;
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

#if defined(RTV_RESTIR_DI_COMPUTE_ACCESSORS) && RTV_RESTIR_DI_COMPUTE_ACCESSORS

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
    uint bindlessTextureCapacity;
} restir_di_scene;

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


#else

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
    r.reservoirMetadata.y = packHalf2x16(vec2(1.0e-6, 0.0));
    return r;
}

float restir_di_target_function(vec3 radiance) {
    return max(dot(radiance, vec3(0.2126, 0.7152, 0.0722)), 1.0e-6);
}

void restir_di_set_valid(inout RestirDiReservoir r, bool valid) {
    if (valid) r.reservoirMetadata.x |= (1u << 18u);
    else r.reservoirMetadata.x &= ~(1u << 18u);
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
    float prevWeight = unpackHalf2x16(r.reservoirMetadata.y).y;
    r.reservoirMetadata.y = packHalf2x16(vec2(clamp(pdf, 1.0e-6, 65504.0), prevWeight));
}
void restir_di_set_previous_weight(inout RestirDiReservoir r, float w) {
    float pdf = unpackHalf2x16(r.reservoirMetadata.y).x;
    r.reservoirMetadata.y = packHalf2x16(vec2(pdf, clamp(w, 0.0, 1.0)));
}
bool restir_di_reservoir_valid(RestirDiReservoir r) {
    bool validBit = (r.reservoirMetadata.x & (1u << 18u)) != 0u;
#if RTV_RESTIR_DI_VALIDATION_FULL
    float target = r.sampleRadiance_target.w;
    float weight = r.sampleNormal_weightSum.w;
    float pdf = r.sampleDirection_pdf.w;
    vec3 radiance = r.sampleRadiance_target.xyz;
#else
    vec2 targetWeight = unpackHalf2x16(r.reservoirMetadata.w);
    float target = targetWeight.x;
    float weight = targetWeight.y;
    float pdf = unpackHalf2x16(r.reservoirMetadata.y).x;
    vec3 radiance = restir_di_unpack_radiance(r.sampleMetadata.w);
#endif
    return validBit && ((r.reservoirMetadata.x >> 8u) & 0xffu) > 0u &&
        target > 0.0 && weight > 0.0 && pdf > 0.0 &&
        !isnan(target) && !isinf(target) && !isnan(weight) && !isinf(weight) &&
        !isnan(pdf) && !isinf(pdf) &&
        !any(isnan(r.samplePosition_distance.xyz)) && !any(isinf(r.samplePosition_distance.xyz)) &&
        !any(isnan(radiance)) && !any(isinf(radiance));
}
uint restir_di_light_kind(RestirDiReservoir r) { return r.sampleMetadata.z & 0xffu; }
uint restir_di_light_id(RestirDiReservoir r) { return r.sampleMetadata.x; }
uint restir_di_light_index(RestirDiReservoir r) { return r.sampleMetadata.z >> 8u; }
uint restir_di_light_version(RestirDiReservoir r) { return r.sampleMetadata.y; }
float restir_di_target(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleRadiance_target.w, 0.0);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.w).x, 0.0);
#endif
}

uint restir_di_identity_hash(uvec2 identity) {
    uint h = identity.x ^ (identity.y + 0x9e3779b9u + (identity.x << 6u) + (identity.x >> 2u));
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    return h ^ (h >> 16u);
}
float restir_di_source_pdf(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleDirection_pdf.w, 1.0e-6);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.y).x, 1.0e-6);
#endif
}
float restir_di_weight_sum(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleNormal_weightSum.w, 0.0);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.w).y, 0.0);
#endif
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
void restir_di_set_confidence(inout RestirDiReservoir r, float confidence) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.contribution_confidence.w = clamp(confidence, 0.0, 1.0);
#else
    uint packedConfidence = uint(round(clamp(confidence, 0.0, 1.0) * 31.0));
    r.reservoirMetadata.x = (r.reservoirMetadata.x & 0x07ffffffu) |
        (packedConfidence << 27u);
#endif
}
vec3 restir_di_sample_radiance(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleRadiance_target.rgb, vec3(0.0));
#else
    return max(restir_di_unpack_radiance(r.sampleMetadata.w), vec3(0.0));
#endif
}
void restir_di_set_direction(inout RestirDiReservoir r, vec3 direction) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleDirection_pdf.xyz = normalize(direction);
#endif
}
vec2 restir_di_oct_encode(vec3 value) {
    vec3 n = normalize(value);
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
}
void restir_di_set_light_normal(inout RestirDiReservoir r, vec3 normal) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleNormal_weightSum.xyz = normalize(normal);
#else
    r.reservoirMetadata.z = packSnorm2x16(restir_di_oct_encode(normal));
#endif
}
float restir_di_previous_weight(RestirDiReservoir r) { return unpackHalf2x16(r.reservoirMetadata.y).y; }
uint restir_di_m(RestirDiReservoir r) { return (r.reservoirMetadata.x >> 8u) & 0xffu; }
uint restir_di_visibility(RestirDiReservoir r) { return (r.reservoirMetadata.x >> 16u) & 3u; }
uint restir_di_rejection_flags(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return r.reservoirMetadata.w;
#else
    return (r.reservoirMetadata.x >> 19u) & 0xffu;
#endif
}

#endif

#endif // RTV_RESTIR_DI_TYPES_ACCESSORS_GLSL
