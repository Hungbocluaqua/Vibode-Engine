#ifndef RTV_RT_MATERIAL_GLSL
#define RTV_RT_MATERIAL_GLSL

#ifndef RTV_MATERIAL_TEXTURES_ENABLED
#define RTV_MATERIAL_TEXTURES_ENABLED 1
#endif

// Material records, texture evaluation, alpha handling, and terminal material sampling.
struct Material {
    uint material_data_index;
    vec3 color;
    float roughness;
    float ior;
    uint mat_type;
    float metallic;
    float pad2;
    vec3 emissive;
    float alpha_factor;
    int base_color_texture;
    int normal_texture;
    int metallic_roughness_texture;
    int emissive_texture;
    float alpha_cutoff;
    uint alpha_mode;
    uint double_sided;
    vec3 conductor_eta;
    uint use_conductor_optics;
    vec3 conductor_k;
    float anisotropy_strength;
    float anisotropy_rotation;
    int occlusion_texture;
    float occlusion_strength;
    vec3 sheen_color;
    float sheen_roughness;
    int sheen_color_texture;
    int sheen_roughness_texture;
    float iridescence_factor;
    float iridescence_ior;
    float iridescence_thickness_min;
    float iridescence_thickness_max;
    int iridescence_texture;
    int iridescence_thickness_texture;
    float occlusion;
    float normal_variance;
    float clearcoat_factor;
    float clearcoat_roughness;
    int clearcoat_texture;
    int clearcoat_roughness_texture;
    int clearcoat_normal_texture;
    float transmission_factor;
    int transmission_texture;
    float specular_factor;
    vec3 specular_color;
    int specular_texture;
    int specular_color_texture;
    int anisotropy_texture;
    float volume_thickness_factor;
    int volume_thickness_texture;
    vec3 volume_attenuation_color;
    float volume_attenuation_distance;
    float dispersion_factor;
    int opacity_texture;
    int height_texture;
    float height_scale;
    vec3 clearcoat_normal;
    float clearcoat_normal_variance;
};

const uint MATERIAL_CLOSURE_FLAG_DIFFUSE       = 1u << 0u;
const uint MATERIAL_CLOSURE_FLAG_SPECULAR      = 1u << 1u;
const uint MATERIAL_CLOSURE_FLAG_SSS           = 1u << 2u;
const uint MATERIAL_CLOSURE_FLAG_TRANSMISSION  = 1u << 3u;
const uint MATERIAL_CLOSURE_FLAG_CLEARCOAT     = 1u << 4u;
const uint MATERIAL_CLOSURE_FLAG_SHEEN         = 1u << 5u;
const uint MATERIAL_CLOSURE_FLAG_THIN_FILM     = 1u << 6u;
const uint MATERIAL_CLOSURE_FLAG_METAL         = 1u << 7u;
const uint MATERIAL_CLOSURE_FLAG_UNLIT         = 1u << 8u;
const uint MATERIAL_CLOSURE_FLAG_VOLUME        = 1u << 9u;
const uint MATERIAL_CLOSURE_FLAG_DISPERSION    = 1u << 10u;
const float SCREEN_VELOCITY_PACK_SCALE = 512.0;
const float SCREEN_VELOCITY_SATURATION_THRESHOLD = SCREEN_VELOCITY_PACK_SCALE - 0.5;
const float SCREEN_VELOCITY_DEBUG_SCALE = 128.0;

struct MaterialClosure {
    uint flags;
    float weight;
    vec3 color;
    float roughness;
    float metallic;
    float ior;
    float pad;
};

float material_effective_transmission(Material material) {
    float explicitTransmission = clamp(material.transmission_factor, 0.0, 1.0);
    if (explicitTransmission > 1.0e-5 || material.transmission_texture >= 0) {
        return explicitTransmission;
    }
    return material.mat_type == 2u ? 1.0 : 0.0;
}

MaterialClosure material_to_closure(Material m) {
    MaterialClosure c;
    c.color = m.color;
    c.roughness = m.roughness;
    c.metallic = m.metallic;
    c.ior = m.ior;
    c.weight = 1.0;
    c.pad = 0.0;
    c.flags = 0u;

    if (m.mat_type == 0u) {
        c.flags = MATERIAL_CLOSURE_FLAG_DIFFUSE;
    } else if (m.mat_type == 1u || m.mat_type == 3u) {
        c.flags = MATERIAL_CLOSURE_FLAG_SPECULAR | MATERIAL_CLOSURE_FLAG_METAL;
    } else if (m.mat_type == 2u) {
        c.flags = MATERIAL_CLOSURE_FLAG_SPECULAR | MATERIAL_CLOSURE_FLAG_TRANSMISSION;
    } else if (m.mat_type == 4u) {
        c.flags = MATERIAL_CLOSURE_FLAG_SPECULAR | MATERIAL_CLOSURE_FLAG_CLEARCOAT;
    } else if (m.mat_type == 5u) {
        c.flags = MATERIAL_CLOSURE_FLAG_UNLIT;
    }

    if ((c.flags & MATERIAL_CLOSURE_FLAG_UNLIT) != 0u) {
        return c;
    }

    if (material_effective_transmission(m) > 1.0e-5) {
        c.flags |= MATERIAL_CLOSURE_FLAG_TRANSMISSION;
    }
    if (m.clearcoat_factor > 1.0e-5) {
        c.flags |= MATERIAL_CLOSURE_FLAG_CLEARCOAT | MATERIAL_CLOSURE_FLAG_SPECULAR;
    }

    if (dot(max(m.sheen_color, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)) > 1.0e-5) {
        c.flags |= MATERIAL_CLOSURE_FLAG_SHEEN;
    }
    if (m.iridescence_factor > 1.0e-5) {
        c.flags |= MATERIAL_CLOSURE_FLAG_THIN_FILM;
    }
    if (m.volume_thickness_factor > 1.0e-6 && m.volume_attenuation_distance > 0.0) {
        c.flags |= MATERIAL_CLOSURE_FLAG_VOLUME;
    }
    if (m.dispersion_factor > 1.0e-6) {
        c.flags |= MATERIAL_CLOSURE_FLAG_DISPERSION;
    }

    return c;
}

bool material_is_unlit(Material material) {
    return material.mat_type == 5u;
}

bool material_is_transmissive(Material material) {
    return material_effective_transmission(material) > 1.0e-5;
}

bool closure_has_flag(MaterialClosure c, uint flag) {
    return (c.flags & flag) != 0u;
}

