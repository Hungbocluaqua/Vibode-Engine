#ifndef RTV_ATMOSPHERE_LUT_SAMPLING_GLSL
#define RTV_ATMOSPHERE_LUT_SAMPLING_GLSL

#include "atmosphere_common.glsl"

vec2 atmosphere_sky_lut_uv(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

vec3 sample_atmosphere_sky_view_lut(vec3 dir) {
    vec2 uv = atmosphere_sky_lut_uv(dir);
    return texture(sampler2D(atmosphere_sky_view_lut, atmosphere_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
}

vec3 sample_atmosphere_transmittance_lut(vec3 worldPos, vec3 dir) {
    vec3 planetary = atmosphere_scene_to_planetary(worldPos);
    float heightMeters = max(length(planetary) - ATMOSPHERE_PLANET_RADIUS, 0.0);
    float mu = dot(normalize(dir), normalize(planetary));
    float atmosphereHeight = max(ATMOSPHERE_TOP_RADIUS - ATMOSPHERE_PLANET_RADIUS, 1.0);
    vec2 uv = vec2(clamp((mu + 0.20) / 1.20, 0.0, 1.0), clamp(heightMeters / atmosphereHeight, 0.0, 1.0));
    vec3 sampled = texture(sampler2D(atmosphere_transmittance_lut, atmosphere_sampler), uv).rgb;
    float lum = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    return lum > 1.0e-5 ? sampled : vec3(1.0);
}

vec3 sample_atmosphere_multi_scatter_lut(vec3 dir) {
    vec3 sunDir = analytical_sun_direction();
    float viewMu = clamp(normalize(dir).y, -0.20, 1.0);
    float sunMu = clamp(sunDir.y, -0.20, 1.0);
    vec2 uv = vec2(clamp((viewMu + 0.20) / 1.20, 0.0, 1.0), clamp((sunMu + 0.20) / 1.20, 0.0, 1.0));
    return texture(sampler2D(atmosphere_multi_scatter_lut, atmosphere_sampler), uv).rgb;
}

void sample_atmosphere_aerial_perspective(vec3 worldPos, vec3 viewDir, out vec3 inscatter, out float transmittance) {
    float distanceMeters = length(worldPos - camera.pos.xyz);
    if (distanceMeters <= 0.0 || distanceMeters >= 100000.0) {
        inscatter = vec3(0.0);
        transmittance = 1.0;
        return;
    }
    vec3 dirNorm = normalize(viewDir);
    vec3 planetary = atmosphere_scene_to_planetary(camera.pos.xyz);
    float cosZenith = clamp(dot(dirNorm, normalize(planetary)), -1.0, 1.0);
    float heightMeters = max(length(planetary) - ATMOSPHERE_PLANET_RADIUS, 0.0);
    float atmosphereHeight = max(ATMOSPHERE_TOP_RADIUS - ATMOSPHERE_PLANET_RADIUS, 1.0);
    float distanceNormalized = clamp((max(distanceMeters, 1.0) - 1.0) / (100000.0 - 1.0), 0.0, 1.0);
    float depthNormalized = pow(distanceNormalized, 1.0 / 3.0);
    vec3 uvw = vec3(cosZenith * 0.5 + 0.5, clamp(heightMeters / atmosphereHeight, 0.0, 1.0), clamp(depthNormalized, 0.0, 1.0));
    vec4 aerial = texture(sampler3D(atmosphere_aerial_perspective_lut, atmosphere_sampler), uvw);
    float lum = dot(aerial.rgb, vec3(0.2126, 0.7152, 0.0722));
    if (aerial.a <= 1.0e-5 && lum <= 1.0e-5) {
        inscatter = vec3(0.0);
        transmittance = 1.0;
        return;
    }
    inscatter = max(aerial.rgb, vec3(0.0));
    transmittance = clamp(aerial.a, 0.0, 1.0);
}

#endif
