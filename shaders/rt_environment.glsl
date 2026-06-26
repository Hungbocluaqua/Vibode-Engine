#ifndef RTV_RT_ENVIRONMENT_GLSL
#define RTV_RT_ENVIRONMENT_GLSL

// Environment, atmosphere, sun, and sky sampling helpers.
vec3 rotate_y(vec3 v, float angle);
vec2 env_uv_from_dir(vec3 dir);

vec3 environment_color(vec3 direction) {
    if (debug_params.view == 27u) {
        return vec3(DEBUG_WHITE_ENV_RADIANCE);
    }
    if (env_params.enabled != 0u && env_params.width > 1u && env_params.height > 1u) {
        vec3 localDir = rotate_y(direction, env_params.rotation);
        vec2 uv = env_uv_from_dir(localDir);
        float scale = env_params.procedural != 0u ? camera.sky_intensity : env_params.intensity;
        return texture(sampler2D(env_map, env_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb *
            scale * env_params.background_intensity;
    }
    float t = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.70, 0.74, 0.80), vec3(0.56, 0.68, 0.92), t) *
        camera.sky_intensity * env_params.background_intensity;
}

vec3 rotate_y(vec3 v, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

vec2 env_uv_from_dir(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

vec3 env_dir_from_uv(vec2 uv) {
    float phi = (uv.x - 0.5) * 2.0 * PI;
    float lat = (uv.y - 0.5) * PI;
    float cosLat = cos(lat);
    return normalize(vec3(cosLat * cos(phi), sin(lat), cosLat * sin(phi)));
}

vec3 analytical_sun_direction() {
    return normalize(camera.sun_direction_illuminance.xyz);
}

float analytical_sun_visibility() {
    if (camera.sunlight_enabled == 0u) {
        return 0.0;
    }
    return analytical_sun_direction().y > 0.0 ? 1.0 : 0.0;
}

float analytical_sun_solid_angle() {
    float radius = clamp(camera.sun_color_angular_radius.w, 0.0001, 0.08);
    return max(2.0 * PI * (1.0 - cos(radius)), 1.0e-8);
}

float analytical_sun_pdf(vec3 dir) {
    if (analytical_sun_visibility() <= 0.0) {
        return 0.0;
    }
    vec3 sunDir = analytical_sun_direction();
    float radius = clamp(camera.sun_color_angular_radius.w, 0.0001, 0.08);
    return dot(normalize(dir), sunDir) >= cos(radius) ? 1.0 / analytical_sun_solid_angle() : 0.0;
}

float atmosphere_planet_horizon_visibility(vec3 scenePos, vec3 dir, float width) {
    vec3 planetary = atmosphere_scene_to_planetary(scenePos);
    float radius = max(length(planetary), ATMOSPHERE_PLANET_RADIUS + 1.0);
    float horizonMu = -sqrt(max(1.0 - (ATMOSPHERE_PLANET_RADIUS * ATMOSPHERE_PLANET_RADIUS) / (radius * radius), 0.0));
    float viewMu = dot(normalize(dir), normalize(planetary));
    return smoothstep(horizonMu - width, horizonMu + width, viewMu);
}

vec3 analytical_sun_center_radiance() {
    if (analytical_sun_visibility() <= 0.0) {
        return vec3(0.0);
    }
    return max(camera.sun_color_angular_radius.rgb, vec3(0.0)) *
        max(camera.sun_direction_illuminance.w, 0.0) / analytical_sun_solid_angle();
}

vec3 analytical_sun_disk_radiance(vec3 dir) {
    if (analytical_sun_visibility() <= 0.0) {
        return vec3(0.0);
    }
    vec3 sunDir = analytical_sun_direction();
    float rayHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, dir, 0.003);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.006);
    if (rayHorizon <= 1.0e-4 || sunHorizon <= 1.0e-4) {
        return vec3(0.0);
    }
    float radius = clamp(camera.sun_angular_radius, 0.00465, 0.08);
    float cosAngle = dot(normalize(dir), sunDir);
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

vec3 high_resolution_sun_disk_radiance(vec3 dir) {
    if (analytical_sun_visibility() <= 0.0) {
        return vec3(0.0);
    }
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    float rayHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, viewDir, 0.003);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.006);
    if (rayHorizon <= 1.0e-4 || sunHorizon <= 1.0e-4) {
        return vec3(0.0);
    }
    float sunVisibility = smoothstep(-0.08, 0.08, sunDir.y);
    return visible_sun_core(viewDir, sunDir, sunVisibility, sunHorizon, rayHorizon) * camera.sky_intensity;
}

