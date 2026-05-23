#ifndef RTV_RESTIR_COMMON_GLSL
#define RTV_RESTIR_COMMON_GLSL

struct RestirReservoir {
    uvec4 metadata;
    vec4 sample_value_confidence;
    vec4 target_pdf_weight_sum_m;
};

RestirReservoir empty_restir_reservoir() {
    RestirReservoir reservoir;
    reservoir.metadata = uvec4(0u);
    reservoir.sample_value_confidence = vec4(0.0);
    reservoir.target_pdf_weight_sum_m = vec4(0.0);
    return reservoir;
}

bool restir_reservoir_valid(RestirReservoir reservoir) {
    return reservoir.metadata.z != 0u &&
        reservoir.target_pdf_weight_sum_m.z > 0.0 &&
        reservoir.sample_value_confidence.a > 0.0;
}

#endif