struct RayPayload {
    uint hit;
    float t;
    vec3 world_pos;
    uint material_id;
    vec3 local_pos;
    vec3 geom_normal;
    uint front_face;
    vec3 normal;
    uint instance_id;
    uint mesh_id;
    uint primitive_id;
    uint picking;
    vec3 barycentrics;
    vec2 uv;
    vec2 uv1;
    vec3 tangent;
    vec3 bitangent;
    vec4 vertex_color;
};

struct TerminalRayPayload {
    uint hit;
    float t;
    vec3 geom_normal;
    vec3 normal;
    vec3 tangent;
    vec3 bitangent;
    vec2 uv;
    vec2 uv1;
    vec4 vertex_color;
    uint material_id;
};

const uint TERMINAL_MATERIAL_CONTRIBUTING_BIT = 0x80000000u;
const uint TERMINAL_MATERIAL_INDEX_MASK = 0x7fffffffu;

const float PI = 3.14159265358979323846;
const uint TRI_STRIDE = 12u;
const uint MATERIAL_PARAMETER_STRIDE = 18u;
const uint MATERIAL_TEXTURE_TRANSFORM_COUNT = 17u;
const uint MATERIAL_TEXTURE_TRANSFORM_BASE = MATERIAL_PARAMETER_STRIDE;
const uint MATERIAL_STRIDE = MATERIAL_PARAMETER_STRIDE + MATERIAL_TEXTURE_TRANSFORM_COUNT * 2u;
const uint MATERIAL_TEXTURE_TRANSFORM_BASE_COLOR = 0u;
const uint MATERIAL_TEXTURE_TRANSFORM_NORMAL = 1u;
const uint MATERIAL_TEXTURE_TRANSFORM_METALLIC_ROUGHNESS = 2u;
const uint MATERIAL_TEXTURE_TRANSFORM_EMISSIVE = 3u;
const uint MATERIAL_TEXTURE_TRANSFORM_OCCLUSION = 4u;
const uint MATERIAL_TEXTURE_TRANSFORM_SHEEN_COLOR = 5u;
const uint MATERIAL_TEXTURE_TRANSFORM_SHEEN_ROUGHNESS = 6u;
const uint MATERIAL_TEXTURE_TRANSFORM_IRIDESCENCE = 7u;
const uint MATERIAL_TEXTURE_TRANSFORM_IRIDESCENCE_THICKNESS = 8u;
const uint MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT = 9u;
const uint MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT_ROUGHNESS = 10u;
const uint MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT_NORMAL = 11u;
const uint MATERIAL_TEXTURE_TRANSFORM_TRANSMISSION = 12u;
const uint MATERIAL_TEXTURE_TRANSFORM_VOLUME_THICKNESS = 13u;
const uint MATERIAL_TEXTURE_TRANSFORM_SPECULAR = 14u;
const uint MATERIAL_TEXTURE_TRANSFORM_SPECULAR_COLOR = 15u;
const uint MATERIAL_TEXTURE_TRANSFORM_ANISOTROPY = 16u;
#define MATERIAL_TEXTURE_LIMIT int(mesh_params.bindless_texture_capacity)
const uint MATERIAL_FLAG_MANUAL_BASE_COLOR_SRGB = 1u << 0u;
const uint MATERIAL_FLAG_MANUAL_EMISSIVE_SRGB = 1u << 1u;
const uint MATERIAL_FLAG_NORMAL_MAP_DIRECTX = 1u << 2u;
const uint MATERIAL_FLAG_SPECULAR_GLOSSINESS_WORKFLOW = 1u << 3u;
const uint MATERIAL_FLAG_SPECULAR_ALPHA_GLOSSINESS = 1u << 4u;
const uint MATERIAL_FLAG_MAY_BE_TRANSMISSIVE = 1u << 5u;
const uint ALPHA_MODE_OPAQUE = 0u;
const uint ALPHA_MODE_MASK = 1u;
const uint ALPHA_MODE_BLEND = 2u;
const float MATERIAL_DELTA_ROUGHNESS_THRESHOLD = 0.001;
const float MATERIAL_MIN_GGX_ROUGHNESS = 0.001;

#if RTV_RT_DIAGNOSTIC_COUNTERS
void record_rt_alpha_class_counter(uint alphaMode, uint opaqueCounter, uint alphaTestedCounter, uint blendedCounter) {
    if (alphaMode == ALPHA_MODE_OPAQUE) {
        record_rt_counter(opaqueCounter);
    } else if (alphaMode == ALPHA_MODE_MASK) {
        record_rt_counter(alphaTestedCounter);
    } else if (alphaMode == ALPHA_MODE_BLEND) {
        record_rt_counter(blendedCounter);
    }
}
#else
#define record_rt_alpha_class_counter(alphaMode, opaqueCounter, alphaTestedCounter, blendedCounter)
#endif

struct MaterialRuntimeHeader {
    uint alpha_mode;
    uint double_sided;
};

struct TerminalMaterialHeader {
    uint mat_type;
    vec3 emissive;
    int emissive_texture;
};

MaterialRuntimeHeader decode_material_runtime_header(uint mat_idx) {
    uint idx = min(mat_idx, max(mesh_params.material_count, 1u) - 1u) * MATERIAL_STRIDE;
    vec4 d4 = mesh_materials[idx + 4u];
    MaterialRuntimeHeader h;
    h.alpha_mode = uint(round(d4.y));
    h.double_sided = uint(round(d4.z));
    return h;
}

TerminalMaterialHeader decode_terminal_material_header(uint mat_idx) {
    uint idx = min(mat_idx, max(mesh_params.material_count, 1u) - 1u) * MATERIAL_STRIDE;
    vec4 d1 = mesh_materials[idx + 1u];
    vec4 d2 = mesh_materials[idx + 2u];
    vec4 d3 = mesh_materials[idx + 3u];
    TerminalMaterialHeader h;
    h.mat_type = uint(d1.y);
    h.emissive = d2.xyz;
    h.emissive_texture = int(round(d3.w));
    return h;
}

bool terminal_material_can_contribute(TerminalMaterialHeader header) {
    return header.mat_type == 5u || dot(max(header.emissive, vec3(0.0)), vec3(1.0)) > 0.0;
}

bool material_is_delta(Material material) {
    return material_is_transmissive(material) ||
        (material.mat_type == 1u && material.roughness < MATERIAL_DELTA_ROUGHNESS_THRESHOLD);
}

float ggx_safe_roughness(float roughness) {
    return clamp(roughness, MATERIAL_MIN_GGX_ROUGHNESS, 1.0);
}

float shadow_self_hit_epsilon() {
    return max(camera.render_controls.x, 0.00001);
}

