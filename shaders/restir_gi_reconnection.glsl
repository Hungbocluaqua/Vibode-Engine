// Shared receiver evaluation, measure conversion, shift validation, and versioning.

#ifndef RTV_RESTIR_GI_RECONNECTION_GLSL
#define RTV_RESTIR_GI_RECONNECTION_GLSL

const float RESTIR_GI_PI = 3.14159265358979323846;

float restir_gi_luma(vec3 v) {
    return dot(max(v, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
}

bool restir_gi_finite(float v) { return !isnan(v) && !isinf(v); }
bool restir_gi_finite3(vec3 v) { return all(not(isnan(v))) && all(not(isinf(v))); }

uint restir_gi_hash_u32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uint restir_gi_receiver_version_hash(RestirGiReceiver receiver) {
    return restir_gi_hash_u32(receiver.motion.z ^ restir_gi_hash_u32(receiver.motion.w + 0x9e3779b9u));
}

bool restir_gi_receiver_valid_for_reuse(RestirGiReceiver receiver) {
    float depth = restir_gi_receiver_depth(receiver);
    return restir_gi_receiver_material_id(receiver) != 0xffffffffu &&
        depth > 0.0 && depth < 65504.0 && restir_gi_finite(depth) &&
        dot(receiver.normalRoughness.xyz, receiver.normalRoughness.xyz) > 1.0e-8;
}

vec3 restir_gi_fresnel_schlick(float cosTheta, vec3 f0) {
    float m = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return f0 + (vec3(1.0) - f0) * m;
}

float restir_gi_ggx_D(float nDotH, float alpha) {
    float a2 = alpha * alpha;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(RESTIR_GI_PI * d * d, 1.0e-8);
}

float restir_gi_smith_G1(float nDotV, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * nDotV / max(nDotV + sqrt(a2 + (1.0 - a2) * nDotV * nDotV), 1.0e-6);
}

vec3 restir_gi_receiver_factor(RestirGiReceiver receiver, vec3 cameraPosition, vec3 wi) {
    vec3 n = restir_gi_receiver_normal(receiver);
    vec3 wo = normalize(cameraPosition - restir_gi_receiver_position(receiver));
    float nDotL = max(dot(n, wi), 0.0);
    float nDotV = max(dot(n, wo), 0.0);
    if (nDotL <= 1.0e-5 || nDotV <= 1.0e-5) return vec3(0.0);

    vec3 albedo = max(restir_gi_receiver_albedo(receiver), vec3(0.0));
    float metallic = restir_gi_receiver_metallic(receiver);
    float roughness = max(restir_gi_receiver_roughness(receiver), 0.2);
    vec3 h = normalize(wi + wo);
    float nDotH = max(dot(n, h), 0.0);
    float vDotH = max(dot(wo, h), 0.0);
    float alpha = roughness * roughness;
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = restir_gi_fresnel_schlick(vDotH, f0);
    float D = restir_gi_ggx_D(nDotH, alpha);
    float G = restir_gi_smith_G1(nDotV, alpha) * restir_gi_smith_G1(nDotL, alpha);
    vec3 specular = F * (D * G / max(4.0 * nDotV * nDotL, 1.0e-6));
    vec3 diffuse = (vec3(1.0) - F) * (1.0 - metallic) * albedo / RESTIR_GI_PI;
    diffuse *= restir_gi_receiver_occlusion(receiver);
    return max((diffuse + specular) * nDotL, vec3(0.0));
}

bool restir_gi_reconnect_sample(
    inout ProdRestirGiReservoir reservoir,
    RestirGiReceiver receiver,
    vec3 cameraPosition,
    out vec3 wi,
    out float distance)
{
    wi = vec3(0.0, 1.0, 0.0);
    distance = 65504.0;
    if (!restir_gi_receiver_valid_for_reuse(receiver) ||
        prod_unpack_version_hash(reservoir) != restir_gi_receiver_version_hash(receiver)) {
        return false;
    }

    uint pathClass = prod_unpack_path_class(reservoir);
    bool environment = pathClass == PROD_PATH_CLASS_ENVIRONMENT_REUSABLE ||
        (prod_unpack_flags(reservoir) & PROD_FLAG_ENVIRONMENT) != 0u;
    float measureFactor = 1.0;
    if (environment) {
        wi = normalize(reservoir.x2PositionDistance.xyz);
    } else {
        vec3 delta = reservoir.x2PositionDistance.xyz - restir_gi_receiver_position(receiver);
        float dist2 = dot(delta, delta);
        if (dist2 <= 1.0e-8 || dist2 >= 65504.0 * 65504.0) return false;
        distance = sqrt(dist2);
        wi = delta / distance;
        vec3 x2Normal = normalize(reservoir.x2NormalRoughness.xyz);
        float x2Cos = max(dot(x2Normal, -wi), 0.0);
        if (x2Cos <= 1.0e-5) return false;
        measureFactor = x2Cos / dist2;

        float x2Roughness = clamp(reservoir.x2NormalRoughness.w, 0.0, 1.0);
        if (x2Roughness < 0.65) {
            float directionAgreement = dot(normalize(reservoir.sourceDirectionBsdfPdf.xyz), wi);
            float minimumAgreement = mix(0.9995, 0.90, smoothstep(0.25, 0.65, x2Roughness));
            if (directionAgreement < minimumAgreement) return false;
        }
    }

    vec3 receiverFactor = restir_gi_receiver_factor(receiver, cameraPosition, wi);
    vec3 integrand = max(reservoir.suffixRadianceSourcePdf.rgb, vec3(0.0)) * receiverFactor * measureFactor;
    float target = restir_gi_luma(integrand);
    if (!restir_gi_finite3(integrand) || !restir_gi_finite(target) || target <= 1.0e-10) return false;
    reservoir.selectedIntegrandTarget = vec4(integrand, target);
    return true;
}

float restir_gi_source_measure_pdf(
    float sourceBsdfPdf,
    vec3 sourceDirection,
    vec3 x2Normal,
    float distance,
    bool environment)
{
    if (environment) return max(sourceBsdfPdf, 1.0e-10);
    float x2Cos = max(dot(normalize(x2Normal), -normalize(sourceDirection)), 0.0);
    return max(sourceBsdfPdf * x2Cos / max(distance * distance, 1.0e-8), 1.0e-10);
}

#endif
