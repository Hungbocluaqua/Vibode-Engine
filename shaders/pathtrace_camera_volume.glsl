#ifndef RTV_PATHTRACE_CAMERA_VOLUME_GLSL
#define RTV_PATHTRACE_CAMERA_VOLUME_GLSL

// Requires rt_common.glsl plus PathComponents and Ray declarations from pathtrace.rgen.
bool homogeneous_volume_enabled() {
#if RTV_NATIVE2B_PIPELINE
    return false;
#else
    return camera.volume_controls.x > 0.5 &&
        (max(camera.volume_controls.y, 0.0) + max(camera.volume_controls.z, 0.0)) > 1.0e-7;
#endif
}

float homogeneous_sigma_s() { return max(camera.volume_controls.y, 0.0); }
float homogeneous_sigma_a() { return max(camera.volume_controls.z, 0.0); }
float homogeneous_sigma_t() { return homogeneous_sigma_s() + homogeneous_sigma_a(); }
float homogeneous_anisotropy() { return clamp(camera.volume_controls.w, -0.95, 0.95); }

float homogeneous_transmittance_scalar(float distance) {
    if (!homogeneous_volume_enabled()) {
        return 1.0;
    }
    return exp(-homogeneous_sigma_t() * max(distance, 0.0));
}

float sample_homogeneous_medium_distance(ivec2 coords, uint bounce) {
    if (!homogeneous_volume_enabled()) {
        return 1.0e30;
    }
    float u = sample_dimension_1d(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_VOLUME_DISTANCE);
    return -log(max(1.0 - u, 1.0e-6)) / max(homogeneous_sigma_t(), 1.0e-7);
}

float henyey_greenstein_phase(float cosTheta, float g) {
    float gg = g * g;
    float denom = max(1.0 + gg - 2.0 * g * cosTheta, 1.0e-4);
    return (1.0 - gg) / (4.0 * PI * denom * sqrt(denom));
}

vec3 sample_henyey_greenstein(vec3 incidentDir, ivec2 coords, uint bounce, out float pdf) {
    vec2 u = sample_dimension_2d(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_VOLUME_PHASE);
    float g = homogeneous_anisotropy();
    float cosTheta;
    if (abs(g) < 1.0e-3) {
        cosTheta = 1.0 - 2.0 * u.x;
    } else {
        float s = (1.0 - g * g) / max(1.0 - g + 2.0 * g * u.x, 1.0e-4);
        cosTheta = clamp((1.0 + g * g - s * s) / (2.0 * g), -1.0, 1.0);
    }
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float phi = 2.0 * PI * u.y;
    vec3 tangent;
    vec3 bitangent;
    tangent_frame(normalize(incidentDir), tangent, bitangent);
    vec3 wi = normalize(tangent * cos(phi) * sinTheta + bitangent * sin(phi) * sinTheta + normalize(incidentDir) * cosTheta);
    pdf = henyey_greenstein_phase(dot(normalize(incidentDir), wi), g);
    return wi;
}

float sample_camera_time(ivec2 sampleCoords, uint sampleIndex) {
    if (camera.motion_blur_controls.x <= 0.5) {
        return 0.0;
    }
    float shutterOpen = clamp(camera.motion_blur_controls.y, 0.0, 1.0);
    float shutterClose = clamp(camera.motion_blur_controls.z, shutterOpen, 1.0);
    float u = sample_dimension_1d(sampleCoords, camera.temporal_frame_index, sampleIndex, SAMPLE_DIM_CAMERA_TIME);
    return mix(shutterOpen, shutterClose, u);
}

vec2 concentric_disk_sample(vec2 u) {
    vec2 offset = u * 2.0 - vec2(1.0);
    if (abs(offset.x) < 1.0e-7 && abs(offset.y) < 1.0e-7) {
        return vec2(0.0);
    }

    float r;
    float theta;
    if (abs(offset.x) > abs(offset.y)) {
        r = offset.x;
        theta = (PI * 0.25) * (offset.y / offset.x);
    } else {
        r = offset.y;
        theta = (PI * 0.5) - (PI * 0.25) * (offset.x / offset.y);
    }
    return r * vec2(cos(theta), sin(theta));
}

float polygon_aperture_radius(vec2 dir, float bladeCount, float rotation) {
    if (bladeCount < 3.0 || dot(dir, dir) <= 1.0e-8) {
        return 1.0;
    }
    float blades = clamp(floor(bladeCount + 0.5), 3.0, 16.0);
    float sector = 2.0 * PI / blades;
    float local = mod(atan(dir.y, dir.x) - rotation + sector * 0.5, sector) - sector * 0.5;
    return cos(PI / blades) / max(cos(local), 1.0e-4);
}

