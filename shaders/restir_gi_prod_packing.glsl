// Shared production/validation reservoir ABI for ReSTIR GI.
// Production: 6 vec4 slots (96 bytes). Validation: 8 vec4 slots (128 bytes).

#ifndef RTV_RESTIR_GI_PROD_PACKING_GLSL
#define RTV_RESTIR_GI_PROD_PACKING_GLSL

#ifndef RTV_RESTIR_GI_VALIDATION_FULL
#define RTV_RESTIR_GI_VALIDATION_FULL 0
#endif

const uint PROD_PATH_CLASS_INVALID = 0u;
const uint PROD_PATH_CLASS_DIFFUSE_REUSABLE = 1u;
const uint PROD_PATH_CLASS_GLOSSY_REUSABLE = 2u;
const uint PROD_PATH_CLASS_ENVIRONMENT_REUSABLE = 3u;
const uint PROD_PATH_CLASS_EMISSIVE_REUSABLE = 4u;
const uint PROD_PATH_CLASS_TRANSMISSIVE_CURRENT_ONLY = 5u;
const uint PROD_PATH_CLASS_DELTA_CURRENT_ONLY = 6u;
const uint PROD_PATH_CLASS_UNSUPPORTED_CURRENT_ONLY = 7u;

const uint PROD_FLAG_VALID = 1u << 0u;
const uint PROD_FLAG_VISIBLE = 1u << 1u;
const uint PROD_FLAG_FINALIZED = 1u << 2u;
const uint PROD_FLAG_ALPHA_TESTED = 1u << 3u;
const uint PROD_FLAG_ENVIRONMENT = 1u << 4u;
const uint PROD_FLAG_VISIBILITY_KNOWN = 1u << 5u;
const uint PROD_FLAG_RECONNECTED_VISIBILITY = 1u << 6u;
const uint PROD_FLAG_NON_FINITE = 1u << 7u;

struct ProdRestirGiReservoir {
    vec4 x2PositionDistance;       // xyz = x2, or environment direction; w = distance, -1 for environment
    vec4 x2NormalRoughness;        // xyz = x2 normal; w = x2 roughness
    vec4 suffixRadianceSourcePdf;  // rgb = receiver-independent suffix; w = source PDF in selected measure
    vec4 sourceDirectionBsdfPdf;   // xyz = source x1->x2 direction; w = source BSDF PDF in solid angle
    vec4 selectedIntegrandTarget;  // rgb = shifted integrand in selected measure; w = target at current receiver
    vec4 reservoirData;            // x = W, y = M, z = packed age/flags/class/confidence, w = version hash
#if RTV_RESTIR_GI_VALIDATION_FULL
    vec4 validationIds;            // uint bits: x2 material, instance, primitive, receiver material
    vec4 validationData;           // source receiver position or rejection/debug data
#endif
};

uint prod_pack_meta(uint age, uint flags, uint pathClass, float confidence) {
    uint conf = uint(round(clamp(confidence, 0.0, 1.0) * 255.0));
    return (age & 0xffu) | ((flags & 0xffu) << 8u) |
        ((pathClass & 0xffu) << 16u) | (conf << 24u);
}

uint prod_meta(ProdRestirGiReservoir r) { return floatBitsToUint(r.reservoirData.z); }
uint prod_unpack_age(ProdRestirGiReservoir r) { return prod_meta(r) & 0xffu; }
uint prod_unpack_flags(ProdRestirGiReservoir r) { return (prod_meta(r) >> 8u) & 0xffu; }
uint prod_unpack_path_class(ProdRestirGiReservoir r) { return (prod_meta(r) >> 16u) & 0xffu; }
float prod_unpack_confidence(ProdRestirGiReservoir r) { return float((prod_meta(r) >> 24u) & 0xffu) / 255.0; }
float prod_unpack_weight_sum(ProdRestirGiReservoir r) { return max(r.reservoirData.x, 0.0); }
float prod_unpack_sample_count(ProdRestirGiReservoir r) { return max(r.reservoirData.y, 1.0); }
float prod_unpack_target(ProdRestirGiReservoir r) { return max(r.selectedIntegrandTarget.w, 0.0); }
float prod_unpack_source_pdf(ProdRestirGiReservoir r) { return max(r.suffixRadianceSourcePdf.w, 0.0); }
uint prod_unpack_version_hash(ProdRestirGiReservoir r) { return floatBitsToUint(r.reservoirData.w); }

void prod_set_meta(inout ProdRestirGiReservoir r, uint age, uint flags, uint pathClass, float confidence) {
    r.reservoirData.z = uintBitsToFloat(prod_pack_meta(age, flags, pathClass, confidence));
}

bool prod_path_class_reusable(uint pathClass) {
    return pathClass >= PROD_PATH_CLASS_DIFFUSE_REUSABLE &&
        pathClass <= PROD_PATH_CLASS_EMISSIVE_REUSABLE;
}

bool prod_path_class_current_only(uint pathClass) {
    return pathClass >= PROD_PATH_CLASS_TRANSMISSIVE_CURRENT_ONLY;
}

bool prod_reservoir_valid(ProdRestirGiReservoir r) {
    return (prod_unpack_flags(r) & PROD_FLAG_VALID) != 0u &&
        prod_path_class_reusable(prod_unpack_path_class(r)) &&
        prod_unpack_weight_sum(r) > 0.0 &&
        prod_unpack_sample_count(r) >= 1.0 &&
        prod_unpack_target(r) > 0.0 &&
        prod_unpack_source_pdf(r) > 0.0;
}

bool prod_reservoir_reusable(ProdRestirGiReservoir r) {
    return prod_reservoir_valid(r) && prod_path_class_reusable(prod_unpack_path_class(r));
}

ProdRestirGiReservoir prod_empty_reservoir() {
    ProdRestirGiReservoir r;
    r.x2PositionDistance = vec4(0.0);
    r.x2NormalRoughness = vec4(0.0, 1.0, 0.0, 1.0);
    r.suffixRadianceSourcePdf = vec4(0.0);
    r.sourceDirectionBsdfPdf = vec4(0.0);
    r.selectedIntegrandTarget = vec4(0.0);
    r.reservoirData = vec4(0.0);
#if RTV_RESTIR_GI_VALIDATION_FULL
    r.validationIds = vec4(0.0);
    r.validationData = vec4(0.0);
#endif
    return r;
}

float prod_final_weight(ProdRestirGiReservoir r) {
    return prod_unpack_weight_sum(r) /
        max(prod_unpack_sample_count(r) * prod_unpack_target(r), 1.0e-10);
}

float prod_shifted_reservoir_weight(
    ProdRestirGiReservoir source,
    float sourceTarget,
    float shiftedTarget) {
    return shiftedTarget * prod_unpack_weight_sum(source) /
        max(sourceTarget, 1.0e-10);
}

void prod_finalize_mass(inout ProdRestirGiReservoir r, float weightSum, float sampleCount) {
    const float maxM = 255.0;
    float m = max(sampleCount, 1.0);
    float scale = m > maxM ? maxM / m : 1.0;
    r.reservoirData.x = max(weightSum * scale, 0.0);
    r.reservoirData.y = min(m, maxM);
}

#endif
