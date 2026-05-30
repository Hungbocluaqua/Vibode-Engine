#ifndef RTV_RESTIR_COMMON_GLSL
#define RTV_RESTIR_COMMON_GLSL

#ifndef RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
#define RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT 0
#endif

struct RestirReservoir {
    uvec4 metadata;
    vec4 sample_value_confidence;
};

struct RestirGiReservoir {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    vec4 hit_position_target_pdf;
    vec4 normal_roughness;
#endif
    vec4 radiance_weight_sum;
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    vec4 receiver_position_hit_distance;
#endif
    uvec4 metadata;
};

const uint RESTIR_VISIBILITY_UNKNOWN = 0u;
const uint RESTIR_VISIBILITY_VISIBLE = 1u;
const uint RESTIR_VISIBILITY_INVALID = 2u;
const uint RESTIR_GI_FLAG_VALID = 1u << 0u;
const uint RESTIR_GI_FLAG_VISIBLE = 1u << 1u;

uint pack_snorm2x16(vec2 value) {
    ivec2 quantized = ivec2(round(clamp(value, vec2(-1.0), vec2(1.0)) * 32767.0));
    return (uint(quantized.x) & 0xffffu) | ((uint(quantized.y) & 0xffffu) << 16u);
}

uint encode_octahedral_normal(vec3 normal) {
    vec3 n = normalize(normal);
    float denom = abs(n.x) + abs(n.y) + abs(n.z) + 1.0e-8;
    vec2 p = n.xy / denom;
    if (n.z < 0.0) {
        p = (vec2(1.0) - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    }
    return pack_snorm2x16(p);
}

vec2 unpack_unorm2x16(uint packedValue) {
    return vec2(float(packedValue & 0xffffu), float((packedValue >> 16u) & 0xffffu)) / 65535.0;
}

vec2 unpack_snorm2x16(uint packedValue) {
    ivec2 quantized = ivec2(int(packedValue & 0xffffu), int((packedValue >> 16u) & 0xffffu));
    if (quantized.x >= 32768) {
        quantized.x -= 65536;
    }
    if (quantized.y >= 32768) {
        quantized.y -= 65536;
    }
    return clamp(vec2(quantized) / 32767.0, vec2(-1.0), vec2(1.0));
}

vec3 decode_octahedral_normal(uint packedValue) {
    vec2 f = unpack_snorm2x16(packedValue);
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

uint restir_gi_pack_metadata(uint sampleCount, uint age, uint flags, float roughness) {
    uint sampleBits = min(sampleCount, 255u);
    uint ageBits = min(age, 255u);
    uint flagBits = flags & 0xffu;
    uint roughnessBits = uint(clamp(round(clamp(roughness, 0.0, 1.0) * 255.0), 0.0, 255.0));
    return sampleBits | (ageBits << 8u) | (flagBits << 16u) | (roughnessBits << 24u);
}

void restir_gi_set_metadata(inout RestirGiReservoir reservoir, uint sampleCount, uint age, uint flags, float roughness) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.metadata.x = sampleCount;
    reservoir.metadata.y = age;
    reservoir.metadata.z = flags;
    reservoir.normal_roughness.w = clamp(roughness, 0.0, 1.0);
#else
    reservoir.metadata.x = restir_gi_pack_metadata(sampleCount, age, flags, roughness);
#endif
}

void restir_gi_set_normal(inout RestirGiReservoir reservoir, vec3 normal) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.normal_roughness.xyz = normalize(normal);
#else
    reservoir.metadata.y = encode_octahedral_normal(normalize(normal));
#endif
}

void restir_gi_set_material_id(inout RestirGiReservoir reservoir, uint materialId) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.metadata.w = materialId;
#else
    reservoir.metadata.z = min(materialId, 255u);
#endif
}

float restir_gi_hit_distance(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.receiver_position_hit_distance.w;
#else
    return unpackHalf2x16(reservoir.metadata.w).x;
#endif
}

float restir_gi_target_pdf(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.hit_position_target_pdf.w;
#else
    return unpackHalf2x16(reservoir.metadata.w).y;
#endif
}

void restir_gi_set_hit_distance_target_pdf(inout RestirGiReservoir reservoir, float hitDistance, float targetPdf) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.hit_position_target_pdf.w = max(targetPdf, 1.0e-4);
    reservoir.receiver_position_hit_distance.w = max(hitDistance, 0.0);
#else
    reservoir.metadata.w = packHalf2x16(vec2(clamp(hitDistance, 0.0, 65504.0), clamp(targetPdf, 1.0e-4, 65504.0)));
#endif
}

uint restir_pack_state(uint age, uint validityVisibility, uint sampleCount) {
    return min(age, 255u) |
        ((validityVisibility & 0xffu) << 8u) |
        (min(sampleCount, 255u) << 16u);
}

uint restir_age(RestirReservoir reservoir) {
    return reservoir.metadata.z & 0xffu;
}

