#ifndef RTV_RESTIR_DI_LIGHT_EVALUATION_GLSL
#define RTV_RESTIR_DI_LIGHT_EVALUATION_GLSL

// Compute-pass light resolution and target-integrand evaluation; requires restir_di_types_accessors.glsl.
bool restir_di_resolve_light(
    RestirDiReservoir reservoir,
    out RestirDiLightRecord light,
    out uint resolvedIndex) {
    uint reservoirKind = restir_di_light_kind(reservoir);
    if (restir_di_light_kind_infinite(reservoirKind)) {
        resolvedIndex = RESTIR_DI_PSEUDO_LIGHT_INDEX;
        vec3 storedDirection = reservoir.samplePosition_distance.xyz;
        float storedDirectionLen2 = dot(storedDirection, storedDirection);
        vec3 direction = storedDirectionLen2 > 1.0e-8
            ? storedDirection * inversesqrt(storedDirectionLen2)
            : vec3(0.0, 1.0, 0.0);
        vec3 radiance = restir_di_sample_radiance(reservoir);
        float sourcePdf = restir_di_source_pdf(reservoir);
        light.metadata = uvec4(reservoirKind, 0u, 0u, 0u);
        light.identity = reservoirKind == RESTIR_DI_LIGHT_ENVIRONMENT
            ? uvec4(RESTIR_DI_ENVIRONMENT_ID_HASH, 0u, RESTIR_DI_ENVIRONMENT_VERSION, 0u)
            : uvec4(RESTIR_DI_SUN_ID_HASH, 0u, RESTIR_DI_SUN_VERSION, 0u);
        light.data0 = vec4(sourcePdf, sourcePdf, 0.0, 0.0);
        light.data1 = vec4(direction, 0.0);
        light.data2 = vec4(radiance, 0.0);
        light.data3 = vec4(0.0);
        return sourcePdf > 1.0e-6 &&
            storedDirectionLen2 > 1.0e-8 &&
            dot(radiance, vec3(0.2126, 0.7152, 0.0722)) > 0.0;
    }

    uint cachedIndex = restir_di_light_index(reservoir);
    if (cachedIndex < restir_di_scene.lightCount &&
        restir_di_light_identity_matches(reservoir, restir_di_light_records[cachedIndex])) {
        resolvedIndex = cachedIndex;
        light = restir_di_light_records[cachedIndex];
        return true;
    }
    // Light/distribution changes invalidate DI history on the CPU. Avoid an
    // unbounded per-pixel scan when a stale cached index does not match.
    resolvedIndex = 0u;
    light.metadata = uvec4(0u);
    light.identity = uvec4(0u);
    light.data0 = vec4(0.0);
    light.data1 = vec4(0.0);
    light.data2 = vec4(0.0);
    light.data3 = vec4(0.0);
    return false;
}

float restir_di_light_selection_pdf(uint lightIndex, RestirDiLightRecord light) {
    if (restir_di_light_kind_infinite(light.metadata.x)) {
        return max(light.data0.x, 0.0);
    }
    if (restir_di_scene.lightCount == 0u || restir_di_scene.totalLightWeight <= 1.0e-8) return 0.0;
    float authoredProbability = restir_di_scene.authoredLightCount == 0u
        ? 0.0
        : (restir_di_scene.lightCount > restir_di_scene.authoredLightCount ? 0.5 : 1.0);
    float pdf = (1.0 - authoredProbability) * max(light.data0.x, 0.0) /
        max(restir_di_scene.totalLightWeight, 1.0e-6);
    if (light.metadata.x >= 2u && restir_di_scene.authoredLightCount > 0u) {
        pdf += authoredProbability / float(restir_di_scene.authoredLightCount);
    }
    return pdf;
}

float restir_di_pdf_at_receiver(
    uint lightIndex,
    RestirDiLightRecord light,
    vec3 receiverPosition,
    vec3 samplePosition,
    vec3 lightNormal) {
    float selectionPdf = restir_di_light_selection_pdf(lightIndex, light);
    uint kind = light.metadata.x;
    if (kind == RESTIR_DI_LIGHT_DIRECTIONAL ||
        kind == RESTIR_DI_LIGHT_POINT ||
        kind == RESTIR_DI_LIGHT_SPOT ||
        restir_di_light_kind_infinite(kind)) {
        return selectionPdf;
    }
    vec3 toLight = samplePosition - receiverPosition;
    float distanceSquared = dot(toLight, toLight);
    vec3 direction = toLight * inversesqrt(max(distanceSquared, 1.0e-8));
    float rawCosineAtLight = dot(-direction, normalize(lightNormal));
    float cosineAtLight = kind == RESTIR_DI_LIGHT_EMISSIVE_TRIANGLE
        ? abs(rawCosineAtLight)
        : max(rawCosineAtLight, 0.0);
    float area = max(light.data0.w, 1.0e-6);
    return cosineAtLight > 1.0e-6 ? selectionPdf * distanceSquared / (cosineAtLight * area) : 0.0;
}