float shadow_distance_bias() {
    return max(camera.render_controls.y, 0.0);
}

float firefly_clamp_luminance() {
    return max(camera.render_controls.z, 1.0);
}

float russian_roulette_min_survival() {
    return clamp(camera.render_controls.w, 0.02, 0.50);
}
const float DEBUG_WHITE_ENV_RADIANCE = 4.0;

uint pack_unorm2x16(vec2 v) {
    uint x = uint(round(clamp(v.x, 0.0, 1.0) * 65535.0));
    uint y = uint(round(clamp(v.y, 0.0, 1.0) * 65535.0));
    return x | (y << 16u);
}

uint pack_snorm2x16(vec2 v) {
    ivec2 quantized = ivec2(round(clamp(v, vec2(-1.0), vec2(1.0)) * 32767.0));
    return (uint(quantized.x) & 0xffffu) | ((uint(quantized.y) & 0xffffu) << 16u);
}

uint encode_octahedral_normal(vec3 n) {
    float denom = abs(n.x) + abs(n.y) + abs(n.z) + 1e-8;
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

uvec4 pack_depth_normal(float depth, vec3 normal, float roughness) {
    return uvec4(floatBitsToUint(depth), encode_octahedral_normal(normal), floatBitsToUint(roughness), 0u);
}

uint pack_variance(float v) {
    return pack_unorm2x16(vec2(clamp(v / 64.0, 0.0, 1.0), 0.0));
}

float temporal_normalized_hit_distance(float hitDistance) {
    return clamp(log2(max(hitDistance, 0.0) + 1.0) / 10.0, 0.0, 1.0);
}

float restir_luminance(vec3 value) {
    return dot(value, vec3(0.2126, 0.7152, 0.0722));
}

const uint RESTIR_VISIBILITY_UNKNOWN = 0u;
const uint RESTIR_VISIBILITY_VISIBLE = 1u;
const uint RESTIR_VISIBILITY_INVALID = 2u;
const uint RESTIR_GI_FLAG_VALID = 1u << 0u;
const uint RESTIR_GI_FLAG_VISIBLE = 1u << 1u;
const float WORLD_POSITION_PACK_RANGE = 64.0;

uint restir_pack_validity_visibility(bool valid, uint visibility) {
    return valid ? (1u | ((visibility & 3u) << 1u)) : 0u;
}

bool restir_validity_bit(uint value) {
    return (value & 1u) != 0u;
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

uint restir_visibility_state(RestirReservoir reservoir) {
    return (restir_validity_visibility_bits(reservoir) >> 1u) & 3u;
}

bool restir_reservoir_valid(RestirReservoir reservoir) {
    return restir_validity_bit(restir_validity_visibility_bits(reservoir)) &&
        restir_sample_count_u(reservoir) > 0u &&
        reservoir.sample_value_confidence.a > 0.0 &&
        reservoir.sample_radiance_target.w > 0.0;
}

bool restir_gi_reservoir_valid(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return ((reservoir.metadata.z & 0xffu) & RESTIR_GI_FLAG_VALID) != 0u &&
        reservoir.radiance_weight_sum.w > 0.0 &&
        reservoir.hit_position_target_pdf.w > 0.0 &&
        reservoir.suffix_radiance_source_pdf.w > 0.0 &&
        reservoir.metadata.x > 0u;
#else
    return ((reservoir.metadata.x >> 16u) & RESTIR_GI_FLAG_VALID) != 0u &&
        reservoir.radiance_weight_sum.w > 0.0 &&
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

bool restir_gi_visible(RestirGiReservoir reservoir) {
    return (restir_gi_flags(reservoir) & RESTIR_GI_FLAG_VISIBLE) != 0u;
}

uint restir_gi_material_id(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return 0xffffffffu;
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

bool restir_gi_has_positions(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return restir_gi_reservoir_valid(reservoir) &&
        reservoir.source_direction_distance.w > 0.0;
#else
    return false;
#endif
}

vec3 restir_gi_hit_position(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.hit_position_target_pdf.xyz;
#else
    return vec3(0.0);
#endif
}

vec3 restir_gi_receiver_position(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.hit_position_target_pdf.xyz -
        normalize(reservoir.source_direction_distance.xyz) * reservoir.source_direction_distance.w;
#else
    return vec3(0.0);
#endif
}

void restir_gi_set_receiver_position(inout RestirGiReservoir reservoir, vec3 receiverPosition) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    vec3 delta = reservoir.hit_position_target_pdf.xyz - receiverPosition;
    reservoir.source_direction_distance = vec4(normalize(delta), length(delta));
#endif
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
    reservoir.metadata.z = (reservoir.metadata.z & 0xffffff00u) | (flags & 0xffu);
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
    // The full estimator layout uses metadata.w for the scene version hash.
#else
    reservoir.metadata.z = materialId;
#endif
}

float restir_gi_hit_distance(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return abs(reservoir.source_direction_distance.w);
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
    reservoir.source_direction_distance.w = max(hitDistance, 0.0);
#else
    reservoir.metadata.w = packHalf2x16(vec2(clamp(hitDistance, 0.0, 65504.0), clamp(targetPdf, 1.0e-4, 65504.0)));
#endif
}

RestirGiReservoir empty_restir_gi_reservoir() {
    RestirGiReservoir reservoir;
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.hit_position_target_pdf = vec4(0.0);
    reservoir.normal_roughness = vec4(0.0, 1.0, 0.0, 1.0);
    reservoir.suffix_radiance_source_pdf = vec4(0.0);
    reservoir.source_direction_distance = vec4(0.0);
#endif
    reservoir.radiance_weight_sum = vec4(0.0);
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.metadata = uvec4(0u);
#else
    reservoir.metadata = uvec4(restir_gi_pack_metadata(0u, 0u, 0u, 1.0), encode_octahedral_normal(vec3(0.0, 1.0, 0.0)), 0u, 0u);
#endif
    return reservoir;
}

float restir_gi_age_normalized(RestirGiReservoir reservoir, float maxAge) {
    return clamp(float(restir_gi_age(reservoir)) / max(maxAge, 1.0), 0.0, 1.0);
}

float restir_target_function(RestirReservoir reservoir) {
    return max(reservoir.sample_radiance_target.w, max(restir_luminance(reservoir.sample_value_confidence.rgb), 0.0));
}

float restir_weight_sum(RestirReservoir reservoir) {
    return max(reservoir.sample_normal_weight.w, 0.0);
}

float restir_source_pdf(RestirReservoir reservoir) {
    return max(unpackHalf2x16(reservoir.metadata.y).x, 1.0e-6);
}

float restir_sample_count(RestirReservoir reservoir) {
    return float(restir_sample_count_u(reservoir));
}

uint restir_temporal_signature(uint sampleType, float roughness, uint materialId, uint instanceId) {
    uint roughnessBucket = uint(clamp(floor(clamp(roughness, 0.0, 1.0) * 15.0 + 0.5), 0.0, 15.0));
    uint surfaceHash = materialId * 747796405u ^ instanceId * 2891336453u;
    surfaceHash ^= surfaceHash >> 16u;
    return (sampleType & 0xffu) | (roughnessBucket << 8u) | ((surfaceHash & 0xfffffu) << 12u);
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

    if (restir_visibility_state(previous) == RESTIR_VISIBILITY_INVALID) {
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
    RestirReservoir selected = previousWeight > 0.5 ? previous : current;
    bool selectedPrevious = previousWeight > 0.5;
    uint selectedVisibility = restir_visibility_state(selected);
    restir_set_age(selected, selectedPrevious ? min(restir_age(previous) + 1u, 255u) : 0u);
    restir_set_validity_visibility(selected, restir_pack_validity_visibility(restir_reservoir_valid(selected), selectedVisibility));
    selected.sample_value_confidence.a = clamp(selected.sample_value_confidence.a * clamp(motionConfidence, 0.0, 1.0), 0.0, 1.0);
    selected.sample_normal_weight.w = min(
        max(restir_weight_sum(current), restir_target_function(current)) +
            max(restir_weight_sum(previous), restir_target_function(previous)) * previousWeight,
        65504.0);
    restir_set_source_pdf_and_previous_weight(selected, restir_source_pdf(selected), previousWeight);
    restir_set_sample_count(selected, min(
        restir_sample_count(current) + restir_sample_count(previous) * previousWeight,
        64.0));
    return selected;
}

uvec2 pack_world_position(vec3 world_pos) {
    vec3 relative = clamp((world_pos - camera.pos.xyz) / WORLD_POSITION_PACK_RANGE, vec3(-1.0), vec3(1.0));
    return uvec2(pack_snorm2x16(relative.xy), pack_snorm2x16(vec2(relative.z, 0.0)));
}

bool project_unjittered_to_pixels_checked(mat4 viewProj, vec2 jitterPixels, vec3 worldPos, ivec2 dims, out vec2 pixels) {
    // PrevCamera matrices are uploaded without projection jitter; the jitter argument
    // is retained for older call sites but must not be applied here.
    vec4 clip = viewProj * vec4(worldPos, 1.0);
    if (clip.w <= 0.0) {
        pixels = vec2(0.0);
        return false;
    }
    vec3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0001) {
        pixels = vec2(0.0);
        return false;
    }
    pixels = vec2(
        (ndc.x * 0.5 + 0.5) * float(dims.x) - 0.5,
        (ndc.y * 0.5 + 0.5) * float(dims.y) - 0.5);
    return true;
}

vec2 project_unjittered_to_pixels(mat4 viewProj, vec2 jitterPixels, vec3 worldPos, ivec2 dims) {
    vec2 pixels;
    return project_unjittered_to_pixels_checked(viewProj, jitterPixels, worldPos, dims, pixels) ? pixels : vec2(-1.0);
}

uint pack_velocity_pixels(vec2 velocityPixels) {
    ivec2 encoded = ivec2(round(clamp(velocityPixels / SCREEN_VELOCITY_PACK_SCALE, vec2(-1.0), vec2(1.0)) * 32767.0));
    return (uint(encoded.x) & 0xffffu) | ((uint(encoded.y) & 0xffffu) << 16u);
}

uint pack_invalid_velocity_pixels() {
    return pack_velocity_pixels(vec2(SCREEN_VELOCITY_PACK_SCALE));
}

uint compute_surface_velocity_from_previous_world(vec3 currentWorldPos, vec3 previousWorldPos, ivec2 dims) {
    vec2 currentPos;
    vec2 previousPos;
    bool valid = project_unjittered_to_pixels_checked(prev_camera.view_proj, prev_camera.jitter.xy, currentWorldPos, dims, currentPos) &&
        project_unjittered_to_pixels_checked(prev_camera.prev_view_proj, prev_camera.jitter.zw, previousWorldPos, dims, previousPos);
    return valid ? pack_velocity_pixels(currentPos - previousPos) : pack_invalid_velocity_pixels();
}

vec3 previous_skinned_local_position(uint meshIndex, uint primitiveIndex, vec3 barycentrics, vec3 fallbackLocalPos) {
    if (meshIndex >= mesh_params.mesh_count) {
        return fallbackLocalPos;
    }

    uvec4 binding = gpu_skinning_rt_mesh_bindings[meshIndex];
    if (binding.z == 0u) {
        return fallbackLocalPos;
    }

    uint triangleCount = mesh_params.local_index_count / 3u;
    if (primitiveIndex >= triangleCount) {
        return fallbackLocalPos;
    }

    uint triIndex = primitiveIndex * 3u;
    uint i0 = local_mesh_indices[triIndex + 0u];
    uint i1 = local_mesh_indices[triIndex + 1u];
    uint i2 = local_mesh_indices[triIndex + 2u];
    bool inSkinnedRange = i0 >= binding.x && (i0 - binding.x) < binding.y &&
        i1 >= binding.x && (i1 - binding.x) < binding.y &&
        i2 >= binding.x && (i2 - binding.x) < binding.y;
    if (!inSkinnedRange) {
        return fallbackLocalPos;
    }

    LocalVertex v0 = gpu_skinning_rt_previous_vertices[i0];
    LocalVertex v1 = gpu_skinning_rt_previous_vertices[i1];
    LocalVertex v2 = gpu_skinning_rt_previous_vertices[i2];
    return v0.position_uv_x.xyz * barycentrics.x +
        v1.position_uv_x.xyz * barycentrics.y +
        v2.position_uv_x.xyz * barycentrics.z;
}

uint compute_surface_velocity(
    vec3 currentWorldPos,
    vec3 localPos,
    uint instanceId,
    uint meshId,
    uint primitiveId,
    vec3 barycentrics,
    ivec2 dims) {
    vec3 previousWorldPos = currentWorldPos;
    if (instanceId < mesh_params.instance_count) {
        InstanceRecord instance = instance_records[instanceId];
        vec3 previousLocalPos = previous_skinned_local_position(meshId, primitiveId, barycentrics, localPos);
        previousWorldPos = (instance.prev_transform * vec4(previousLocalPos, 1.0)).xyz;
    }
    return compute_surface_velocity_from_previous_world(currentWorldPos, previousWorldPos, dims);
}

uint compute_sky_velocity(vec3 rayDir, ivec2 dims) {
    vec3 currentWorldPos = prev_camera.current_pos.xyz + normalize(rayDir) * 512.0;
    vec3 previousWorldPos = prev_camera.prev_pos.xyz + normalize(rayDir) * 512.0;
    vec2 currentPos;
    vec2 previousPos;
    bool valid = project_unjittered_to_pixels_checked(prev_camera.view_proj, prev_camera.jitter.xy, currentWorldPos, dims, currentPos) &&
        project_unjittered_to_pixels_checked(prev_camera.prev_view_proj, prev_camera.jitter.zw, previousWorldPos, dims, previousPos);
    return valid ? pack_velocity_pixels(currentPos - previousPos) : pack_invalid_velocity_pixels();
}

uint pcg_hash(uint inputValue) {
    uint state = inputValue * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec3 id_color(uint id) {
    uint h = pcg_hash(id + 1u);
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

float rand_f32(inout uint state) {
    state = pcg_hash(state);
    return float(state) / float(0xffffffffu);
}

vec3 rand_unit_vector(inout uint state) {
    float z = rand_f32(state) * 2.0 - 1.0;
    float a = rand_f32(state) * 2.0 * 3.141592653589793;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(a), r * sin(a), z);
}

Material decode_material(uint mat_idx) {
    uint idx = min(mat_idx, max(mesh_params.material_count, 1u) - 1u) * MATERIAL_STRIDE;
    vec4 d0 = mesh_materials[idx + 0u];
    vec4 d1 = mesh_materials[idx + 1u];
    vec4 d2 = mesh_materials[idx + 2u];
#if RTV_MATERIAL_TEXTURES_ENABLED
    vec4 d3 = mesh_materials[idx + 3u];
#endif
    vec4 d4 = mesh_materials[idx + 4u];
    vec4 d5 = mesh_materials[idx + 5u];
    vec4 d6 = mesh_materials[idx + 6u];
    vec4 d7 = mesh_materials[idx + 7u];
    vec4 d8 = mesh_materials[idx + 8u];
    vec4 d9 = mesh_materials[idx + 9u];
    vec4 d10 = mesh_materials[idx + 10u];
    vec4 d11 = mesh_materials[idx + 11u];
    vec4 d12 = mesh_materials[idx + 12u];
    vec4 d13 = mesh_materials[idx + 13u];
    vec4 d14 = mesh_materials[idx + 14u];
    vec4 d15 = mesh_materials[idx + 15u];
    vec4 d16 = mesh_materials[idx + 16u];
#if RTV_MATERIAL_TEXTURES_ENABLED
    vec4 d17 = mesh_materials[idx + 17u];
#endif
    Material m;
    m.material_data_index = idx;
    m.color = d0.xyz;
    m.roughness = d0.w;
    m.ior = d1.x;
    m.mat_type = uint(d1.y);
    m.metallic = d1.z;
    m.pad2 = d1.w;
    m.emissive = d2.xyz;
    m.alpha_factor = d2.w;
#if RTV_MATERIAL_TEXTURES_ENABLED
    m.base_color_texture = int(round(d3.x));
    m.normal_texture = int(round(d3.y));
    m.metallic_roughness_texture = int(round(d3.z));
    m.emissive_texture = int(round(d3.w));
#else
    m.base_color_texture = -1;
    m.normal_texture = -1;
    m.metallic_roughness_texture = -1;
    m.emissive_texture = -1;
#endif
    m.alpha_cutoff = d4.x;
    m.alpha_mode = uint(round(d4.y));
    m.double_sided = uint(round(d4.z));
    m.conductor_eta = d5.xyz;
    m.use_conductor_optics = uint(round(d5.w));
    m.conductor_k = d6.xyz;
    m.anisotropy_strength = d7.x;
    m.anisotropy_rotation = d7.y;
    m.occlusion_texture = int(round(d7.z));
    m.occlusion_strength = clamp(d7.w, 0.0, 1.0);
    m.sheen_color = max(d8.xyz, vec3(0.0));
    m.sheen_roughness = clamp(d8.w, 0.0, 1.0);
    m.sheen_color_texture = int(round(d9.x));
    m.sheen_roughness_texture = int(round(d9.y));
    m.iridescence_factor = clamp(d9.z, 0.0, 1.0);
    m.iridescence_ior = max(d9.w, 1.01);
    m.iridescence_thickness_min = max(d10.x, 0.0);
    m.iridescence_thickness_max = max(d10.y, m.iridescence_thickness_min);
    m.iridescence_texture = int(round(d10.z));
    m.iridescence_thickness_texture = int(round(d10.w));
    m.occlusion = 1.0;
    m.normal_variance = 0.0;
    m.clearcoat_factor = clamp(d11.x, 0.0, 1.0);
    m.clearcoat_roughness = clamp(d11.y, 0.0, 1.0);
    m.clearcoat_texture = int(round(d11.z));
    m.clearcoat_roughness_texture = int(round(d11.w));
    m.clearcoat_normal_texture = int(round(d12.x));
    m.transmission_factor = clamp(d12.y, 0.0, 1.0);
    m.transmission_texture = int(round(d12.z));
    m.specular_factor = clamp(d12.w, 0.0, 1.0);
    m.specular_color = max(d13.xyz, vec3(0.0));
    m.specular_texture = int(round(d13.w));
    m.specular_color_texture = int(round(d14.x));
    m.anisotropy_texture = int(round(d14.y));
    m.volume_thickness_factor = max(d14.z, 0.0);
    m.volume_thickness_texture = int(round(d14.w));
    m.volume_attenuation_color = clamp(d15.xyz, vec3(0.0), vec3(1.0));
    m.volume_attenuation_distance = d15.w;
    m.dispersion_factor = max(d16.x, 0.0);
#if RTV_MATERIAL_TEXTURES_ENABLED
    m.opacity_texture = int(round(d17.x));
    m.height_texture = int(round(d17.y));
    m.height_scale = d17.z;
#else
    m.opacity_texture = -1;
    m.height_texture = -1;
    m.height_scale = 0.0;
#endif
    m.clearcoat_normal = vec3(0.0);
    m.clearcoat_normal_variance = 0.0;
    return m;
}

bool material_static_may_be_transmissive(uint mat_idx) {
    uint idx = min(mat_idx, max(mesh_params.material_count, 1u) - 1u) * MATERIAL_STRIDE;
    vec4 d1 = mesh_materials[idx + 1u];
    return (uint(round(d1.w)) & MATERIAL_FLAG_MAY_BE_TRANSMISSIVE) != 0u;
}

uint material_for_triangle_index(uint triangleIndex) {
    uint triangleCount = mesh_params.local_index_count / 3u;
    if (triangleIndex >= triangleCount) {
        return 0u;
    }
    return rt_triangle_material_ids[triangleIndex];
}

uint material_for_raw_triangle(uint meshFirstIndex, uint primitiveId) {
    uint triangleIndex = meshFirstIndex / 3u + primitiveId;
    return material_for_triangle_index(triangleIndex);
}

uint scene_instance_index_from_tlas_record(uint tlasRecordIndex) {
    if (tlasRecordIndex < rt_tlas_geometry_ranges.length()) {
        uvec4 tlasRecord = rt_tlas_geometry_ranges[tlasRecordIndex];
        if (tlasRecord.y != 0u && tlasRecord.z != 0xffffffffu) {
            return tlasRecord.z;
        }
    }
    return tlasRecordIndex;
}

uint geometry_triangle_offset(uint meshIndex, uint tlasRecordIndex, uint geometryIndex, uint meshFirstIndex) {
    if (tlasRecordIndex < rt_tlas_geometry_ranges.length()) {
        uvec4 tlasRange = rt_tlas_geometry_ranges[tlasRecordIndex];
        if (tlasRange.y != 0u) {
            return rt_geometry_triangle_offsets[tlasRange.x + min(geometryIndex, tlasRange.y - 1u)];
        }
    }
    if (meshIndex >= mesh_params.mesh_count) {
        return meshFirstIndex / 3u;
    }
    uvec2 range = rt_mesh_geometry_ranges[meshIndex];
    if (range.y == 0u) {
        return meshFirstIndex / 3u;
    }
    return rt_geometry_triangle_offsets[range.x + min(geometryIndex, range.y - 1u)];
}

uvec4 ray_tracing_gpu_skinning_binding(uint meshIndex) {
    if (meshIndex < mesh_params.mesh_count) {
        return gpu_skinning_rt_mesh_bindings[meshIndex];
    }
    return uvec4(0u);
}

LocalVertex ray_tracing_local_vertex_with_binding(uvec4 binding, uint vertexIndex) {
    if (binding.z != 0u && vertexIndex >= binding.x && (vertexIndex - binding.x) < binding.y) {
        return gpu_skinning_rt_vertices[vertexIndex];
    }
    return local_mesh_vertices[vertexIndex];
}

LocalVertex ray_tracing_local_vertex(uint meshIndex, uint vertexIndex) {
    return ray_tracing_local_vertex_with_binding(ray_tracing_gpu_skinning_binding(meshIndex), vertexIndex);
}

bool ray_tracing_triangle_indices(uint triangleIndex, out uvec3 indices) {
    indices = uvec3(0u);
    if (triangleIndex > 0x55555555u) {
        return false;
    }

    uint triIndex = triangleIndex * 3u;
    if (triIndex + 2u >= mesh_params.local_index_count) {
        return false;
    }

    indices = uvec3(
        local_mesh_indices[triIndex + 0u],
        local_mesh_indices[triIndex + 1u],
        local_mesh_indices[triIndex + 2u]);
    return indices.x < mesh_params.local_vertex_count &&
        indices.y < mesh_params.local_vertex_count &&
        indices.z < mesh_params.local_vertex_count;
}

bool ray_tracing_mesh_has_gpu_skinning(uint meshIndex) {
    return meshIndex < mesh_params.mesh_count && gpu_skinning_rt_mesh_bindings[meshIndex].z != 0u;
}

vec2 material_texture_uv(Material material, uint slot, vec2 uv0, vec2 uv1) {
    if (slot >= MATERIAL_TEXTURE_TRANSFORM_COUNT) {
        return uv0;
    }
    uint transformBase = material.material_data_index + MATERIAL_TEXTURE_TRANSFORM_BASE + slot * 2u;
    vec4 transform1 = mesh_materials[transformBase + 1u];
    uint texCoord = uint(round(transform1.z));
    return texCoord == 1u ? uv1 : uv0;
}

vec2 apply_material_texture_transform(Material material, uint slot, vec2 uv0, vec2 uv1) {
    vec2 uv = material_texture_uv(material, slot, uv0, uv1);
    if (slot >= MATERIAL_TEXTURE_TRANSFORM_COUNT) {
        return uv;
    }
    uint transformBase = material.material_data_index + MATERIAL_TEXTURE_TRANSFORM_BASE + slot * 2u;
    vec4 transform0 = mesh_materials[transformBase + 0u];
    vec4 transform1 = mesh_materials[transformBase + 1u];
    if (uint(round(transform1.y)) == 0u) {
        return uv;
    }
    vec2 scaled = uv * transform0.zw;
    float c = cos(transform1.x);
    float s = sin(transform1.x);
    vec2 rotated = vec2(c * scaled.x - s * scaled.y, s * scaled.x + c * scaled.y);
    return transform0.xy + rotated;
}

float g_material_texture_lod = 0.0;

void set_material_texture_lod(float lod) {
#if RTV_MATERIAL_TEXTURES_ENABLED
    g_material_texture_lod = clamp(lod, 0.0, 4.0);
#endif
}

vec4 sample_material_texture(int textureIndex, vec2 uv) {
    return textureLod(material_textures[nonuniformEXT(textureIndex)], uv, g_material_texture_lod);
}

vec2 apply_material_height_parallax(Material material, vec2 uv0, vec2 uv1, vec3 normal, vec3 tangent, vec3 bitangent, vec3 rayDirection) {
    if (material.height_texture < 0 || material.height_texture >= MATERIAL_TEXTURE_LIMIT) {
        return uv0;
    }
    vec3 n = normalize(normal);
    vec3 t = normalize(tangent);
    vec3 b = normalize(bitangent);
    vec3 viewDir = normalize(-rayDirection);
    vec3 viewTs = vec3(dot(viewDir, t), dot(viewDir, b), max(dot(viewDir, n), 0.08));
    float height = sample_material_texture(material.height_texture, uv0).r;
    float centeredHeight = clamp(height, 0.0, 1.0) - 0.5;
    return uv0 + (viewTs.xy / viewTs.z) * centeredHeight * clamp(material.height_scale, 0.0, 0.25);
}

void apply_material_textures(inout Material material, vec2 uv0, vec2 uv1) {
#if RTV_MATERIAL_TEXTURES_ENABLED
    uint flags = uint(round(material.pad2));
    if (material.base_color_texture >= 0 && material.base_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.base_color_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_BASE_COLOR, uv0, uv1);
        vec4 base = sample_material_texture(textureIndex, sampleUv);
        vec3 baseColor = (flags & MATERIAL_FLAG_MANUAL_BASE_COLOR_SRGB) != 0u
            ? pow(max(base.rgb, vec3(0.0)), vec3(2.2))
            : base.rgb;
        material.color *= baseColor;
        material.alpha_factor *= base.a;
    }
    if (material.metallic_roughness_texture >= 0 && material.metallic_roughness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.metallic_roughness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_METALLIC_ROUGHNESS, uv0, uv1);
        vec4 mr = sample_material_texture(textureIndex, sampleUv);
        material.roughness = clamp(material.roughness * mr.g, 0.0, 1.0);
        material.metallic = clamp(material.metallic * mr.b, 0.0, 1.0);
        if (material.mat_type == 0u || material.mat_type == 3u) {
            material.mat_type = 3u;
        }
    }
    if (material.emissive_texture >= 0 && material.emissive_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.emissive_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_EMISSIVE, uv0, uv1);
        vec4 emissive = sample_material_texture(textureIndex, sampleUv);
        vec3 emissiveColor = (flags & MATERIAL_FLAG_MANUAL_EMISSIVE_SRGB) != 0u
            ? pow(max(emissive.rgb, vec3(0.0)), vec3(2.2))
            : emissive.rgb;
        material.emissive *= emissiveColor;
    }
    if (material.occlusion_texture >= 0 && material.occlusion_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.occlusion_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_OCCLUSION, uv0, uv1);
        float ao = sample_material_texture(textureIndex, sampleUv).r;
        material.occlusion = mix(1.0, clamp(ao, 0.0, 1.0), material.occlusion_strength);
    }
    if (material.sheen_color_texture >= 0 && material.sheen_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.sheen_color_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_SHEEN_COLOR, uv0, uv1);
        material.sheen_color *= sample_material_texture(textureIndex, sampleUv).rgb;
    }
    if (material.sheen_roughness_texture >= 0 && material.sheen_roughness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.sheen_roughness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_SHEEN_ROUGHNESS, uv0, uv1);
        material.sheen_roughness = clamp(material.sheen_roughness * sample_material_texture(textureIndex, sampleUv).a, 0.0, 1.0);
    }
    if (material.iridescence_texture >= 0 && material.iridescence_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.iridescence_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_IRIDESCENCE, uv0, uv1);
        material.iridescence_factor = clamp(material.iridescence_factor * sample_material_texture(textureIndex, sampleUv).r, 0.0, 1.0);
    }
    if (material.iridescence_thickness_texture >= 0 && material.iridescence_thickness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.iridescence_thickness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_IRIDESCENCE_THICKNESS, uv0, uv1);
        float thicknessMix = sample_material_texture(textureIndex, sampleUv).g;
        material.iridescence_thickness_min = mix(material.iridescence_thickness_min, material.iridescence_thickness_max, clamp(thicknessMix, 0.0, 1.0));
    }
    if (material.clearcoat_texture >= 0 && material.clearcoat_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.clearcoat_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT, uv0, uv1);
        material.clearcoat_factor = clamp(material.clearcoat_factor * sample_material_texture(textureIndex, sampleUv).r, 0.0, 1.0);
    }
    if (material.clearcoat_roughness_texture >= 0 && material.clearcoat_roughness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.clearcoat_roughness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT_ROUGHNESS, uv0, uv1);
        material.clearcoat_roughness = clamp(material.clearcoat_roughness * sample_material_texture(textureIndex, sampleUv).g, 0.0, 1.0);
    }
    if (material.transmission_texture >= 0 && material.transmission_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.transmission_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_TRANSMISSION, uv0, uv1);
        material.transmission_factor = clamp(material.transmission_factor * sample_material_texture(textureIndex, sampleUv).r, 0.0, 1.0);
    }
    if (material.specular_texture >= 0 && material.specular_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.specular_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_SPECULAR, uv0, uv1);
        vec4 specularSample = sample_material_texture(textureIndex, sampleUv);
        if ((flags & MATERIAL_FLAG_SPECULAR_ALPHA_GLOSSINESS) != 0u) {
            float glossinessFactor = clamp(1.0 - material.roughness, 0.0, 1.0);
            material.roughness = clamp(1.0 - glossinessFactor * specularSample.a, 0.0, 1.0);
        } else {
            material.specular_factor = clamp(material.specular_factor * specularSample.a, 0.0, 1.0);
        }
        if ((flags & MATERIAL_FLAG_SPECULAR_GLOSSINESS_WORKFLOW) != 0u) {
            material.specular_color *= max(specularSample.rgb, vec3(0.0));
        }
    }
    if (material.specular_color_texture >= 0 && material.specular_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.specular_color_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_SPECULAR_COLOR, uv0, uv1);
        material.specular_color *= max(sample_material_texture(textureIndex, sampleUv).rgb, vec3(0.0));
    }
    if (material.anisotropy_texture >= 0 && material.anisotropy_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.anisotropy_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_ANISOTROPY, uv0, uv1);
        vec3 anisotropySample = sample_material_texture(textureIndex, sampleUv).rgb;
        material.anisotropy_strength = clamp(material.anisotropy_strength * anisotropySample.b, -1.0, 1.0);
        vec2 direction = anisotropySample.rg * 2.0 - 1.0;
        if (dot(direction, direction) > 1.0e-6) {
            material.anisotropy_rotation += atan(direction.y, direction.x);
        }
    }
    if (material.volume_thickness_texture >= 0 && material.volume_thickness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.volume_thickness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_VOLUME_THICKNESS, uv0, uv1);
        material.volume_thickness_factor *= max(sample_material_texture(textureIndex, sampleUv).g, 0.0);
    }
    if (material.opacity_texture >= 0 && material.opacity_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.opacity_texture;
        float opacity = sample_material_texture(textureIndex, uv0).r;
        material.alpha_factor *= clamp(opacity, 0.0, 1.0);
    }
