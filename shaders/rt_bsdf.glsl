#ifndef RTV_RT_BSDF_GLSL
#define RTV_RT_BSDF_GLSL

// BSDF evaluation, PDF, and sampling helpers.
float reflectance(float cosine, float ref_idx) {
    float r0 = (1.0 - ref_idx) / (1.0 + ref_idx);
    r0 = r0 * r0;
    float t = clamp(1.0 - cosine, 0.0, 1.0);
    float t2 = t * t;
    return r0 + (1.0 - r0) * (t2 * t2 * t);
}

vec3 material_dispersion_ior(Material material) {
    float baseIor = max(material.ior, 1.01);
    float halfSpread = (baseIor - 1.0) * 0.025 * max(material.dispersion_factor, 0.0);
    return max(vec3(baseIor - halfSpread, baseIor, baseIor + halfSpread), vec3(1.01));
}

uint material_dispersion_channel(Material material, float sampleValue) {
    if (material.dispersion_factor <= 1.0e-6) {
        return 1u;
    }
    return min(uint(floor(clamp(sampleValue, 0.0, 0.999999) * 3.0)), 2u);
}

float material_channel_value(vec3 value, uint channel) {
    return channel == 0u ? value.r : (channel == 1u ? value.g : value.b);
}

vec3 material_dispersion_channel_weight(Material material, uint channel) {
    if (material.dispersion_factor <= 1.0e-6) {
        return vec3(1.0);
    }
    return channel == 0u ? vec3(3.0, 0.0, 0.0) : (channel == 1u ? vec3(0.0, 3.0, 0.0) : vec3(0.0, 0.0, 3.0));
}

float schlick_fresnel_pow5(float t) {
    float t2 = t * t;
    return t2 * t2 * t;
}

float diffuse_pdf(vec3 normal, vec3 wi) {
    return max(dot(normal, wi), 0.0) / PI;
}

bool use_lambert_diffuse(Material material) {
    return material.roughness <= 0.08 || camera.path_tracing_enabled == 0u;
}

float oren_nayar_diffuse_factor(Material material, vec3 wo, vec3 wi, vec3 n) {
    if (use_lambert_diffuse(material)) {
        return 1.0;
    }

    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v <= 0.0 || n_dot_l <= 0.0) {
        return 0.0;
    }

    float sigma = clamp(material.roughness, 0.0, 1.0) * 1.2217304764; // 70 degrees in radians.
    float sigma2 = sigma * sigma;
    float a = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    float b = 0.45 * sigma2 / (sigma2 + 0.09);

    vec3 viewTangent = wo - n * n_dot_v;
    vec3 lightTangent = wi - n * n_dot_l;
    float viewLen2 = dot(viewTangent, viewTangent);
    float lightLen2 = dot(lightTangent, lightTangent);
    float cosPhiDiff = viewLen2 > 1.0e-8 && lightLen2 > 1.0e-8
        ? dot(viewTangent, lightTangent) * inversesqrt(viewLen2 * lightLen2)
        : 0.0;
    float sinAlpha = sqrt(max(1.0 - max(n_dot_v, n_dot_l) * max(n_dot_v, n_dot_l), 0.0));
    float tanBeta = sqrt(max(1.0 - min(n_dot_v, n_dot_l) * min(n_dot_v, n_dot_l), 0.0)) /
        max(min(n_dot_v, n_dot_l), 1.0e-4);

    return clamp(a + b * max(0.0, cosPhiDiff) * sinAlpha * tanBeta, 0.0, 1.0);
}

vec3 eval_diffuse_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    return material.color * (1.0 / PI) * oren_nayar_diffuse_factor(material, wo, wi, n);
}

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

bool has_positive_radiance(vec3 color) {
    return any(greaterThan(color, vec3(0.0)));
}

