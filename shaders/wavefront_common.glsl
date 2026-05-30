#ifndef RTV_WAVEFRONT_COMMON_GLSL
#define RTV_WAVEFRONT_COMMON_GLSL

#define WAVEFRONT_QUEUE_HEADER_STRIDE_BYTES 32u
#define WAVEFRONT_RAY_STRIDE_BYTES 48u
#define WAVEFRONT_HIT_STRIDE_BYTES 112u
#define WAVEFRONT_SHADOW_RAY_STRIDE_BYTES 48u
#define WAVEFRONT_PIXEL_STATE_STRIDE_BYTES 112u
#define WAVEFRONT_SORT_BUCKET_COUNT 32u
#define WAVEFRONT_SHADOW_LIGHT_TYPE_SHIFT 24u
#define WAVEFRONT_SHADOW_LIGHT_TYPE_MASK 0xffu
#define WAVEFRONT_SHADOW_FLAGS_MASK 0x00ffffffu
#define WAVEFRONT_HALF_MAX 65504.0

struct WavefrontQueueHeader {
    uvec4 counters; // x=count, y=capacity, z=read offset, w=write offset
    uvec4 metadata; // x=max path depth, y=frame index, z=clear validation counter, w=flags
};

struct WavefrontRay {
    vec4 origin_tmin;
    vec4 direction_tmax;
    uvec4 pixel_depth_rng_flags;
};

struct WavefrontHit {
    vec4 position_t;
    vec4 normal_roughness;
    vec4 barycentrics_hit_kind;
    vec4 geom_normal;
    vec4 tangent;
    uvec4 material_instance_primitive;
    uvec4 pixel_depth_flags;
};

struct WavefrontShadowRay {
    vec4 origin_tmin;
    vec4 direction_tmax;
    uvec4 radiance_pdf_pixel_light;
};

uvec2 wavefront_pack_shadow_radiance_pdf(vec3 radiance, float pdf) {
    vec3 packed_radiance = clamp(max(radiance, vec3(0.0)), vec3(0.0), vec3(WAVEFRONT_HALF_MAX));
    float packed_pdf = clamp(max(pdf, 0.0), 0.0, WAVEFRONT_HALF_MAX);
    return uvec2(
        packHalf2x16(packed_radiance.rg),
        packHalf2x16(vec2(packed_radiance.b, packed_pdf)));
}

vec4 wavefront_unpack_shadow_radiance_pdf(uvec2 packed_value) {
    vec2 rg = unpackHalf2x16(packed_value.x);
    vec2 b_pdf = unpackHalf2x16(packed_value.y);
    return vec4(rg, b_pdf.x, b_pdf.y);
}

uint wavefront_pack_shadow_light_flags(uint light_type, uint flags) {
    return ((light_type & WAVEFRONT_SHADOW_LIGHT_TYPE_MASK) << WAVEFRONT_SHADOW_LIGHT_TYPE_SHIFT) |
        (flags & WAVEFRONT_SHADOW_FLAGS_MASK);
}

uint wavefront_shadow_light_type(uint packed_light_flags) {
    return (packed_light_flags >> WAVEFRONT_SHADOW_LIGHT_TYPE_SHIFT) & WAVEFRONT_SHADOW_LIGHT_TYPE_MASK;
}

uint wavefront_shadow_flags(uint packed_light_flags) {
    return packed_light_flags & WAVEFRONT_SHADOW_FLAGS_MASK;
}

struct WavefrontPixelState {
    vec4 radiance;
    vec4 throughput;
    vec4 direct_lighting;
    vec4 indirect_lighting;
    vec4 atmosphere_transmittance;
    uvec4 rng_depth_flags;
    uvec4 material_instance_primitive;
};

#endif