void restir_set_age(inout RestirReservoir reservoir, uint age) {
    reservoir.metadata.z = restir_pack_state(age, (reservoir.metadata.z >> 8u) & 0xffu, (reservoir.metadata.z >> 16u) & 0xffu);
}

uint restir_validity_visibility_bits(RestirReservoir reservoir) {
    return (reservoir.metadata.z >> 8u) & 0xffu;
}

void restir_set_validity_visibility(inout RestirReservoir reservoir, uint validityVisibility) {
    reservoir.metadata.z = restir_pack_state(restir_age(reservoir), validityVisibility, (reservoir.metadata.z >> 16u) & 0xffu);
}

uint restir_sample_count_u(RestirReservoir reservoir) {
    return max((reservoir.metadata.z >> 16u) & 0xffu, 1u);
}

void restir_set_sample_count(inout RestirReservoir reservoir, float sampleCount) {
    reservoir.metadata.z = restir_pack_state(restir_age(reservoir), restir_validity_visibility_bits(reservoir), uint(clamp(ceil(sampleCount), 1.0, 255.0)));
}

void restir_set_source_pdf_and_previous_weight(inout RestirReservoir reservoir, float sourcePdf, float previousWeight) {
    reservoir.metadata.y = packHalf2x16(vec2(clamp(sourcePdf, 1.0e-6, 65504.0), clamp(previousWeight, 0.0, 1.0)));
}

float restir_previous_weight(RestirReservoir reservoir) {
    return unpackHalf2x16(reservoir.metadata.y).y;
}

uint restir_pack_validity_visibility(bool valid, uint visibility) {
    return valid ? (1u | ((visibility & 3u) << 1u)) : 0u;
}

bool restir_validity_bit(uint value) {
    return (value & 1u) != 0u;
}

uint restir_visibility_state(RestirReservoir reservoir) {
    return (restir_validity_visibility_bits(reservoir) >> 1u) & 3u;
}

RestirReservoir empty_restir_reservoir() {
    RestirReservoir reservoir;
    reservoir.metadata = uvec4(0u);
    reservoir.sample_value_confidence = vec4(0.0);
    restir_set_source_pdf_and_previous_weight(reservoir, 1.0e-6, 0.0);
    return reservoir;
}

RestirGiReservoir empty_restir_gi_reservoir() {
    RestirGiReservoir reservoir;
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.hit_position_target_pdf = vec4(0.0);
    reservoir.normal_roughness = vec4(0.0, 1.0, 0.0, 1.0);
#endif
    reservoir.radiance_weight_sum = vec4(0.0);
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.receiver_position_hit_distance = vec4(0.0);
#endif
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.metadata = uvec4(0u);
#else
    reservoir.metadata = uvec4(restir_gi_pack_metadata(0u, 0u, 0u, 1.0), encode_octahedral_normal(vec3(0.0, 1.0, 0.0)), 0u, 0u);
#endif
    return reservoir;
}

bool restir_reservoir_valid(RestirReservoir reservoir) {
    return restir_validity_bit(restir_validity_visibility_bits(reservoir)) &&
        restir_sample_count_u(reservoir) > 0u &&
        reservoir.sample_value_confidence.a > 0.0;
}

bool restir_gi_reservoir_valid(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return (reservoir.metadata.z & RESTIR_GI_FLAG_VALID) != 0u &&
        reservoir.radiance_weight_sum.w > 0.0 &&
        reservoir.hit_position_target_pdf.w > 0.0 &&
        reservoir.metadata.x > 0u;
#else
    return ((reservoir.metadata.x >> 16u) & RESTIR_GI_FLAG_VALID) != 0u &&
        reservoir.radiance_weight_sum.w > 0.0 &&
        restir_gi_target_pdf(reservoir) > 0.0 &&
        (reservoir.metadata.x & 0xffu) > 0u;
#endif
}

uint restir_gi_sample_count_u(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return max(reservoir.metadata.x, 1u);
#else
    return max(reservoir.metadata.x & 0xffu, 1u);
#endif
}

float restir_gi_sample_count(RestirGiReservoir reservoir) {
    return float(restir_gi_sample_count_u(reservoir));
}

uint restir_gi_age(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.metadata.y;
#else
    return (reservoir.metadata.x >> 8u) & 0xffu;
#endif
}

uint restir_gi_flags(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.metadata.z;
#else
    return (reservoir.metadata.x >> 16u) & 0xffu;
#endif
}

bool restir_gi_visible(RestirGiReservoir reservoir) {
    return (restir_gi_flags(reservoir) & RESTIR_GI_FLAG_VISIBLE) != 0u;
}

uint restir_gi_material_id(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.metadata.w;
#else
    return reservoir.metadata.z;
#endif
}

float restir_gi_roughness(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return clamp(reservoir.normal_roughness.w, 0.0, 1.0);
#else
    return float((reservoir.metadata.x >> 24u) & 0xffu) / 255.0;
#endif
}

