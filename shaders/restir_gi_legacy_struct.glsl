// Minimal legacy RestirGiReservoir struct + accessors for production shaders.
// Does NOT declare buffer bindings — those are declared by each shader.
// Include BEFORE restir_gi_prod_packing.glsl so the legacy struct is available.

#ifndef RTV_RESTIR_GI_LEGACY_STRUCT_GLSL
#define RTV_RESTIR_GI_LEGACY_STRUCT_GLSL

// ── Legacy RestirGiReservoir (from rt_common.glsl, without bindings) ──

const uint RESTIR_GI_FLAG_VALID = 1u << 0u;
const uint RESTIR_GI_FLAG_VISIBLE = 1u << 1u;

struct RestirGiReservoir {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    vec4 hit_position_target_pdf;        // xyz = selected x2, w = target pdf
    vec4 normal_roughness;               // xyz = selected x2 normal, w = roughness
    vec4 suffix_radiance_source_pdf;     // rgb = reusable suffix, w = proposal PDF in selected measure
    vec4 source_direction_distance;      // xyz = source direction, w = distance or -1 for environment
#endif
    vec4 radiance_weight_sum;            // rgb = selected integrand, w = weight sum
    uvec4 metadata;                      // packed: sample count, age, flags, roughness, material id, etc.
};

// ── Legacy accessor functions ──

bool restir_gi_reservoir_valid(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return ((reservoir.metadata.z & 0xffu) & RESTIR_GI_FLAG_VALID) != 0u &&
           reservoir.radiance_weight_sum.a > 0.0 &&
           reservoir.hit_position_target_pdf.w > 0.0 &&
           reservoir.suffix_radiance_source_pdf.w > 0.0 &&
           reservoir.metadata.x > 0u;
#else
    return ((reservoir.metadata.x >> 16u) & RESTIR_GI_FLAG_VALID) != 0u &&
           reservoir.radiance_weight_sum.a > 0.0 &&
           unpackHalf2x16(reservoir.metadata.w).y > 0.0 &&
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
    return reservoir.metadata.z & 0xffu;
#else
    return (reservoir.metadata.x >> 16u) & 0xffu;
#endif
}

float restir_gi_roughness(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return clamp(reservoir.normal_roughness.w, 0.0, 1.0);
#else
    return float((reservoir.metadata.x >> 24u) & 0xffu) / 255.0;
#endif
}

uint restir_gi_material_id(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return 0xffffffffu;
#else
    return reservoir.metadata.z;
#endif
}

float restir_gi_target_pdf(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.hit_position_target_pdf.w;
#else
    return unpackHalf2x16(reservoir.metadata.w).y;
#endif
}

float restir_gi_hit_distance(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return abs(reservoir.source_direction_distance.w);
#else
    return unpackHalf2x16(reservoir.metadata.w).x;
#endif
}

vec3 restir_gi_hit_position(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.hit_position_target_pdf.xyz;
#else
    return vec3(0.0);
#endif
}

vec3 restir_gi_normal(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return normalize(reservoir.normal_roughness.xyz);
#else
    uint enc = reservoir.metadata.y;
    ivec2 q = ivec2(int(enc & 0xffffu), int((enc >> 16u) & 0xffffu));
    if (q.x >= 32768) q.x -= 65536;
    if (q.y >= 32768) q.y -= 65536;
    vec2 f = clamp(vec2(q) / 32767.0, vec2(-1.0), vec2(1.0));
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
#endif
}

#endif // RTV_RESTIR_GI_LEGACY_STRUCT_GLSL