#endif
}

vec3 material_volume_transmittance(Material material, float distance) {
    float thickness = max(material.volume_thickness_factor, 0.0);
    if (thickness <= 1.0e-6) {
        return vec3(1.0);
    }
    float attenuationDistance = material.volume_attenuation_distance;
    if (attenuationDistance <= 0.0 || attenuationDistance >= 1.0e20) {
        return vec3(1.0);
    }
    vec3 color = clamp(material.volume_attenuation_color, vec3(1.0e-4), vec3(1.0));
    vec3 attenuationCoefficient = -log(color) / max(attenuationDistance, 1.0e-6);
    return exp(-attenuationCoefficient * max(distance * thickness, 0.0));
}

void apply_material_vertex_color(inout Material material, vec4 vertexColor) {
    material.color *= clamp(vertexColor.rgb, vec3(0.0), vec3(1.0));
    material.alpha_factor *= clamp(vertexColor.a, 0.0, 1.0);
}

void apply_material_alpha_texture(inout Material material, vec2 uv0, vec2 uv1) {
#if RTV_MATERIAL_TEXTURES_ENABLED
    if (material.base_color_texture >= 0 && material.base_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.base_color_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_BASE_COLOR, uv0, uv1);
        vec4 base = sample_material_texture(textureIndex, sampleUv);
        material.alpha_factor *= base.a;
    }
    if (material.opacity_texture >= 0 && material.opacity_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.opacity_texture;
        float opacity = sample_material_texture(textureIndex, uv0).r;
        material.alpha_factor *= clamp(opacity, 0.0, 1.0);
    }
