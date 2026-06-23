// PathDataRecord — shared between raygen and compute passes.
// Separated from rt_common.glsl so GI compute shaders can consume it
// without pulling in legacy reservoir definitions.
//
// Include this BEFORE any file that defines RestirGiReservoir.

#ifndef RTV_PATH_DATA_COMMON_GLSL
#define RTV_PATH_DATA_COMMON_GLSL

struct PathDataRecord {
    vec4 direct_diffuse;
    vec4 direct_specular;
    vec4 indirect_diffuse;
    vec4 indirect_specular;
    vec4 albedo_roughness_hit_confidence;
    vec4 material_specular_albedo;
    vec4 denoiser_hit_distance;
    vec4 diffuse_ray_direction_hit_distance;
    vec4 specular_ray_direction_hit_distance;
    vec4 emissive_residual;
    vec4 restir_gi_fallback_reactive; // rgb = current one-sample GI term, a = GI reactive mask
};

#endif // RTV_PATH_DATA_COMMON_GLSL