vec3 pbr_f0(Material material) {
    if (material.use_conductor_optics != 0u) {
        vec3 eta = max(material.conductor_eta, vec3(0.0));
        vec3 k = max(material.conductor_k, vec3(0.0));
        vec3 etaMinusOne = eta - vec3(1.0);
        vec3 etaPlusOne = eta + vec3(1.0);
        vec3 k2 = k * k;
        return clamp((etaMinusOne * etaMinusOne + k2) / max(etaPlusOne * etaPlusOne + k2, vec3(1.0e-6)), vec3(0.0), vec3(1.0));
    }
    uint flags = uint(round(material.pad2));
    vec3 dielectricF0 = (flags & MATERIAL_FLAG_SPECULAR_GLOSSINESS_WORKFLOW) != 0u
        ? clamp(material.specular_factor * material.specular_color, vec3(0.0), vec3(1.0))
        : clamp(vec3(0.04) * material.specular_factor * material.specular_color, vec3(0.0), vec3(1.0));
    return mix(dielectricF0, material.color, clamp(material.metallic, 0.0, 1.0));
}

vec3 conductor_fresnel(Material material, float v_dot_h) {
    float cosTheta = clamp(v_dot_h, 0.0, 1.0);
    vec3 eta = max(material.conductor_eta, vec3(0.0));
    vec3 k = max(material.conductor_k, vec3(0.0));
    vec3 eta2 = eta * eta;
    vec3 k2 = k * k;
    float cos2 = cosTheta * cosTheta;
    float sin2 = max(1.0 - cos2, 0.0);
    vec3 t0 = eta2 - k2 - vec3(sin2);
    vec3 a2plusb2 = sqrt(max(t0 * t0 + 4.0 * eta2 * k2, vec3(0.0)));
    vec3 t1 = a2plusb2 + vec3(cos2);
    vec3 a = sqrt(max((a2plusb2 + t0) * 0.5, vec3(0.0)));
    vec3 t2 = 2.0 * cosTheta * a;
    vec3 rs = (t1 - t2) / max(t1 + t2, vec3(1.0e-6));
    vec3 t3 = cos2 * a2plusb2 + vec3(sin2 * sin2);
    vec3 t4 = t2 * sin2;
    vec3 rp = rs * (t3 - t4) / max(t3 + t4, vec3(1.0e-6));
    return clamp((rp + rs) * 0.5, vec3(0.0), vec3(1.0));
}

vec3 pbr_diffuse_reflectance(Material material) {
    return clamp(material.color * (1.0 - clamp(material.metallic, 0.0, 1.0)), vec3(0.0), vec3(1.0));
}

vec3 pbr_specular_reflectance(Material material, float n_dot_v) {
    float cosTheta = clamp(n_dot_v, 0.0, 1.0);
    if (material.use_conductor_optics != 0u) {
        return conductor_fresnel(material, cosTheta);
    }
    vec3 f0 = pbr_f0(material);
    return clamp(f0 + (vec3(1.0) - f0) * schlick_fresnel_pow5(1.0 - cosTheta), vec3(0.0), vec3(1.0));
}

vec3 pbr_average_fresnel(vec3 f0) {
    return f0 + (vec3(1.0) - f0) * (1.0 / 21.0);
}

vec3 pbr_diffuse_energy(Material material) {
    vec3 f0 = pbr_f0(material);
    return clamp(vec3(1.0) - pbr_average_fresnel(f0), vec3(0.0), vec3(1.0)) *
        (1.0 - clamp(material.metallic, 0.0, 1.0)) *
        (1.0 - material_effective_transmission(material));
}

float material_sheen_weight(Material material, float NdotV) {
    float sheenLum = luminance(max(material.sheen_color, vec3(0.0)));
    if (sheenLum <= 1.0e-5) {
        return 0.0;
    }
    float grazing = schlick_fresnel_pow5(1.0 - clamp(NdotV, 0.0, 1.0));
    return sheenLum * mix(0.35, 1.0, grazing);
}

