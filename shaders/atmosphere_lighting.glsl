#ifndef RTV_ATMOSPHERE_LIGHTING_GLSL
#define RTV_ATMOSPHERE_LIGHTING_GLSL

#include "atmosphere_common.glsl"

vec3 analytical_sun_direction() {
    float elevation = clamp(camera.atmosphere.x, -0.20, 1.45);
    float sunAzimuth = camera.atmosphere.w;
    float cosElev = cos(elevation);
    return normalize(vec3(cosElev * sin(sunAzimuth), sin(elevation), cosElev * cos(sunAzimuth)));
}

float atmosphere_planet_horizon_visibility(vec3 scenePos, vec3 dir, float width) {
    vec3 planetary = atmosphere_scene_to_planetary(scenePos);
    float radius = max(length(planetary), ATMOSPHERE_PLANET_RADIUS + 1.0);
    float horizonMu = -sqrt(max(1.0 - (ATMOSPHERE_PLANET_RADIUS * ATMOSPHERE_PLANET_RADIUS) / (radius * radius), 0.0));
    float viewMu = dot(normalize(dir), normalize(planetary));
    return smoothstep(horizonMu - width, horizonMu + width, viewMu);
}

vec3 analytical_sun_center_radiance() {
    if (camera.sunlight_enabled != 0u) {
        float sunHeight = smoothstep(-0.08, 0.22, analytical_sun_direction().y);
        vec3 sunsetTint = vec3(1.0, 0.58, 0.30);
        vec3 noonTint = vec3(1.0, 0.96, 0.84);
        return mix(sunsetTint, noonTint, sunHeight) * camera.sun_intensity * 28.0;
    }
    return vec3(0.0);
}

vec3 analytical_sun_disk_radiance(vec3 dir) {
    if (camera.sunlight_enabled == 0u) {
        return vec3(0.0);
    }
    vec3 sunDir = analytical_sun_direction();
    float rayHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, dir, 0.003);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.006);
    if (rayHorizon <= 1.0e-4 || sunHorizon <= 1.0e-4) {
        return vec3(0.0);
    }
    float cosAngle = clamp(dot(normalize(dir), sunDir), -1.0, 1.0);
    float radius = clamp(camera.sun_angular_radius, 0.00465, 0.08);
    float cosRadius = cos(radius);
    float disk = smoothstep(cosRadius, mix(cosRadius, 1.0, 0.18), cosAngle);
    float limb = 0.62 + 0.38 * sqrt(clamp((cosAngle - cosRadius) / max(1.0 - cosRadius, 1.0e-5), 0.0, 1.0));
    return analytical_sun_center_radiance() * disk * limb * rayHorizon * sunHorizon;
}

float atmosphere_saturate(float v) {
    return clamp(v, 0.0, 1.0);
}

vec3 visible_sun_core(vec3 viewDir, vec3 sunDir, float sunVisibility, float sunHorizon, float horizonVisibility) {
    float cosTheta = clamp(dot(normalize(viewDir), normalize(sunDir)), -1.0, 1.0);
    float angle = acos(cosTheta);
    float core = 1.0 - smoothstep(0.010, 0.018, angle);
    float rim = 1.0 - smoothstep(0.018, 0.030, angle);
    float sunHeight = smoothstep(-0.08, 0.22, sunDir.y);
    vec3 lowTint = vec3(1.0, 0.56, 0.28);
    vec3 highTint = vec3(1.0, 0.93, 0.72);
    vec3 tint = mix(lowTint, highTint, sunHeight);
    return tint * (core * 18.0 + rim * 3.0) * sunVisibility * sunHorizon * horizonVisibility;
}