vec3 restir_gi_normal(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return normalize(reservoir.normal_roughness.xyz);
#else
    return decode_octahedral_normal(reservoir.metadata.y);
#endif
}

float restir_gi_age_normalized(RestirGiReservoir reservoir, float maxAge) {
    return clamp(float(restir_gi_age(reservoir)) / max(maxAge, 1.0), 0.0, 1.0);
}

float restir_luminance(vec3 value) {
    return dot(value, vec3(0.2126, 0.7152, 0.0722));
}

float restir_target_function(RestirReservoir reservoir) {
    return max(restir_luminance(reservoir.sample_value_confidence.rgb), 0.0);
}

float restir_source_pdf(RestirReservoir reservoir) {
    return max(unpackHalf2x16(reservoir.metadata.y).x, 1.0e-6);
}

float restir_sample_count(RestirReservoir reservoir) {
    return float(restir_sample_count_u(reservoir));
}

float restir_age_confidence(RestirReservoir reservoir, float maxAge) {
    return 1.0 - clamp(float(restir_age(reservoir)) / max(maxAge, 1.0), 0.0, 1.0);
}

float restir_pairwise_compatibility(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    if (!restir_reservoir_valid(current) || !restir_reservoir_valid(previous) || restir_age(previous) >= uint(maxAge)) {
        return 0.0;
    }

    if (current.metadata.x != previous.metadata.x) {
        return 0.0;
    }

    if (current.metadata.w != 0u && previous.metadata.w != 0u && current.metadata.w != previous.metadata.w) {
        return 0.0;
    }

    uint previousVisibility = restir_visibility_state(previous);
    if (previousVisibility == RESTIR_VISIBILITY_INVALID) {
        return 0.0;
    }

    float currentPdf = restir_source_pdf(current);
    float previousPdf = restir_source_pdf(previous);
    float pdfRatio = min(currentPdf, previousPdf) / max(currentPdf, previousPdf);

    float currentTarget = max(restir_target_function(current), 1.0e-5);
    float previousTarget = max(restir_target_function(previous), 1.0e-5);
    float targetRatio = min(currentTarget, previousTarget) / max(currentTarget, previousTarget);

    return clamp(motionConfidence, 0.0, 1.0) *
        restir_age_confidence(previous, maxAge) *
        sqrt(clamp(pdfRatio, 0.0, 1.0)) *
        sqrt(clamp(targetRatio, 0.0, 1.0));
}

float restir_pairwise_previous_weight(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    float compatibility = restir_pairwise_compatibility(current, previous, motionConfidence, maxAge);
    if (compatibility <= 0.0) {
        return 0.0;
    }

    float currentMass = max(restir_target_function(current), 0.0) / restir_source_pdf(current);
    float previousMass = max(restir_target_function(previous), 0.0) /
        restir_source_pdf(previous) *
        min(restir_sample_count(previous), 32.0) *
        clamp(previous.sample_value_confidence.a, 0.0, 1.0) *
        compatibility;
    float combined = currentMass + previousMass;
    if (combined <= 1.0e-8) {
        return 0.0;
    }

    float motionCap = mix(0.85, 0.10, 1.0 - clamp(motionConfidence, 0.0, 1.0));
    return clamp(previousMass / combined, 0.0, motionCap);
}

RestirReservoir restir_pairwise_temporal_merge(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    float previousWeight = restir_pairwise_previous_weight(current, previous, motionConfidence, maxAge);
    float currentWeight = 1.0 - previousWeight;
    uint currentVisibility = restir_visibility_state(current);
    uint previousVisibility = restir_visibility_state(previous);
    uint mergedVisibility = previousWeight > 0.0
        ? (currentVisibility == RESTIR_VISIBILITY_VISIBLE && previousVisibility == RESTIR_VISIBILITY_VISIBLE
            ? RESTIR_VISIBILITY_VISIBLE
            : RESTIR_VISIBILITY_UNKNOWN)
        : currentVisibility;

    restir_set_age(current, previousWeight > 0.0 ? min(restir_age(previous) + 1u, 255u) : 0u);
    restir_set_validity_visibility(current, restir_pack_validity_visibility(
        restir_reservoir_valid(current),
        mergedVisibility));
    current.sample_value_confidence.rgb =
        current.sample_value_confidence.rgb * currentWeight +
        previous.sample_value_confidence.rgb * previousWeight;
    current.sample_value_confidence.a = clamp(
        (current.sample_value_confidence.a * currentWeight + previous.sample_value_confidence.a * previousWeight) *
            clamp(motionConfidence, 0.0, 1.0),
        0.0,
        1.0);
    float sourcePdf = restir_source_pdf(current) * currentWeight +
        restir_source_pdf(previous) * previousWeight;
    restir_set_source_pdf_and_previous_weight(current, sourcePdf, previousWeight);
    restir_set_sample_count(current, min(
        restir_sample_count(current) + restir_sample_count(previous) * previousWeight,
        64.0));
    return current;
}

#endif