vec3 environment_sun_disk_radiance(vec3 dir) {
    if (analytical_sun_visibility() <= 0.0) {
        return vec3(0.0);
    }
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    vec3 centerRadiance = analytical_sun_center_radiance();
    if (dot(centerRadiance, vec3(0.2126, 0.7152, 0.0722)) <= 1.0e-6) {
        return vec3(0.0);
    }
    float rayHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, viewDir, 0.003);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.006);
    if (rayHorizon <= 1.0e-4 || sunHorizon <= 1.0e-4) {
        return vec3(0.0);
    }

    float cosAngle = dot(viewDir, sunDir);
    float radius = clamp(camera.sun_angular_radius, 0.00465, 0.08);
    float cosRadius = cos(radius);
    float disk = smoothstep(cosRadius, mix(cosRadius, 1.0, 0.18), cosAngle);
    float limb = 0.62 + 0.38 * sqrt(clamp((cosAngle - cosRadius) / max(1.0 - cosRadius, 1.0e-5), 0.0, 1.0));
    float sunVisibility = smoothstep(-0.08, 0.08, sunDir.y);
    vec3 core = visible_sun_core(viewDir, sunDir, sunVisibility, sunHorizon, rayHorizon);
    return (centerRadiance * disk * limb + core) * rayHorizon * sunHorizon * camera.sky_intensity;
}