Ray make_camera_ray(uvec2 pixel, uvec2 size, ivec2 sampleCoords, uint sampleIndex) {
    vec2 jitter = camera.path_tracing_enabled != 0u ? vec2(0.5) + camera.jitter.xy : vec2(0.5);
    vec2 uv = (vec2(pixel) + jitter) / vec2(size);
    vec2 ndc = uv * 2.0 - vec2(1.0);
    float viewportAspect = float(size.x) / max(float(size.y), 1.0);
    float authoredAspect = camera.projection_controls.y > 0.0 ? camera.projection_controls.y : viewportAspect;
    bool orthographic = camera.projection_controls.x >= 0.5;
    vec3 pinholeOrigin = camera.pos.xyz;
    vec3 pinholeDir = normalize(camera.forward.xyz);
    if (orthographic) {
        float xmag = max(camera.projection_controls.z, 1.0e-4);
        float ymag = max(camera.projection_controls.w, 1.0e-4);
        pinholeOrigin += camera.right.xyz * ndc.x * xmag * 0.5 - camera.up.xyz * ndc.y * ymag * 0.5;
    } else {
        float scale = tan(camera.fov_y * 0.5);
        pinholeDir = normalize(camera.forward.xyz + camera.right.xyz * ndc.x * authoredAspect * scale - camera.up.xyz * ndc.y * scale);
    }

    Ray ray;
    ray.origin = pinholeOrigin;
    ray.direction = pinholeDir;
    ray.time = sample_camera_time(sampleCoords, sampleIndex);

    float apertureRadius = max(camera.dof_controls.x, 0.0);
    if (apertureRadius <= 1.0e-6) {
        return ray;
    }

    vec2 lens = concentric_disk_sample(sample_dimension_2d(sampleCoords, camera.temporal_frame_index, sampleIndex, SAMPLE_DIM_CAMERA_LENS));
    lens *= polygon_aperture_radius(lens, camera.dof_controls.z, camera.dof_controls.w) * apertureRadius;
    float forwardProjection = max(dot(pinholeDir, normalize(camera.forward.xyz)), 1.0e-4);
    vec3 focusPoint = pinholeOrigin + pinholeDir * (max(camera.dof_controls.y, 0.01) / forwardProjection);
    vec3 lensOrigin = pinholeOrigin + camera.right.xyz * lens.x + camera.up.xyz * lens.y;
    ray.origin = lensOrigin;
    ray.direction = normalize(focusPoint - lensOrigin);
    return ray;
}

vec2 restir_unpack_unorm2x16(uint packedValue) {
    return vec2(float(packedValue & 0xffffu), float((packedValue >> 16u) & 0xffffu)) / 65535.0;
}

vec2 restir_unpack_velocity_pixels(uint packedVelocity) {
    int x = int(packedVelocity & 0xffffu);
    int y = int((packedVelocity >> 16u) & 0xffffu);
    if (x >= 32768) {
        x -= 65536;
    }
    if (y >= 32768) {
        y -= 65536;
    }
    return vec2(float(x), float(y)) * (SCREEN_VELOCITY_PACK_SCALE / 32767.0);
}

uint restir_mode() {
    return uint(clamp(camera.atmosphere.y, 0.0, 2.0));
}

bool restir_gi_enabled() {
#if RTV_NATIVE2B_PIPELINE
    return false;
#else
    return (camera.restir_gi_controls.y & 2u) != 0u;
#endif
}

bool restir_gi_legacy_cache_mode() {
    return (camera.restir_gi_controls.y & 8u) != 0u;
}

bool restir_gi_debug_view() {
#if RTV_NATIVE2B_PIPELINE
    return false;
#else
    uint debugView = renderer_debug_view();
    return (debugView >= 68u && debugView <= 73u) ||
        debugView == 90u ||
        debugView == 91u ||
        debugView == 127u ||
        debugView == 128u;
#endif
}

bool native2b_kernel_enabled() {
#if RTV_NATIVE2B_PIPELINE
    return true;
#else
    return false;
#endif
}

uint pathtrace_sbt_stride() {
    return native2b_kernel_enabled() ? 3u : 2u;
}

uint direct_light_ris_candidate_count(uint bounce) {
    if (camera.restir_di_controls.x != 0u && bounce == 0u) {
        // The finite-light RIS loop owns local-light candidates; environment
        // and BRDF candidates are evaluated by their dedicated techniques.
        return max(restir_di_raygen_params.rtxdiDiLocalLightSamples, 1u);
    }
    uint count = restir_mode() != 0u ? 4u : 1u;
    if (mesh_params.light_count > 65536u) {
        count = max(count, 12u);
    } else if (mesh_params.light_count > 16384u) {
        count = max(count, 8u);
    } else if (mesh_params.light_count > 4096u) {
        count = max(count, 6u);
    }
    return count;
}

bool temporal_history_available() {
    return camera.atmosphere.z > 0.5;
}

vec3 clamp_luminance_preserve_hue(vec3 value, float maxLum) {
    value = max(value, vec3(0.0));
    float valueLum = luminance(value);
    if (valueLum <= maxLum || valueLum <= 1.0e-6) {
        return value;
    }
    return value * (maxLum / valueLum);
}

float direct_contribution_luminance_limit(uint bounce, uint sampleType, float roughness) {
    float base = firefly_clamp_luminance();
    float bounceScale = bounce == 0u ? 4.0 : 1.75;
    float sunScale = sampleType == 3u ? 2.0 : 1.0;
    float roughnessScale = mix(0.75, 1.35, clamp(roughness, 0.0, 1.0));
    return max(base, base * bounceScale * sunScale * roughnessScale);
}

float restir_temporal_luminance_limit(vec3 currentDirect) {
    float base = firefly_clamp_luminance();
    float currentLum = luminance(max(currentDirect, vec3(0.0)));
    return max(base * 2.0, currentLum * 4.0);
}

float throughput_luminance_limit(uint nextBounce, float roughness, bool deltaPath) {
    float base = max(firefly_clamp_luminance(), 8.0);
    float depthScale = nextBounce <= 1u ? 2.0 : (nextBounce == 2u ? 1.25 : 0.85);
    float materialScale = deltaPath ? 2.0 : mix(0.75, 1.25, clamp(roughness, 0.0, 1.0));
    return max(4.0, base * depthScale * materialScale);
}

vec3 clamp_path_throughput(vec3 value, uint nextBounce, float roughness, bool deltaPath) {
    return clamp_luminance_preserve_hue(
        value,
        throughput_luminance_limit(nextBounce, roughness, deltaPath));
}


#endif // RTV_PATHTRACE_CAMERA_VOLUME_GLSL