#endif
}

bool accept_material_alpha(Material material) {
    if (material.alpha_mode == ALPHA_MODE_MASK) {
        return material.alpha_factor >= material.alpha_cutoff;
    }
    if (material.alpha_mode == ALPHA_MODE_BLEND) {
        return material.alpha_factor >= 0.10;
    }
    return true;
}

vec3 apply_normal_texture(inout Material material, vec2 uv0, vec2 uv1, vec3 normal, vec3 tangent, vec3 bitangent, vec3 rayDirection) {
    if (material.normal_texture < 0 || material.normal_texture >= MATERIAL_TEXTURE_LIMIT) {
        return normal;
    }
    int textureIndex = material.normal_texture;
    vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_NORMAL, uv0, uv1);
    vec3 tangentSample = sample_material_texture(textureIndex, sampleUv).xyz * 2.0 - 1.0;
    uint flags = uint(round(material.pad2));
    if ((flags & MATERIAL_FLAG_NORMAL_MAP_DIRECTX) != 0u) {
        tangentSample.y = -tangentSample.y;
    }
    if (tangentSample.z < 0.0) {
        tangentSample.z = sqrt(max(1.0 - dot(tangentSample.xy, tangentSample.xy), 0.0));
    }
    material.normal_variance = clamp(dot(tangentSample.xy, tangentSample.xy), 0.0, 1.0);
    vec3 t = normalize(tangent - normal * dot(normal, tangent));
    vec3 b = normalize(bitangent - normal * dot(normal, bitangent));
    vec3 mapped = normalize(t * tangentSample.x + b * tangentSample.y + normal * max(tangentSample.z, 0.0));
    return dot(mapped, rayDirection) > 0.0 ? -mapped : mapped;
}

