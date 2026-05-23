#ifndef RTV_ATMOSPHERE_LIGHTING_GLSL
#define RTV_ATMOSPHERE_LIGHTING_GLSL

#include "atmosphere_common.glsl"

vec3 analytical_sun_direction() {
    float cosElev = cos(camera.sun_elevation - 0.5 * PI);
    float sinElev = sin(camera.sun_elevation - 0.5 * PI);
    float sunAzimuth = camera.sun_azimuth;
    float cosAzi = cos(sunAzimuth);
    float sinAzi = sin(sunAzimuth);
    return normalize(vec3(sinElev * sinAzi, cosElev, -sinElev * cosAzi));
}

vec3 analytical_sun_center_radiance() {
    if (camera.sunlight_enabled != 0u) {
        float sunHeight = smoothstep(-0.08, 0.22, analytical_sun_direction().y);
        vec3 sunsetTint = vec3(1.0, 0.46, 0.16);
        vec3 noonTint = vec3(1.0, 0.96, 0.84);
        return mix(sunsetTint, noonTint, sunHeight) * camera.sun_intensity * 32.0;
    }
    return vec3(0.0);
}

vec3 analytical_sun_disk_radiance(vec3 dir) {
    vec3 sunDir = analytical_sun_direction();
    float cosAngle = clamp(dot(normalize(dir), sunDir), -1.0, 1.0);
    float radius = camera.sun_angular_radius;
    float cosRadius = cos(radius);
    float disk = smoothstep(cosRadius, mix(cosRadius, 1.0, 0.18), cosAngle);
    float limb = 0.62 + 0.38 * sqrt(clamp((cosAngle - cosRadius) / max(1.0 - cosRadius, 1.0e-5), 0.0, 1.0));
    return analytical_sun_center_radiance() * disk * limb;
}

vec3 fast_sky_radiance(vec3 dir) {
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    float viewUp = clamp(viewDir.y, -0.08, 1.0);
    float sunUp = clamp(sunDir.y, -0.08, 1.0);
    float cosTheta = clamp(dot(viewDir, sunDir), -1.0, 1.0);
    float sunVisibility = smoothstep(-0.06, 0.08, sunUp);
    float viewMass = atmosphere_air_mass(viewUp);
    float sunMass = atmosphere_air_mass(sunUp);
    float horizon = pow(1.0 - clamp(viewUp, 0.0, 1.0), 2.0);

    vec3 rayleighBeta = vec3(0.170, 0.398, 0.970);
    vec3 mieBeta = vec3(0.82, 0.74, 0.62);
    vec3 transmittance = exp(-(rayleighBeta * 0.30 + mieBeta * 0.08) * (viewMass + sunMass * 0.65));
    vec3 sunsetScatter = vec3(1.0, 0.42, 0.12) * smoothstep(-0.08, 0.18, horizon) * (1.0 - smoothstep(0.05, 0.55, sunUp));

    vec3 rayleigh = rayleighBeta * atmosphere_rayleigh_phase(cosTheta) * (0.55 + horizon * 0.75);
    vec3 mie = mieBeta * atmosphere_mie_phase(cosTheta, 0.78) * (0.05 + horizon * 0.26);
    vec3 sky = (rayleigh + mie + sunsetScatter * 0.22) * transmittance * sunVisibility;

    vec3 night = vec3(0.004, 0.006, 0.012) * smoothstep(-0.25, -0.05, sunUp);
    return max(sky * camera.sky_intensity * max(camera.atmosphere.w, 0.1) + night * camera.sky_intensity, vec3(0.0));
}

vec3 sun_transmittance(vec3 worldPos, vec3 sunDir) {
    vec3 planetary = atmosphere_scene_to_planetary(worldPos);
    float heightMeters = max(length(planetary) - ATMOSPHERE_PLANET_RADIUS, 0.0);
    vec3 d = normalize(sunDir);
    float mu = dot(d, normalize(planetary));
    float atmosphereHeight = max(ATMOSPHERE_TOP_RADIUS - ATMOSPHERE_PLANET_RADIUS, 1.0);
    vec2 uv = vec2(clamp((mu + 0.20) / 1.20, 0.0, 1.0), clamp(heightMeters / atmosphereHeight, 0.0, 1.0));
    vec3 sampled = texture(sampler2D(atmosphere_transmittance_lut, atmosphere_sampler), uv).rgb;
    float lum = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    return lum > 1.0e-5 ? sampled : vec3(1.0);
}