vec3 unreal_sky_grade(vec3 dir, vec3 physicalSky) {
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    float viewY = viewDir.y;
    float sunUp = clamp(sunDir.y, -0.12, 1.0);
    float sunVisibility = smoothstep(-0.08, 0.08, sunUp);
    float lowSun = 1.0 - smoothstep(0.18, 0.82, sunUp);
    float sunset = 1.0 - smoothstep(0.02, 0.34, sunUp);
    float horizonVisibility = atmosphere_planet_horizon_visibility(camera.pos.xyz, viewDir, 0.006);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.010);
    float horizon = pow(1.0 - smoothstep(-0.02, 0.62, viewY), 1.65);
    float cosTheta = clamp(dot(viewDir, sunDir), -1.0, 1.0);

    vec3 dayZenith = vec3(0.32, 0.50, 0.78);
    vec3 dayHorizon = vec3(0.74, 0.84, 0.96);
    vec3 lowZenith = vec3(0.43, 0.47, 0.63);
    vec3 sunsetZenith = vec3(0.36, 0.34, 0.50);
    vec3 lowHorizon = vec3(1.0, 0.72, 0.46);
    vec3 sunsetHorizon = vec3(1.0, 0.52, 0.30);
    vec3 zenithColor = mix(dayZenith, mix(lowZenith, sunsetZenith, sunset), lowSun);
    vec3 horizonColor = mix(dayHorizon, mix(lowHorizon, sunsetHorizon, sunset), lowSun);
    vec3 palette = mix(zenithColor, horizonColor, horizon);

    float physicalLum = dot(max(physicalSky, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    vec3 sky = palette * (0.55 + 0.28 * atmosphere_saturate(physicalLum)) + physicalSky * 0.13;

    float haloTight = pow(atmosphere_saturate(cosTheta), mix(42.0, 16.0, lowSun));
    float haloWide = pow(atmosphere_saturate(cosTheta), mix(10.0, 5.5, lowSun));
    vec3 haloColor = mix(vec3(1.0, 0.94, 0.78), vec3(1.0, 0.62, 0.34), lowSun);
    sky += haloColor * sunVisibility * sunHorizon * horizonVisibility * (haloTight * 0.26 + haloWide * 0.045);

    return max(sky * camera.sky_intensity * horizonVisibility, vec3(0.0));
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
    vec3 sky = (rayleigh + mie + sunsetScatter * 0.10) * transmittance * sunVisibility;

    vec3 night = vec3(0.004, 0.006, 0.012) * smoothstep(-0.25, -0.05, sunUp);
    return unreal_sky_grade(viewDir, sky * 5.5 * 0.70 + night);
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
    if (distanceMeters <= 0.0 || distanceMeters >= 100000.0) {
        return radiance;
    }
    vec3 dirNorm = normalize(direction);
    vec3 planetary = atmosphere_scene_to_planetary(origin);
    float cosZenith = clamp(dot(dirNorm, normalize(planetary)), -1.0, 1.0);
    float heightMeters = max(length(planetary) - ATMOSPHERE_PLANET_RADIUS, 0.0);
    float atmosphereHeight = max(ATMOSPHERE_TOP_RADIUS - ATMOSPHERE_PLANET_RADIUS, 1.0);
    float distanceNormalized = clamp((max(distanceMeters, 1.0) - 1.0) / (100000.0 - 1.0), 0.0, 1.0);
    float depthNormalized = pow(distanceNormalized, 1.0 / 3.0);
    vec3 uvw = vec3(cosZenith * 0.5 + 0.5, clamp(heightMeters / atmosphereHeight, 0.0, 1.0), clamp(depthNormalized, 0.0, 1.0));
    vec4 aerial = texture(sampler3D(atmosphere_aerial_perspective_lut, atmosphere_sampler), uvw);
    float lum = dot(aerial.rgb, vec3(0.2126, 0.7152, 0.0722));
    if (aerial.a <= 1.0e-5 && lum <= 1.0e-5) {
        return radiance;
    }
    return radiance * clamp(aerial.a, 0.0, 1.0) + max(aerial.rgb, vec3(0.0));
}

float atmosphere_horizon_visibility(vec3 scenePos, vec3 dir) {
    return atmosphere_planet_horizon_visibility(scenePos, dir, 0.004);
}

vec3 atmosphere_sky_radiance(vec3 dir, uint quality) {
    vec3 viewDir = normalize(dir);
    if (quality == ATMOSPHERE_RAY_QUALITY_MINIMAL) {
        return vec3(0.0);
    }
    if (quality == ATMOSPHERE_RAY_QUALITY_FAST) {
        return fast_sky_radiance(viewDir);
    }
    vec2 uv = vec2(atan(viewDir.z, viewDir.x) / (2.0 * PI) + 0.5, asin(clamp(viewDir.y, -1.0, 1.0)) / PI + 0.5);
    vec3 sampled = texture(sampler2D(atmosphere_sky_view_lut, atmosphere_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
    float lum = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    if (lum > 1.0e-5) {
        return sampled;
    }
    return fast_sky_radiance(viewDir);
}

vec3 environment_radiance_atmosphere(vec3 dir, uint quality) {
    if (env_params.enabled != 0u && env_params.procedural != 0u) {
        return atmosphere_sky_radiance(dir, quality);
    }
    return atmosphere_sky_radiance(dir, quality);
}

#endif