vec3 unreal_sky_grade(vec3 dir, vec3 physicalSky) {
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    float viewY = viewDir.y;
    float activeSun = analytical_sun_visibility();
    float sunUp = clamp(sunDir.y, -0.12, 1.0);
    float sunVisibility = activeSun * smoothstep(-0.08, 0.08, sunUp);
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
    float activeSun = analytical_sun_visibility();
    float viewUp = clamp(viewDir.y, -0.08, 1.0);
    float sunUp = clamp(sunDir.y, -0.08, 1.0);
    float cosTheta = clamp(dot(viewDir, sunDir), -1.0, 1.0);
    float sunVisibility = activeSun * smoothstep(-0.06, 0.08, sunUp);
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

vec3 analytical_sun_radiance(vec3 dir) {
    return analytical_sun_disk_radiance(dir);
}

vec2 atmosphere_latlong_uv(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

vec3 sample_sky_view_lut(vec3 dir) {
    vec2 uv = atmosphere_latlong_uv(dir);
    return texture(sampler2D(atmosphere_sky_view_lut, atmosphere_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
}

vec3 atmosphere_sky_radiance(vec3 dir, uint quality);

bool sky_cdf_available() {
    uint width = max(env_params.sky_cdf_width, 1u);
    uint height = max(env_params.sky_cdf_height, 1u);
    return env_params.procedural != 0u && sky_cdf_cols.length() >= width * height;
}

float sky_cdf_pixel_probability(uint idx) {
    float previous = idx > 0u ? sky_cdf_cols[idx - 1u] : 0.0;
    return max(sky_cdf_cols[idx] - previous, 0.0);
}

float sky_cdf_direction_pdf(vec3 dir) {
    vec2 uv = atmosphere_latlong_uv(dir);
    uint width = max(env_params.sky_cdf_width, 1u);
    uint height = max(env_params.sky_cdf_height, 1u);
    uint col = uint(clamp(fract(uv.x) * float(width), 0.0, float(width - 1u)));
    uint row = uint(clamp(clamp(uv.y, 0.0, 1.0) * float(height), 0.0, float(height - 1u)));
    uint idx = row * width + col;
    float lat = ((float(row) + 0.5) / float(height) - 0.5) * PI;
    float sinTheta = max(cos(lat), 0.001);
    return sky_cdf_pixel_probability(idx) * float(width * height) * SKY_CDF_HALF_RCP_PI_SQ / sinTheta;
}

vec3 sample_sky_cdf_direction(inout uint state, out vec3 out_dir, out float out_pdf) {
    float u = rand_f32(state);
    uint width = max(env_params.sky_cdf_width, 1u);
    uint height = max(env_params.sky_cdf_height, 1u);
    uint totalPixels = width * height;
    uint low = 0u;
    uint high = totalPixels - 1u;
    while (low < high) {
        uint mid = (low + high) / 2u;
        if (sky_cdf_cols[mid] < u) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    uint x = low % width;
    uint y = low / width;
    vec2 uv = (vec2(x, y) + vec2(rand_f32(state), rand_f32(state))) / vec2(width, height);
    out_dir = env_dir_from_uv(uv);
    out_pdf = sky_cdf_direction_pdf(out_dir);
    return atmosphere_sky_radiance(out_dir, ATMOSPHERE_RAY_QUALITY_FULL);
}

float atmosphere_horizon_visibility(vec3 scenePos, vec3 dir) {
    return atmosphere_planet_horizon_visibility(scenePos, dir, 0.004);
}

vec3 atmosphere_sky_radiance(vec3 dir, uint quality) {
    vec3 viewDir = normalize(dir);
    if (quality == ATMOSPHERE_RAY_QUALITY_MINIMAL) {
        return vec3(0.0);
    }
    if (camera.sunlight_enabled == 0u) {
        return fast_sky_radiance(viewDir);
    }
    if (quality == ATMOSPHERE_RAY_QUALITY_FAST) {
        return fast_sky_radiance(viewDir);
    }
    vec3 sampled = sample_sky_view_lut(viewDir);
    float sampledLuminance = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    if (sampledLuminance > 1.0e-5) {
        return sampled;
    }
    return fast_sky_radiance(viewDir);
}

vec3 sample_atmosphere_transmittance_lut(vec3 worldPos, vec3 dir) {
    vec3 planetary = atmosphere_scene_to_planetary(worldPos);
    float heightMeters = max(length(planetary) - ATMOSPHERE_PLANET_RADIUS, 0.0);
    float mu = dot(normalize(dir), normalize(planetary));
    float atmosphereHeight = max(ATMOSPHERE_TOP_RADIUS - ATMOSPHERE_PLANET_RADIUS, 1.0);
    vec2 uv = vec2(clamp((mu + 0.20) / 1.20, 0.0, 1.0), clamp(heightMeters / atmosphereHeight, 0.0, 1.0));
    vec3 sampled = texture(sampler2D(atmosphere_transmittance_lut, atmosphere_sampler), uv).rgb;
    float sampledLuminance = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    return sampledLuminance > 1.0e-5 ? sampled : vec3(1.0);
}

vec3 sample_multi_scatter_lut_debug(vec3 dir) {
    vec3 sunDir = analytical_sun_direction();
    float viewMu = clamp(normalize(dir).y, -0.20, 1.0);
    float sunMu = clamp(sunDir.y, -0.20, 1.0);
    vec2 uv = vec2(clamp((viewMu + 0.20) / 1.20, 0.0, 1.0), clamp((sunMu + 0.20) / 1.20, 0.0, 1.0));
    return texture(sampler2D(atmosphere_multi_scatter_lut, atmosphere_sampler), uv).rgb;
}

vec3 sun_transmittance(vec3 worldPos, vec3 sunDir) {
    return sample_atmosphere_transmittance_lut(worldPos, sunDir);
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
    float aerialLuminance = dot(aerial.rgb, vec3(0.2126, 0.7152, 0.0722));
    if (aerial.a <= 1.0e-5 && aerialLuminance <= 1.0e-5) {
        return radiance;
    }
    return radiance * clamp(aerial.a, 0.0, 1.0) + max(aerial.rgb, vec3(0.0));
}

vec3 environment_radiance(vec3 dir, uint quality) {
    if (debug_params.view == 27u) {
        return vec3(DEBUG_WHITE_ENV_RADIANCE);
    }
    if (env_params.enabled != 0u) {
        if (env_params.procedural != 0u) {
            return atmosphere_sky_radiance(dir, quality);
        }
        vec3 localDir = rotate_y(dir, env_params.rotation);
        vec2 uv = env_uv_from_dir(localDir);
        vec3 sampled = texture(sampler2D(env_map, env_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
        return sampled * env_params.intensity;
    }
    return vec3(0.0);
}

vec3 environment_background_radiance(vec3 dir, uint quality) {
    if (debug_params.view == 27u) {
        return vec3(DEBUG_WHITE_ENV_RADIANCE);
    }
    if (env_params.enabled != 0u && env_params.procedural == 0u) {
        vec3 localDir = rotate_y(dir, env_params.rotation);
        vec2 uv = env_uv_from_dir(localDir);
        vec3 sampled = texture(sampler2D(env_map, env_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
        return sampled * env_params.intensity;
    }
    return atmosphere_sky_radiance(dir, quality);
}

vec3 debug_display_tonemap(vec3 color) {
    color = max(color, vec3(0.0));
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

float environment_pdf(vec3 dir) {
    if (env_params.enabled == 0u) {
        return 0.0;
    }
    if (sky_cdf_available()) {
        return sky_cdf_direction_pdf(dir);
    }
    if (env_params.width == 0u || env_params.height == 0u || env_params.inv_total_lum <= 0.0) {
        if (env_params.procedural != 0u) {
            vec3 radiance = atmosphere_sky_radiance(dir, ATMOSPHERE_RAY_QUALITY_FULL);
            float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
            if (lum <= 1.0e-5) {
                return 1.0 / (4.0 * PI);
            }
            float lat = asin(clamp(normalize(dir).y, -1.0, 1.0));
            float sinTheta = max(cos(lat), 0.001);
            return lum / (2.0 * PI * PI * max(sinTheta, 0.001));
        }
        return 1.0 / (4.0 * PI);
    }
    vec3 localDir = rotate_y(dir, env_params.rotation);
    vec2 uv = env_uv_from_dir(localDir);
    uint col = uint(clamp(uv.x * float(env_params.width), 0.0, float(env_params.width - 1u)));
    uint row = uint(clamp(uv.y * float(env_params.height), 0.0, float(env_params.height - 1u)));
    vec3 sampleValue = texelFetch(sampler2D(env_map, env_sampler), ivec2(int(col), int(row)), 0).rgb;
    float lum = dot(sampleValue, vec3(0.2126, 0.7152, 0.0722));
    float lat = ((float(row) + 0.5) / float(env_params.height) - 0.5) * PI;
    float sinTheta = max(cos(lat), 0.001);
    return max(lum * float(env_params.width) * float(env_params.height) * env_params.inv_total_lum / (2.0 * PI * PI * sinTheta), 0.0);
}

vec3 sample_environment_direction(inout uint state, out vec3 out_dir, out float out_pdf) {
    out_pdf = 0.0;
    out_dir = vec3(0.0, 1.0, 0.0);
    if (env_params.enabled == 0u) {
        return vec3(0.0);
    }
    if (sky_cdf_available()) {
        return sample_sky_cdf_direction(state, out_dir, out_pdf);
    }
    if (env_params.width == 0u || env_params.height == 0u || env_params.inv_total_lum <= 0.0) {
        float z = 1.0 - 2.0 * rand_f32(state);
        float phi = 2.0 * PI * rand_f32(state);
        float r = sqrt(max(1.0 - z * z, 0.0));
        out_dir = vec3(r * cos(phi), z, r * sin(phi));
        out_pdf = 1.0 / (4.0 * PI);
        return atmosphere_sky_radiance(out_dir, ATMOSPHERE_RAY_QUALITY_FULL);
    }

    float rowSample = rand_f32(state) * float(env_params.height);
    uint rowCandidate = min(uint(rowSample), env_params.height - 1u);
    vec2 rowAlias = env_alias_rows[rowCandidate];
    uint row = fract(rowSample) <= rowAlias.x ? rowCandidate : min(uint(rowAlias.y + 0.5), env_params.height - 1u);

    float colSample = rand_f32(state) * float(env_params.width);
    uint colCandidate = min(uint(colSample), env_params.width - 1u);
    uint colOffset = row * env_params.width;
    vec2 colAlias = env_alias_cols[colOffset + colCandidate];
    uint col = fract(colSample) <= colAlias.x ? colCandidate : min(uint(colAlias.y + 0.5), env_params.width - 1u);
    vec2 uv = vec2((float(col) + 0.5) / float(env_params.width), (float(row) + 0.5) / float(env_params.height));
    out_dir = rotate_y(env_dir_from_uv(uv), -env_params.rotation);
    if (env_params.procedural != 0u) {
        vec3 radiance = atmosphere_sky_radiance(out_dir, ATMOSPHERE_RAY_QUALITY_FULL);
        out_pdf = environment_pdf(out_dir);
        return radiance;
    }
    vec3 radiance = texelFetch(sampler2D(env_map, env_sampler), ivec2(int(col), int(row)), 0).rgb * env_params.intensity;
    out_pdf = environment_pdf(out_dir);
    return radiance;
}


#endif // RTV_RT_ENVIRONMENT_GLSL