void pbr_lobe_probabilities(Material material, float NdotV, out float diffuseProbability, out float specularProbability, out float sheenProbability, out float clearcoatProbability) {
    vec3 f0 = pbr_f0(material);
    vec3 fresnel = material.use_conductor_optics != 0u
        ? conductor_fresnel(material, NdotV)
        : f0 + (vec3(1.0) - f0) * schlick_fresnel_pow5(1.0 - max(NdotV, 0.0));
    float specularWeight = max(luminance(fresnel), 0.0);
    float diffuseWeight = max(luminance(pbr_diffuse_energy(material) * material.color * (1.0 / PI)), 0.0);
    float sheenWeight = material_sheen_weight(material, NdotV);
    float clearcoatWeight = clamp(material.clearcoat_factor, 0.0, 1.0) * 0.25;
    float totalWeight = max(diffuseWeight + specularWeight + sheenWeight + clearcoatWeight, 1.0e-6);
    diffuseProbability = diffuseWeight / totalWeight;
    specularProbability = specularWeight / totalWeight;
    sheenProbability = sheenWeight / totalWeight;
    clearcoatProbability = clearcoatWeight / totalWeight;
    if (clearcoatWeight > 0.0) {
        clearcoatProbability = clamp(clearcoatProbability, 0.03, 0.25);
        float remaining = 1.0 - clearcoatProbability;
        float baseTotal = max(diffuseWeight + specularWeight + sheenWeight, 1.0e-6);
        diffuseProbability = remaining * diffuseWeight / baseTotal;
        specularProbability = remaining * specularWeight / baseTotal;
        sheenProbability = remaining * sheenWeight / baseTotal;
    }
    if (sheenWeight > 0.0) {
        sheenProbability = clamp(sheenProbability, 0.05, 0.35 * (1.0 - clearcoatProbability));
        float remaining = 1.0 - clearcoatProbability - sheenProbability;
        float baseTotal = max(diffuseWeight + specularWeight, 1.0e-6);
        diffuseProbability = remaining * diffuseWeight / baseTotal;
        specularProbability = remaining * specularWeight / baseTotal;
    }
    specularProbability = clamp(specularProbability, 0.05, max(0.05, 0.95 - sheenProbability - clearcoatProbability));
    diffuseProbability = max(1.0 - specularProbability - sheenProbability - clearcoatProbability, 0.0);
}

float pbr_specular_sample_probability(Material material, float NdotV) {
    float diffuseProbability;
    float specularProbability;
    float sheenProbability;
    float clearcoatProbability;
    pbr_lobe_probabilities(material, NdotV, diffuseProbability, specularProbability, sheenProbability, clearcoatProbability);
    return specularProbability;
}

vec3 sample_cosine_hemisphere(inout uint state, vec3 normal, out float pdf) {
    float r1 = 2.0 * PI * rand_f32(state);
    float r2 = rand_f32(state);
    float r2s = sqrt(r2);
    vec3 axis = abs(normal.x) > 0.1 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(axis, normal));
    vec3 bitangent = cross(normal, tangent);
    vec3 dir = tangent * cos(r1) * r2s + bitangent * sin(r1) * r2s + normal * sqrt(max(1.0 - r2, 0.0));
    pdf = diffuse_pdf(normal, dir);
    return dir;
}

void tangent_frame(vec3 n, out vec3 tangent, out vec3 bitangent) {
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    tangent = normalize(cross(axis, n));
    bitangent = cross(n, tangent);
}

vec3 to_tangent_space(vec3 v, vec3 tangent, vec3 bitangent, vec3 n) {
    return vec3(dot(v, tangent), dot(v, bitangent), dot(v, n));
}

vec3 from_tangent_space(vec3 v, vec3 tangent, vec3 bitangent, vec3 n) {
    return tangent * v.x + bitangent * v.y + n * v.z;
}

float ggx_ndf(float roughness, float n_dot_h) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float a2 = a * a;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-10);
}

vec3 schlick_fresnel(vec3 f0, float v_dot_h) {
    float f = schlick_fresnel_pow5(clamp(1.0 - v_dot_h, 0.0, 1.0));
    return f0 + (vec3(1.0) - f0) * f;
}

bool material_uses_anisotropy(Material material);
void material_anisotropic_frame(Material material, vec3 n, inout vec3 tangent, inout vec3 bitangent);
void material_anisotropic_alpha(Material material, out float alphaX, out float alphaY);
float ggx_anisotropic_ndf(float alphaX, float alphaY, vec3 h, vec3 n, vec3 tangent, vec3 bitangent);
float smith_g1_anisotropic(float alphaX, float alphaY, vec3 v, vec3 n, vec3 tangent, vec3 bitangent);
vec3 sample_ggx_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent);

float smith_g1(float roughness, float n_dot_x) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float a2 = a * a;
    float n2 = n_dot_x * n_dot_x;
    return 2.0 * n_dot_x / max(n_dot_x + sqrt(a2 + (1.0 - a2) * n2), 1e-10);
}

