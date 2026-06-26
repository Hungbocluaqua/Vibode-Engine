#ifndef RTV_PATHTRACE_OUTPUT_GLSL
#define RTV_PATHTRACE_OUTPUT_GLSL

// Sample stabilization and diagnostic output helpers.
vec3 clamp_firefly_sample_to_luminance(vec3 sampleColor, float maxLum) {
    sampleColor = max(sampleColor, vec3(0.0));
    float sampleLum = dot(sampleColor, vec3(0.2126, 0.7152, 0.0722));
    if (sampleLum <= 1.0e-5) {
        return sampleColor;
    }
    return sampleColor * min(1.0, maxLum / sampleLum);
}

float sanitize_effect_hit_distance(float distance, float fallbackDistance) {
    float fallback = clamp(max(fallbackDistance, 0.0), 0.0, 65504.0);
    return distance > 0.001 && distance < 65000.0
        ? clamp(distance, 0.0, 65504.0)
        : fallback;
}

float weighted_effect_hit_distance(float firstWeight, float firstDistance, float secondWeight, float secondDistance, float fallbackDistance) {
    float d0 = sanitize_effect_hit_distance(firstDistance, fallbackDistance);
    float d1 = sanitize_effect_hit_distance(secondDistance, fallbackDistance);
    float w0 = max(firstWeight, 0.0);
    float w1 = max(secondWeight, 0.0);
    float sumW = w0 + w1;
    if (sumW <= 1.0e-5) {
        return sanitize_effect_hit_distance(fallbackDistance, fallbackDistance);
    }
    return clamp((d0 * w0 + d1 * w1) / sumW, 0.0, 65504.0);
}

vec3 select_effect_ray_direction(float firstWeight, vec3 firstDirection, float secondWeight, vec3 secondDirection, vec3 fallbackDirection) {
    vec3 chosen = secondWeight > firstWeight ? secondDirection : firstDirection;
    if (dot(chosen, chosen) <= 1.0e-8) {
        chosen = fallbackDirection;
    }
    return normalize(chosen);
}

vec3 clamp_firefly_sample(vec3 sampleColor, vec3 previousAverage, uint frameCount) {
    sampleColor = max(sampleColor, vec3(0.0));
    float sampleLum = dot(sampleColor, vec3(0.2126, 0.7152, 0.0722));
    if (sampleLum <= 1.0e-5) {
        return sampleColor;
    }

    float previousLum = dot(max(previousAverage, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    float clampLimit = firefly_clamp_luminance();
    float warmupLimit = mix(clampLimit * 2.0, clampLimit, clamp(float(frameCount) / 16.0, 0.0, 1.0));
    float historyLimit = max(clampLimit, previousLum * 10.0 + 6.0);
    float maxLum = frameCount <= 1u ? warmupLimit : min(warmupLimit, historyLimit);
    return clamp_firefly_sample_to_luminance(sampleColor, maxLum);
}

vec3 unsupported_hardware_rt_debug_pattern(uvec2 pixel, uint view) {
    uint stripe = ((pixel.x / 16u) + (pixel.y / 16u) + view) & 1u;
    vec3 a = vec3(0.16, 0.04, 0.26);
    vec3 b = vec3(0.95, 0.45, 0.10);
    return mix(a, b, float(stripe));
}


#endif // RTV_PATHTRACE_OUTPUT_GLSL
