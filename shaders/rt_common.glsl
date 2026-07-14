#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require

#ifndef RTV_BEAUTY_OUTPUT_ONLY
#define RTV_BEAUTY_OUTPUT_ONLY 0
#endif

#include "atmosphere_phase.glsl"
#define RTV_STBN_TEXTURE_ENABLED 1
#include "blue_noise.glsl"

#ifndef RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
#define RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT 0
#endif

#include "rt_resources.glsl"

uint renderer_debug_view() {
#if RTV_BEAUTY_OUTPUT_ONLY
    return 0u;
#else
    return debug_params.view;
#endif
}

#include "rt_material.glsl"
#include "rt_environment.glsl"
#include "rt_scene_lighting.glsl"
#include "rt_bsdf.glsl"

#define RTV_RESTIR_DI_COMPUTE_ACCESSORS 0
#include "restir_di_types_accessors.glsl"
#include "rt_restir_di_bindings.glsl"