vec3 apply_clearcoat_normal_texture(inout Material material, vec2 uv0, vec2 uv1, vec3 normal, vec3 tangent, vec3 bitangent, vec3 rayDirection) {
    material.clearcoat_normal = normal;
    material.clearcoat_normal_variance = 0.0;
    if (material.clearcoat_normal_texture < 0 || material.clearcoat_normal_texture >= MATERIAL_TEXTURE_LIMIT) {
        return normal;
    }
    int textureIndex = material.clearcoat_normal_texture;
    vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT_NORMAL, uv0, uv1);
    vec3 tangentSample = sample_material_texture(textureIndex, sampleUv).xyz * 2.0 - 1.0;
    uint flags = uint(round(material.pad2));
    if ((flags & MATERIAL_FLAG_NORMAL_MAP_DIRECTX) != 0u) {
        tangentSample.y = -tangentSample.y;
    }
    if (tangentSample.z < 0.0) {
        tangentSample.z = sqrt(max(1.0 - dot(tangentSample.xy, tangentSample.xy), 0.0));
    }
    material.clearcoat_normal_variance = clamp(dot(tangentSample.xy, tangentSample.xy), 0.0, 1.0);
    vec3 t = normalize(tangent - normal * dot(normal, tangent));
    vec3 b = normalize(bitangent - normal * dot(normal, bitangent));
    vec3 mapped = normalize(t * tangentSample.x + b * tangentSample.y + normal * max(tangentSample.z, 0.0));
    material.clearcoat_normal = dot(mapped, rayDirection) > 0.0 ? -mapped : mapped;
    return material.clearcoat_normal;
}

