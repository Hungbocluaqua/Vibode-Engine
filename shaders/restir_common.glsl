#ifndef RTV_RESTIR_COMMON_GLSL
#define RTV_RESTIR_COMMON_GLSL

struct RestirReservoir {
    uvec4 metadata;
    vec4 sample_value_confidence;
    vec4 target_pdf_weight_sum_m;
};

const uint RESTIR_VISIBILITY_UNKNOWN = 0u;
const uint RESTIR_VISIBILITY_VISIBLE = 1u;
const uint RESTIR_VISIBILITY_INVALID = 2u;

uint restir_pack_validity_visibility(bool valid, uint visibility) {
    return valid ? (1u | ((visibility & 3u) << 1u)) : 0u;
}

bool restir_validity_bit(uint value) {
    return (value & 1u) != 0u;
}

uint restir_visibility_state(RestirReservoir reservoir) {
    return (reservoir.metadata.z >> 1u) & 3u;
}

RestirReservoir empty_restir_reservoir() {
    RestirReservoir reservoir;
    reservoir.metadata = uvec4(0u);
    reservoir.sample_value_confidence = vec4(0.0);
    reservoir.target_pdf_weight_sum_m = vec4(0.0);
    return reservoir;
}

bool restir_reservoir_valid(RestirReservoir reservoir) {
    return restir_validity_bit(reservoir.metadata.z) &&
        reservoir.target_pdf_weight_sum_m.z > 0.0 &&
        reservoir.sample_value_confidence.a > 0.0;
}

float restir_luminance(vec3 value) {
    return dot(value, vec3(0.2126, 0.7152, 0.0722));
}

float restir_target_function(RestirReservoir reservoir) {
    return max(max(reservoir.target_pdf_weight_sum_m.y, restir_luminance(reservoir.sample_value_confidence.rgb)), 0.0);
}

float restir_source_pdf(RestirReservoir reservoir) {
    return max(reservoir.target_pdf_weight_sum_m.x, 1.0e-6);
}

float restir_sample_count(RestirReservoir reservoir) {
    return max(reservoir.target_pdf_weight_sum_m.z, 1.0);
}

float restir_age_confidence(RestirReservoir reservoir, float maxAge) {
    return 1.0 - clamp(float(reservoir.metadata.y) / max(maxAge, 1.0), 0.0, 1.0);
}

float restir_pairwise_compatibility(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    if (!restir_reservoir_valid(current) || !restir_reservoir_valid(previous) || previous.metadata.y >= uint(maxAge)) {
        return 0.0;
    }

    if (current.metadata.x != previous.metadata.x) {
        return 0.0;
    }

    if (current.metadata.w != 0u && previous.metadata.w != 0u && current.metadata.w != previous.metadata.w) {
        return 0.0;
    }

    uint previousVisibility = restir_visibility_state(previous);
    if (previousVisibility == RESTIR_VISIBILITY_INVALID) {
        return 0.0;
    }

    float currentPdf = restir_source_pdf(current);
    float previousPdf = restir_source_pdf(previous);
    float pdfRatio = min(currentPdf, previousPdf) / max(currentPdf, previousPdf);

    float currentTarget = max(restir_target_function(current), 1.0e-5);
    float previousTarget = max(restir_target_function(previous), 1.0e-5);
    float targetRatio = min(currentTarget, previousTarget) / max(currentTarget, previousTarget);

    return clamp(motionConfidence, 0.0, 1.0) *
        restir_age_confidence(previous, maxAge) *
        sqrt(clamp(pdfRatio, 0.0, 1.0)) *
        sqrt(clamp(targetRatio, 0.0, 1.0));
}

float restir_pairwise_previous_weight(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    float compatibility = restir_pairwise_compatibility(current, previous, motionConfidence, maxAge);
    if (compatibility <= 0.0) {
        return 0.0;
    }

    float currentMass = max(restir_target_function(current), 0.0) / restir_source_pdf(current);
    float previousMass = max(restir_target_function(previous), 0.0) /
        restir_source_pdf(previous) *
        min(restir_sample_count(previous), 32.0) *
        clamp(previous.sample_value_confidence.a, 0.0, 1.0) *
        compatibility;
    float combined = currentMass + previousMass;
    if (combined <= 1.0e-8) {
        return 0.0;
    }

    float motionCap = mix(0.85, 0.10, 1.0 - clamp(motionConfidence, 0.0, 1.0));
    return clamp(previousMass / combined, 0.0, motionCap);
}

RestirReservoir restir_pairwise_temporal_merge(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    float previousWeight = restir_pairwise_previous_weight(current, previous, motionConfidence, maxAge);
    float currentWeight = 1.0 - previousWeight;

    current.metadata.y = previousWeight > 0.0 ? min(previous.metadata.y + 1u, 255u) : 0u;
    current.metadata.z = restir_pack_validity_visibility(
        restir_reservoir_valid(current),
        previousWeight > 0.0 && restir_visibility_state(previous) == RESTIR_VISIBILITY_VISIBLE
            ? RESTIR_VISIBILITY_VISIBLE
            : RESTIR_VISIBILITY_UNKNOWN);
    current.sample_value_confidence.rgb =
        current.sample_value_confidence.rgb * currentWeight +
        previous.sample_value_confidence.rgb * previousWeight;
    current.sample_value_confidence.a = clamp(
        (current.sample_value_confidence.a * currentWeight + previous.sample_value_confidence.a * previousWeight) *
            clamp(motionConfidence, 0.0, 1.0),
        0.0,
        1.0);
    current.target_pdf_weight_sum_m.x =
        current.target_pdf_weight_sum_m.x * currentWeight +
        previous.target_pdf_weight_sum_m.x * previousWeight;
    current.target_pdf_weight_sum_m.y = restir_luminance(current.sample_value_confidence.rgb);
    current.target_pdf_weight_sum_m.z = min(
        restir_sample_count(current) + restir_sample_count(previous) * previousWeight,
        64.0);
    current.target_pdf_weight_sum_m.w = previousWeight;
    return current;
}

#endif