float smith_ggx_lambda(float roughness, float n_dot_x) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float a2 = a * a;
    float n2 = max(n_dot_x * n_dot_x, 1e-8);
    float tan2Theta = max(1.0 - n2, 0.0) / n2;
    return 0.5 * (sqrt(1.0 + a2 * tan2Theta) - 1.0);
}

float ggx_visible_normal_pdf(Material material, vec3 wo, vec3 h, vec3 n, vec3 tangent, vec3 bitangent) {
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(wo, h), 0.0);
    if (n_dot_v < 1e-6 || n_dot_h < 1e-6 || v_dot_h < 1e-6) {
        return 0.0;
    }
    if (material_uses_anisotropy(material)) {
        vec3 t = tangent;
        vec3 b = bitangent;
        material_anisotropic_frame(material, n, t, b);
        float alphaX;
        float alphaY;
        material_anisotropic_alpha(material, alphaX, alphaY);
        return ggx_anisotropic_ndf(alphaX, alphaY, h, n, t, b) *
            smith_g1_anisotropic(alphaX, alphaY, wo, n, t, b) /
            max(4.0 * n_dot_v, 1e-10);
    }
    return ggx_ndf(material.roughness, n_dot_h) * smith_g1(material.roughness, n_dot_v) / max(4.0 * n_dot_v, 1e-10);
}

float smith_g(float roughness, float n_dot_v, float n_dot_l) {
    if (n_dot_v <= 0.0 || n_dot_l <= 0.0) {
        return 0.0;
    }
    float lambdaV = smith_ggx_lambda(roughness, n_dot_v);
    float lambdaL = smith_ggx_lambda(roughness, n_dot_l);
    return 1.0 / max(1.0 + lambdaV + lambdaL, 1e-8);
}

bool material_uses_anisotropy(Material material) {
    return abs(material.anisotropy_strength) > 1.0e-4;
}

void material_anisotropic_frame(Material material, vec3 n, inout vec3 tangent, inout vec3 bitangent) {
    tangent -= n * dot(n, tangent);
    float tangentLen2 = dot(tangent, tangent);
    if (tangentLen2 <= 1.0e-8) {
        tangent_frame(n, tangent, bitangent);
    } else {
        tangent *= inversesqrt(tangentLen2);
        bitangent = normalize(cross(n, tangent));
    }
    float s = sin(material.anisotropy_rotation);
    float c = cos(material.anisotropy_rotation);
    vec3 t = tangent * c + bitangent * s;
    vec3 b = bitangent * c - tangent * s;
    tangent = normalize(t);
    bitangent = normalize(b);
}

void material_anisotropic_alpha(Material material, out float alphaX, out float alphaY) {
    float a = max(ggx_safe_roughness(material.roughness) * ggx_safe_roughness(material.roughness), 1.0e-4);
    float strength = clamp(material.anisotropy_strength, -0.99, 0.99);
    float aspect = sqrt(max(1.0 - 0.9 * abs(strength), 0.1));
    alphaX = strength >= 0.0 ? a / aspect : a * aspect;
    alphaY = strength >= 0.0 ? a * aspect : a / aspect;
}

float ggx_anisotropic_ndf(float alphaX, float alphaY, vec3 h, vec3 n, vec3 tangent, vec3 bitangent) {
    float hx = dot(h, tangent);
    float hy = dot(h, bitangent);
    float hz = max(dot(h, n), 0.0);
    float denom = hx * hx / max(alphaX * alphaX, 1.0e-8) +
        hy * hy / max(alphaY * alphaY, 1.0e-8) +
        hz * hz;
    return 1.0 / max(PI * alphaX * alphaY * denom * denom, 1.0e-10);
}

float smith_ggx_anisotropic_lambda(float alphaX, float alphaY, vec3 v, vec3 n, vec3 tangent, vec3 bitangent) {
    float vx = dot(v, tangent);
    float vy = dot(v, bitangent);
    float vz = max(dot(v, n), 1.0e-6);
    float tan2 = (alphaX * vx) * (alphaX * vx) + (alphaY * vy) * (alphaY * vy);
    return 0.5 * (sqrt(1.0 + tan2 / max(vz * vz, 1.0e-8)) - 1.0);
}