vec3 apply_analytical_aerial_perspective(vec3 radiance, vec3 origin, vec3 direction, float distanceMeters) {
    if (distanceMeters <= 0.0 || distanceMeters >= 9999.0) {
        return radiance;
    }
    vec3 dirNorm = normalize(direction);
    float cosZenith = clamp(dot(dirNorm, vec3(0.0, 1.0, 0.0)), -1.0, 1.0);
    vec3 planetary = atmosphere_scene_to_planetary(origin);
    float heightMeters = max(length(planetary) - ATMOSPHERE_PLANET_RADIUS, 0.0);
    float atmosphereHeight = max(ATMOSPHERE_TOP_RADIUS - ATMOSPHERE_PLANET_RADIUS, 1.0);
    float depthNormalized = log(1.0 + distanceMeters * 0.001) / log(1.0 + 100.0);
    vec3 uvw = vec3(cosZenith * 0.5 + 0.5, clamp(heightMeters / atmosphereHeight, 0.0, 1.0), clamp(depthNormalized, 0.0, 1.0));
    vec4 aerial = texture(sampler3D(atmosphere_aerial_perspective_lut, atmosphere_sampler), uvw);
    float lum = dot(aerial.rgb, vec3(0.2126, 0.7152, 0.0722));
    if (aerial.a <= 1.0e-5 && lum <= 1.0e-5) {
        return radiance;
    }
    return radiance * clamp(aerial.a, 0.0, 1.0) + max(aerial.rgb, vec3(0.0));
}

float atmosphere_horizon_visibility(vec3 scenePos, vec3 dir) {
    vec3 planetary = atmosphere_scene_to_planetary(scenePos);
    float radius = max(length(planetary), ATMOSPHERE_PLANET_RADIUS + 1.0);
    float horizonMu = -sqrt(max(1.0 - (ATMOSPHERE_PLANET_RADIUS * ATMOSPHERE_PLANET_RADIUS) / (radius * radius), 0.0));
    float viewMu = dot(normalize(dir), normalize(planetary));
    return smoothstep(horizonMu - 0.004, horizonMu + 0.004, viewMu);
}

vec3 atmosphere_sky_radiance(vec3 dir, uint quality) {
    vec3 viewDir = normalize(dir);
    if (quality == ATMOSPHERE_RAY_QUALITY_MINIMAL) {
        return vec3(0.0);
    }
    if (quality == ATMOSPHERE_RAY_QUALITY_FAST) {
        return fast_sky_radiance(viewDir) * atmosphere_horizon_visibility(camera.pos.xyz, viewDir);
    }
    float horizonVisibility = atmosphere_horizon_visibility(camera.pos.xyz, viewDir);
    if (horizonVisibility <= 1.0e-4) {
        return vec3(0.0);
    }
    vec2 uv = vec2(atan(viewDir.z, viewDir.x) / (2.0 * PI) + 0.5, asin(clamp(viewDir.y, -1.0, 1.0)) / PI + 0.5);
    vec3 sampled = texture(sampler2D(atmosphere_sky_view_lut, atmosphere_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
    float lum = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    if (lum > 1.0e-5) {
        return sampled * horizonVisibility;
    }
    return fast_sky_radiance(viewDir) * horizonVisibility;
}

vec3 environment_radiance_atmosphere(vec3 dir, uint quality) {
    if (env_params.enabled != 0u && env_params.procedural != 0u) {
        return atmosphere_sky_radiance(dir, quality) + analytical_sun_disk_radiance(dir);
    }
    return atmosphere_sky_radiance(dir, quality) + analytical_sun_disk_radiance(dir);
}

#endif
