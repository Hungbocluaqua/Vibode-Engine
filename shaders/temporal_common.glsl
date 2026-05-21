vec2 temporal_unpack_unorm2x16(uint packedValue) {
    return vec2(float(packedValue & 0xffffu), float((packedValue >> 16u) & 0xffffu)) / 65535.0;
}

vec2 temporal_unpack_snorm2x16(uint packedValue) {
    return temporal_unpack_unorm2x16(packedValue) * 2.0 - vec2(1.0);
}

vec2 temporal_unpack_velocity_pixels(uint packedVelocity, float velocityScale) {
    return temporal_unpack_snorm2x16(packedVelocity) * velocityScale;
}

vec2 temporal_reproject_pixel(ivec2 coords, vec2 velocityPixels) {
    return vec2(coords) - velocityPixels;
}

bool temporal_history_pixel_valid(vec2 historyPos, ivec2 dims, float margin) {
    return historyPos.x >= margin &&
        historyPos.y >= margin &&
        historyPos.x < float(dims.x) - 1.0 - margin &&
        historyPos.y < float(dims.y) - 1.0 - margin;
}

float temporal_motion_confidence(vec2 velocityPixels, float fullRejectPixels, float minConfidence) {
    float motion = clamp(length(velocityPixels) / max(fullRejectPixels, 1.0e-4), 0.0, 1.0);
    return mix(1.0, minConfidence, motion);
}

float temporal_reactive_weight(float lumDelta, float clipDelta, float neighborhoodDelta) {
    return clamp(max(max(lumDelta - 0.12, clipDelta), neighborhoodDelta - 1.8) * 2.5, 0.0, 1.0);
}

float temporal_variance_confidence(float variance, float scale) {
    return exp(-max(variance, 0.0) * scale);
}

float temporal_disocclusion_confidence(
    bool onScreen,
    bool historyKindValid,
    float relativePositionDelta,
    float normalConeMin,
    float maxRelativeDepthDelta) {
    if (!onScreen || !historyKindValid) {
        return 0.0;
    }
    float posConfidence = exp(-max(relativePositionDelta, 0.0) * 18.0);
    float normalConfidence = smoothstep(0.35, 0.92, normalConeMin);
    float depthConfidence = exp(-max(maxRelativeDepthDelta, 0.0) * 6.0);
    return clamp(posConfidence * normalConfidence * depthConfidence, 0.0, 1.0);
}

float temporal_history_weight(
    float varianceConfidence,
    float disocclusionConfidence,
    float motionConfidence,
    float reactiveWeight,
    float frameBlend) {
    float reactiveRejection = 1.0 - reactiveWeight * mix(0.65, 0.95, varianceConfidence);
    float baseAlpha = mix(0.04, 0.88, varianceConfidence) *
        disocclusionConfidence *
        motionConfidence *
        reactiveRejection;
    return clamp(baseAlpha * frameBlend, 0.0, 0.98);
}

vec3 temporal_rgb_to_ycocg(vec3 c) {
    float y = dot(c, vec3(0.25, 0.5, 0.25));
    float co = c.r - c.b;
    float cg = c.g - 0.5 * (c.r + c.b);
    return vec3(y, co, cg);
}

vec3 temporal_ycocg_to_rgb(vec3 c) {
    float t = c.x - c.z * 0.5;
    float g = c.x + c.z * 0.5;
    float r = t + c.y * 0.5;
    float b = t - c.y * 0.5;
    return vec3(r, g, b);
}

vec3 temporal_clamp_history_ycocg(vec3 history, vec3 minColor, vec3 maxColor, float sigmaLum) {
    vec3 historyYcocg = temporal_rgb_to_ycocg(history);
    vec3 minYcocg = temporal_rgb_to_ycocg(minColor);
    vec3 maxYcocg = temporal_rgb_to_ycocg(maxColor);
    vec3 lo = min(minYcocg, maxYcocg);
    vec3 hi = max(minYcocg, maxYcocg);
    vec3 margin = vec3(max(0.01, sigmaLum * 0.25), 0.08 + sigmaLum * 0.20, 0.08 + sigmaLum * 0.20);
    return max(temporal_ycocg_to_rgb(clamp(historyYcocg, lo - margin, hi + margin)), vec3(0.0));
}