float smith_g1_anisotropic(float alphaX, float alphaY, vec3 v, vec3 n, vec3 tangent, vec3 bitangent) {
    if (dot(v, n) <= 0.0) {
        return 0.0;
    }
    return 1.0 / max(1.0 + smith_ggx_anisotropic_lambda(alphaX, alphaY, v, n, tangent, bitangent), 1.0e-8);
}

float smith_g_anisotropic(float alphaX, float alphaY, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    if (dot(wo, n) <= 0.0 || dot(wi, n) <= 0.0) {
        return 0.0;
    }
    float lambdaV = smith_ggx_anisotropic_lambda(alphaX, alphaY, wo, n, tangent, bitangent);
    float lambdaL = smith_ggx_anisotropic_lambda(alphaX, alphaY, wi, n, tangent, bitangent);
    return 1.0 / max(1.0 + lambdaV + lambdaL, 1.0e-8);
}

float ggx_directional_albedo(float roughness, float n_dot_v) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float a2 = a * a;
    float mu = clamp(n_dot_v, 0.0, 1.0);
    return (mu * (1.0 + a2)) / (mu * (1.0 + a2) + a * (1.0 - mu));
}

vec3 ggx_energy_compensation(vec3 f0, float roughness, float n_dot_v) {
    float r = ggx_safe_roughness(roughness);
    float r2 = r * r;
    float singleScatterEnergy = clamp(1.0 - r2 * (0.45 + 0.25 * (1.0 - clamp(n_dot_v, 0.0, 1.0))), 0.35, 1.0);
    vec3 averageFresnel = f0 + (vec3(1.0) - f0) * (1.0 / 21.0);
    vec3 multiScatter = averageFresnel * (1.0 - singleScatterEnergy) / max(singleScatterEnergy, 1e-4);
    return vec3(1.0) + multiScatter;
}

vec3 heitz_ms_ggx(vec3 f0, float roughness, float n_dot_v, float n_dot_l) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float E_v = ggx_directional_albedo(roughness, n_dot_v);
    float E_l = ggx_directional_albedo(roughness, n_dot_l);
    float E_avg = clamp(1.0 / (1.0 + a * 0.66), 0.0, 1.0);
    vec3 f_avg = f0 + (vec3(1.0) - f0) / 21.0;
    vec3 f_ms = f_avg * f_avg / max(vec3(1.0) - f_avg * (1.0 - E_avg), vec3(1e-4));
    return f_ms * (1.0 - E_v) * (1.0 - E_l) / max(PI * E_v * E_l, 1e-8);
}

float charlie_inv_alpha(float roughness) {
    float alpha = max(roughness * roughness, 0.001);
    return 1.0 / alpha;
}

float charlie_ndf(float roughness, float n_dot_h) {
    float invAlpha = charlie_inv_alpha(roughness);
    float sin2Theta = max(1.0 - n_dot_h * n_dot_h, 0.0);
    return (2.0 + invAlpha) * pow(sin2Theta, invAlpha * 0.5) / (2.0 * PI);
}

float sheen_visibility(float n_dot_v, float n_dot_l) {
    return 1.0 / max(4.0 * max(n_dot_v, 0.01) * max(n_dot_l, 0.01), 1.0e-4);
}

vec3 eval_sheen_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v <= 1.0e-6 || n_dot_l <= 1.0e-6 || luminance(material.sheen_color) <= 1.0e-6) {
        return vec3(0.0);
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1.0e-12) {
        return vec3(0.0);
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(n, h), 0.0);
    return material.sheen_color * charlie_ndf(material.sheen_roughness, n_dot_h) * sheen_visibility(n_dot_v, n_dot_l);
}