vec3 restir_di_light_normal(RestirDiReservoir r);

vec3 restir_di_evaluate_integrand(
    RestirDiReservoir reservoir,
    RestirDiReceiver current,
    RestirDiLightRecord light,
    float lightPdf,
    out vec3 currentDirection,
    out float currentDistance,
    out vec3 currentLightNormal) {
    uint kind = restir_di_light_kind(reservoir);
    vec3 samplePosition = reservoir.samplePosition_distance.xyz;
    if (kind == RESTIR_DI_LIGHT_DIRECTIONAL || restir_di_light_kind_infinite(kind)) {
        currentDirection = normalize(light.data1.xyz);
        currentDistance = 10000.0;
    } else {
        vec3 toLight = samplePosition - current.worldPosition_depth.xyz;
        currentDistance = length(toLight);
        currentDirection = toLight / max(currentDistance, 1.0e-6);
    }
    currentLightNormal = restir_di_light_normal(reservoir);
    if (kind == RESTIR_DI_LIGHT_DIRECTIONAL ||
        kind == RESTIR_DI_LIGHT_POINT ||
        restir_di_light_kind_infinite(kind)) {
        currentLightNormal = -currentDirection;
    }
    if (kind == RESTIR_DI_LIGHT_AREA || kind == RESTIR_DI_LIGHT_SPOT) {
        currentLightNormal = normalize(vec3(light.data1.w, light.data2.w, light.data3.x));
    }

    // Emissive triangle/sphere samples retain their sampled radiance. This is
    // required for textured triangle emission and avoids replacing it with a
    // light-record average during reuse.
    vec3 incidentRadiance = restir_di_sample_radiance(reservoir);
    if (kind == RESTIR_DI_LIGHT_DIRECTIONAL || kind == RESTIR_DI_LIGHT_AREA) {
        incidentRadiance = max(light.data2.xyz, vec3(0.0));
    } else if (kind == RESTIR_DI_LIGHT_POINT) {
        incidentRadiance = max(light.data2.xyz, vec3(0.0)) /
            max(currentDistance * currentDistance, 1.0e-4);
    } else if (kind == RESTIR_DI_LIGHT_SPOT) {
        vec3 spotDirection = currentLightNormal;
        float cosInner = cos(clamp(light.data3.y, 0.0, 3.14159265));
        float cosOuter = cos(clamp(max(light.data3.z, light.data3.y), 0.0, 3.14159265));
        float cone = clamp((dot(-currentDirection, spotDirection) - cosOuter) /
            max(cosInner - cosOuter, 1.0e-4), 0.0, 1.0);
        incidentRadiance = max(light.data2.xyz, vec3(0.0)) * (cone * cone) /
            max(currentDistance * currentDistance, 1.0e-4);
    }

    vec3 normal = normalize(current.normal_roughness.xyz);
    vec3 view = restir_di_receiver_view_direction(current);
    float currentCosine = max(dot(normalize(current.normal_roughness.xyz), currentDirection), 0.0);
    if (currentCosine <= 0.0 || dot(incidentRadiance, vec3(0.2126, 0.7152, 0.0722)) <= 0.0) return vec3(0.0);

    vec3 baseColor = restir_di_receiver_base_color(current);
    vec3 f0 = restir_di_receiver_specular_color(current);
    float metallic = restir_di_receiver_metallic(current);
    float roughness = clamp(current.normal_roughness.w, 0.001, 1.0);
    vec3 halfVectorSum = view + currentDirection;
    if (dot(halfVectorSum, halfVectorSum) <= 1.0e-12) return vec3(0.0);
    vec3 halfVector = normalize(halfVectorSum);
    float nDotV = max(dot(normal, view), 1.0e-4);
    float nDotL = currentCosine;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float vDotH = max(dot(view, halfVector), 0.0);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / max(3.14159265 * denom * denom, 1.0e-6);
    float lambdaV = 0.5 * (sqrt(1.0 + alpha2 * max(1.0 - nDotV * nDotV, 0.0) /
        max(nDotV * nDotV, 1.0e-8)) - 1.0);
    float lambdaL = 0.5 * (sqrt(1.0 + alpha2 * max(1.0 - nDotL * nDotL, 0.0) /
        max(nDotL * nDotL, 1.0e-8)) - 1.0);
    float geometry = 1.0 / max(1.0 + lambdaV + lambdaL, 1.0e-8);
    vec3 fresnel = f0 + (vec3(1.0) - f0) * pow(1.0 - vDotH, 5.0);
    vec3 specular = fresnel * distribution * geometry /
        max(4.0 * nDotV * nDotL, 1.0e-5);

    float sigma = roughness * 1.2217304764;
    float sigma2 = sigma * sigma;
    float orenA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    float orenB = 0.45 * sigma2 / (sigma2 + 0.09);
    vec3 viewTangent = view - normal * nDotV;
    vec3 lightTangent = currentDirection - normal * nDotL;
    float viewLen2 = dot(viewTangent, viewTangent);
    float lightLen2 = dot(lightTangent, lightTangent);
    float cosPhiDiff = viewLen2 > 1.0e-8 && lightLen2 > 1.0e-8
        ? dot(viewTangent, lightTangent) * inversesqrt(viewLen2 * lightLen2) : 0.0;
    float sinAlpha = sqrt(max(1.0 - max(nDotV, nDotL) * max(nDotV, nDotL), 0.0));
    float tanBeta = sqrt(max(1.0 - min(nDotV, nDotL) * min(nDotV, nDotL), 0.0)) /
        max(min(nDotV, nDotL), 1.0e-4);
    float orenFactor = roughness <= 0.08 ? 1.0 :
        clamp(orenA + orenB * max(cosPhiDiff, 0.0) * sinAlpha * tanBeta, 0.0, 1.0);
    vec3 averageFresnel = f0 + (vec3(1.0) - f0) * (1.0 / 21.0);
    vec3 diffuseEnergy = clamp(vec3(1.0) - averageFresnel, vec3(0.0), vec3(1.0)) * (1.0 - metallic);
    vec3 diffuse = diffuseEnergy * baseColor * (orenFactor / 3.14159265);

    float eV = (nDotV * (1.0 + alpha2)) /
        max(nDotV * (1.0 + alpha2) + alpha * (1.0 - nDotV), 1.0e-8);
    float eL = (nDotL * (1.0 + alpha2)) /
        max(nDotL * (1.0 + alpha2) + alpha * (1.0 - nDotL), 1.0e-8);
    float eAvg = clamp(1.0 / (1.0 + alpha * 0.66), 0.0, 1.0);
    vec3 fMs = averageFresnel * averageFresnel /
        max(vec3(1.0) - averageFresnel * (1.0 - eAvg), vec3(1.0e-4));
    specular += fMs * (1.0 - eV) * (1.0 - eL) /
        max(3.14159265 * eV * eL, 1.0e-8);

    bool pbrClosure = (restir_di_receiver_surface_flags(current) & RESTIR_DI_SURFACE_PBR) != 0u;
    vec3 bsdf = pbrClosure ? diffuse + specular : baseColor * (orenFactor / 3.14159265);

    float diffusePdf = nDotL / 3.14159265;
    float smithG1V = 2.0 * nDotV /
        max(nDotV + sqrt(alpha2 + (1.0 - alpha2) * nDotV * nDotV), 1.0e-8);
    float specularPdf = distribution * smithG1V / max(4.0 * nDotV, 1.0e-8);
    float fresnelLum = dot(f0 + (vec3(1.0) - f0) * pow(1.0 - nDotV, 5.0),
        vec3(0.2126, 0.7152, 0.0722));
    float diffuseLum = dot(diffuseEnergy * baseColor * (1.0 / 3.14159265),
        vec3(0.2126, 0.7152, 0.0722));
    float specularProbability = clamp(fresnelLum / max(fresnelLum + diffuseLum, 1.0e-6), 0.05, 0.95);
    float bsdfPdf = pbrClosure ? mix(diffusePdf, specularPdf, specularProbability) : diffusePdf;
    float lightPdf2 = lightPdf * lightPdf;
    float misWeight = lightPdf2 / max(lightPdf2 + bsdfPdf * bsdfPdf, 1.0e-8);
    return max(bsdf * incidentRadiance * nDotL * misWeight, vec3(0.0));
}


#endif // RTV_RESTIR_DI_LIGHT_EVALUATION_GLSL