vec3 material_clearcoat_normal(Material material, vec3 fallbackNormal) {
    return dot(material.clearcoat_normal, material.clearcoat_normal) > 1.0e-8
        ? normalize(material.clearcoat_normal)
        : fallbackNormal;
}

bool specular_aa_enabled() {
    return camera.restir_gi_controls.w != 0u;
}

float material_specular_roughness(Material material) {
    float roughness = material.roughness;
    if (!specular_aa_enabled() || material.normal_variance <= 1.0e-5) {
        return roughness;
    }
    float r2 = roughness * roughness;
    float aaVariance = material.normal_variance * 0.18;
    return clamp(sqrt(clamp(r2 + aaVariance, 0.0, 1.0)), roughness, 1.0);
}

Material apply_debug_material_mode(Material material) {
    if (renderer_debug_view() == 22u || renderer_debug_view() == 27u) {
        material.color = vec3(0.72, 0.70, 0.66);
        material.roughness = 0.85;
        material.metallic = 0.0;
        material.mat_type = 0u;
        material.emissive = vec3(0.0);
    }
    return material;
}

void apply_terminal_material_textures(inout Material material, vec2 uv0, vec2 uv1) {
    uint flags = uint(round(material.pad2));
    if (material.base_color_texture >= 0 && material.base_color_texture < MATERIAL_TEXTURE_LIMIT) {
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_BASE_COLOR, uv0, uv1);
        vec4 base = sample_material_texture(material.base_color_texture, sampleUv);
        material.color *= (flags & MATERIAL_FLAG_MANUAL_BASE_COLOR_SRGB) != 0u
            ? pow(max(base.rgb, vec3(0.0)), vec3(2.2))
            : base.rgb;
        material.alpha_factor *= base.a;
    }
    if (material.emissive_texture >= 0 && material.emissive_texture < MATERIAL_TEXTURE_LIMIT) {
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_EMISSIVE, uv0, uv1);
        vec3 emissive = sample_material_texture(material.emissive_texture, sampleUv).rgb;
        material.emissive *= (flags & MATERIAL_FLAG_MANUAL_EMISSIVE_SRGB) != 0u
            ? pow(max(emissive, vec3(0.0)), vec3(2.2))
            : emissive;
    }
}


#endif // RTV_RT_MATERIAL_GLSL