vec3 eval_clearcoat_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    float factor = clamp(material.clearcoat_factor, 0.0, 1.0);
    if (factor <= 1.0e-5) {
        return vec3(0.0);
    }
    vec3 clearcoatN = material_clearcoat_normal(material, n);
    float n_dot_v = max(dot(clearcoatN, wo), 0.0);
    float n_dot_l = max(dot(clearcoatN, wi), 0.0);
    if (n_dot_v <= 1.0e-6 || n_dot_l <= 1.0e-6) {
        return vec3(0.0);
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1.0e-12) {
        return vec3(0.0);
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(clearcoatN, h), 0.0);
    float v_dot_h = max(dot(wo, h), 0.0);
    vec3 f = schlick_fresnel(vec3(0.04), v_dot_h);
    float roughness = clamp(material.clearcoat_roughness, MATERIAL_MIN_GGX_ROUGHNESS, 1.0);
    float d = ggx_ndf(roughness, n_dot_h);
    float g = smith_g(roughness, n_dot_v, n_dot_l);
    return factor * f * d * g / max(4.0 * n_dot_v * n_dot_l, 1.0e-10);
}

float pdf_clearcoat_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    if (material.clearcoat_factor <= 1.0e-5) {
        return 0.0;
    }
    vec3 clearcoatN = material_clearcoat_normal(material, n);
    float n_dot_v = max(dot(clearcoatN, wo), 0.0);
    float n_dot_l = max(dot(clearcoatN, wi), 0.0);
    if (n_dot_v <= 1.0e-6 || n_dot_l <= 1.0e-6) {
        return 0.0;
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1.0e-12) {
        return 0.0;
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(clearcoatN, h), 0.0);
    float v_dot_h = max(dot(wo, h), 1.0e-6);
    float roughness = clamp(material.clearcoat_roughness, MATERIAL_MIN_GGX_ROUGHNESS, 1.0);
    return ggx_ndf(roughness, n_dot_h) * n_dot_h / max(4.0 * v_dot_h, 1.0e-6);
}

vec3 sample_clearcoat_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent) {
    Material clearcoatMaterial = material;
    clearcoatMaterial.roughness = clamp(material.clearcoat_roughness, MATERIAL_MIN_GGX_ROUGHNESS, 1.0);
    clearcoatMaterial.anisotropy_strength = 0.0;
    return sample_ggx_brdf(state, clearcoatMaterial, wo, material_clearcoat_normal(material, n), tangent, bitangent);
}

float pdf_sheen_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v <= 1.0e-6 || n_dot_l <= 1.0e-6 || luminance(material.sheen_color) <= 1.0e-6) {
        return 0.0;
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1.0e-12) {
        return 0.0;
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(wo, h), 1.0e-6);
    return charlie_ndf(material.sheen_roughness, n_dot_h) * n_dot_h / max(4.0 * v_dot_h, 1.0e-6);
}

vec3 sample_sheen_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent) {
    tangent_frame(n, tangent, bitangent);
    float invAlpha = charlie_inv_alpha(material.sheen_roughness);
    float u1 = rand_f32(state);
    float u2 = rand_f32(state);
    float sinTheta = pow(u1, 1.0 / (invAlpha + 2.0));
    float cosTheta = sqrt(max(1.0 - sinTheta * sinTheta, 0.0));
    float phi = 2.0 * PI * u2;
    vec3 h = normalize(from_tangent_space(vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta), tangent, bitangent, n));
    if (dot(wo, h) <= 0.0) {
        h = reflect(h, n);
    }
    return normalize(2.0 * dot(wo, h) * h - wo);
}

vec3 thin_film_tint(Material material, float cosTheta) {
    float factor = clamp(material.iridescence_factor, 0.0, 1.0);
    if (factor <= 1.0e-5) {
        return vec3(1.0);
    }
    float eta = max(material.iridescence_ior, 1.01);
    float thicknessNm = clamp(material.iridescence_thickness_min, 0.0, max(material.iridescence_thickness_max, material.iridescence_thickness_min));
    float sin2T = max(1.0 - cosTheta * cosTheta, 0.0) / (eta * eta);
    float cosT = sqrt(max(1.0 - sin2T, 0.0));
    float opticalPathNm = 2.0 * eta * thicknessNm * cosT;
    vec3 phase = 2.0 * PI * opticalPathNm / vec3(650.0, 510.0, 475.0);
    vec3 tint = 0.55 + 0.45 * cos(phase);
    float tintLum = max(luminance(tint), 1.0e-4);
    tint = clamp(tint / tintLum, vec3(0.25), vec3(1.75));
    return mix(vec3(1.0), tint, factor);
}

vec3 eval_ggx_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    Material specMaterial = material;
    specMaterial.roughness = material_specular_roughness(material);
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v < 1e-6 || n_dot_l < 1e-6) {
        return vec3(0.0);
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1e-12) {
        return vec3(0.0);
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(wo, h), 0.0);
    vec3 f0 = pbr_f0(specMaterial);
    vec3 f = specMaterial.use_conductor_optics != 0u
        ? conductor_fresnel(specMaterial, v_dot_h)
        : schlick_fresnel(f0, v_dot_h);
    f *= thin_film_tint(specMaterial, v_dot_h);
    float d = ggx_ndf(specMaterial.roughness, n_dot_h);
    float g = smith_g(specMaterial.roughness, n_dot_v, n_dot_l);
    if (material_uses_anisotropy(specMaterial)) {
        material_anisotropic_frame(specMaterial, n, tangent, bitangent);
        float alphaX;
        float alphaY;
        material_anisotropic_alpha(specMaterial, alphaX, alphaY);
        d = ggx_anisotropic_ndf(alphaX, alphaY, h, n, tangent, bitangent);
        g = smith_g_anisotropic(alphaX, alphaY, wo, wi, n, tangent, bitangent);
    }
    vec3 specular = f * d * g / max(4.0 * n_dot_v * n_dot_l, 1e-10);
    vec3 msCompensation = heitz_ms_ggx(f0, specMaterial.roughness, n_dot_v, n_dot_l);
    specular += msCompensation;
    vec3 diffuse = pbr_diffuse_energy(material) * eval_diffuse_brdf(material, wo, wi, n);
    return diffuse + specular;
}

float pdf_ggx_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    Material specMaterial = material;
    specMaterial.roughness = material_specular_roughness(material);
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v < 1e-6 || n_dot_l < 1e-6) {
        return 0.0;
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1e-12) {
        return 0.0;
    }
    vec3 h = normalize(halfVector);
    return ggx_visible_normal_pdf(specMaterial, wo, h, n, tangent, bitangent);
}

float pdf_pbr_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    float NdotV = max(dot(n, wo), 0.0);
    float diffuseProbability;
    float specularProbability;
    float sheenProbability;
    float clearcoatProbability;
    pbr_lobe_probabilities(material, NdotV, diffuseProbability, specularProbability, sheenProbability, clearcoatProbability);
    return diffuseProbability * diffuse_pdf(n, wi) +
        specularProbability * pdf_ggx_brdf(material, wo, wi, n, tangent, bitangent) +
        sheenProbability * pdf_sheen_brdf(material, wo, wi, n) +
        clearcoatProbability * pdf_clearcoat_brdf(material, wo, wi, n);
}

vec3 sample_ggx_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent) {
    Material specMaterial = material;
    specMaterial.roughness = material_specular_roughness(material);
    float r = ggx_safe_roughness(specMaterial.roughness);
    float a = r * r;
    float r1 = rand_f32(state);
    float r2 = rand_f32(state);
    if (material_uses_anisotropy(specMaterial)) {
        material_anisotropic_frame(specMaterial, n, tangent, bitangent);
        float alphaX;
        float alphaY;
        material_anisotropic_alpha(specMaterial, alphaX, alphaY);
        vec3 vAniso = to_tangent_space(wo, tangent, bitangent, n);
        if (vAniso.z <= 0.0) {
            return reflect(-wo, n);
        }
        vec3 vhAniso = normalize(vec3(alphaX * vAniso.x, alphaY * vAniso.y, vAniso.z));
        float lensqAniso = vhAniso.x * vhAniso.x + vhAniso.y * vhAniso.y;
        vec3 t1Aniso = lensqAniso > 1.0e-8 ? vec3(-vhAniso.y, vhAniso.x, 0.0) * inversesqrt(lensqAniso) : vec3(1.0, 0.0, 0.0);
        vec3 t2Aniso = cross(vhAniso, t1Aniso);
        float radiusAniso = sqrt(r1);
        float phiAniso = 2.0 * PI * r2;
        float p1Aniso = radiusAniso * cos(phiAniso);
        float p2Aniso = radiusAniso * sin(phiAniso);
        float blendAniso = 0.5 * (1.0 + vhAniso.z);
        p2Aniso = mix(sqrt(max(0.0, 1.0 - p1Aniso * p1Aniso)), p2Aniso, blendAniso);
        vec3 nhAniso = p1Aniso * t1Aniso + p2Aniso * t2Aniso + sqrt(max(0.0, 1.0 - p1Aniso * p1Aniso - p2Aniso * p2Aniso)) * vhAniso;
        vec3 hLocalAniso = normalize(vec3(alphaX * nhAniso.x, alphaY * nhAniso.y, max(nhAniso.z, 0.0)));
        vec3 hAniso = normalize(from_tangent_space(hLocalAniso, tangent, bitangent, n));
        return 2.0 * dot(wo, hAniso) * hAniso - wo;
    }
    tangent_frame(n, tangent, bitangent);
    vec3 v = to_tangent_space(wo, tangent, bitangent, n);
    if (v.z <= 0.0) {
        return reflect(-wo, n);
    }

    vec3 vh = normalize(vec3(a * v.x, a * v.y, v.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lensq > 1.0e-8 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);

    float radius = sqrt(r1);
    float phi = 2.0 * PI * r2;
    float p1 = radius * cos(phi);
    float p2 = radius * sin(phi);
    float blend = 0.5 * (1.0 + vh.z);
    p2 = mix(sqrt(max(0.0, 1.0 - p1 * p1)), p2, blend);

    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    vec3 hLocal = normalize(vec3(a * nh.x, a * nh.y, max(nh.z, 0.0)));
    vec3 h = normalize(from_tangent_space(hLocal, tangent, bitangent, n));
    return 2.0 * dot(wo, h) * h - wo;
}

vec3 eval_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    MaterialClosure c = material_to_closure(material);
    vec3 result = vec3(0.0);

    float NdotL = max(dot(n, wi), 0.0);

    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_DIFFUSE)) {
        result += c.weight * eval_diffuse_brdf(material, wo, wi, n);
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SSS)) {
        float sssRadius = max(c.ior, 0.01);
        float wrap = NdotL * 0.5 + 0.5;
        result += c.weight * c.color * (sssRadius / PI) * wrap;
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SHEEN)) {
        result += c.weight * eval_sheen_brdf(material, wo, wi, n);
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SPECULAR)) {
        result += c.weight * eval_ggx_brdf(material, wo, wi, n, tangent, bitangent);
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_CLEARCOAT)) {
        result += c.weight * eval_clearcoat_brdf(material, wo, wi, n);
    }
    return result;
}

float pdf_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    if (material_is_delta(material)) {
        return 0.0;
    }
    MaterialClosure c = material_to_closure(material);
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SPECULAR)) {
        return pdf_pbr_brdf(material, wo, wi, n, tangent, bitangent);
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_DIFFUSE) ||
        closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SSS) ||
        closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SHEEN)) {
        return diffuse_pdf(n, wi);
    }
    return 0.0;
}

vec3 sample_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent, out float pdf) {
    MaterialClosure c = material_to_closure(material);
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SPECULAR)) {
        float NdotV_sample = max(dot(n, wo), 0.0);
        float diffuseProbability;
        float specularProbability;
        float sheenProbability;
        float clearcoatProbability;
        pbr_lobe_probabilities(material, NdotV_sample, diffuseProbability, specularProbability, sheenProbability, clearcoatProbability);
        vec3 wi;
        float lobeSample = rand_f32(state);
        if (lobeSample < clearcoatProbability) {
            wi = sample_clearcoat_brdf(state, material, wo, n, tangent, bitangent);
        } else if (lobeSample < clearcoatProbability + specularProbability) {
            wi = sample_ggx_brdf(state, material, wo, n, tangent, bitangent);
        } else if (lobeSample < clearcoatProbability + specularProbability + sheenProbability) {
            wi = sample_sheen_brdf(state, material, wo, n, tangent, bitangent);
        } else {
            float diffusePdf;
            wi = sample_cosine_hemisphere(state, n, diffusePdf);
        }
        pdf = pdf_pbr_brdf(material, wo, wi, n, tangent, bitangent);
        return wi;
    }
    return sample_cosine_hemisphere(state, n, pdf);
}

// ---------------------------------------------------------------------------

#endif // RTV_RT_BSDF_GLSL
