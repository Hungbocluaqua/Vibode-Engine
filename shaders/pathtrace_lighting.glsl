#ifndef RTV_PATHTRACE_LIGHTING_GLSL
#define RTV_PATHTRACE_LIGHTING_GLSL

// Direct-light sampling, visibility, ReSTIR reuse, and volume-light helpers.
bool sample_emissive_light(
    inout uint rng,
    vec3 hitPos,
    out vec3 wi,
    out vec3 emission,
    out float pdfSolidAngle,
    out float distanceToLight,
    out uint lightIndexOut,
    out uint lightKindOut,
    out vec3 samplePosition,
    out vec3 sampleRadiance,
    out vec3 lightNormalOut) {
    lightIndexOut = 0u;
    lightKindOut = 0u;
    samplePosition = vec3(0.0);
    sampleRadiance = vec3(0.0);
    lightNormalOut = vec3(0.0, 1.0, 0.0);
    float totalWeight = mesh_params.emissive_total_area;
    if (totalWeight <= 1e-8 || mesh_params.light_count == 0u) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    uint lightIndex;
    if (!sample_scene_light(rng, lightIndex)) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    LightRecord selected = light_records[lightIndex];
    lightIndexOut = lightIndex;
    lightKindOut = selected.metadata.x;
    float selectedPdf = light_record_selection_pdf(lightIndex);
    if (selected.metadata.x == 2u) {
        wi = normalize(selected.data1.xyz);
        emission = selected.data2.xyz;
        samplePosition = wi;
        sampleRadiance = emission;
        lightNormalOut = -wi;
        pdfSolidAngle = selectedPdf;
        distanceToLight = 10000.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 1u) {
        vec3 center = selected.data1.xyz;
        float radius = max(selected.data0.z, 1.0e-6);
        float z = rand_f32(rng) * 2.0 - 1.0;
        float phi = 2.0 * PI * rand_f32(rng);
        float radial = sqrt(max(1.0 - z * z, 0.0));
        vec3 lightNormal = vec3(radial * cos(phi), z, radial * sin(phi));
        vec3 samplePos = center + lightNormal * radius;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1.0e-8));
        wi = toLight / distanceToLight;
        float cosLight = max(dot(-wi, lightNormal), 0.0);
        samplePosition = samplePos;
        sampleRadiance = max(selected.data2.xyz, vec3(0.0));
        lightNormalOut = lightNormal;
        emission = sampleRadiance;
        float area = max(selected.data0.w, 1.0e-6);
        pdfSolidAngle = cosLight > 1.0e-6 ? selectedPdf * distSq / (cosLight * area) : 0.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 3u) {
        vec3 samplePos = selected.data1.xyz;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;
        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz;
        lightNormalOut = -wi;
        emission = sampleRadiance / max(distSq, 1e-4);
        pdfSolidAngle = selectedPdf;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 5u) {
        vec3 samplePos = selected.data1.xyz;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;

        vec3 spotDirection = normalize(vec3(selected.data1.w, selected.data2.w, selected.data3.x));
        float innerCone = clamp(selected.data3.y, 0.0, PI);
        float outerCone = clamp(max(selected.data3.z, innerCone), 0.0, PI);
        float cosInner = cos(innerCone);
        float cosOuter = cos(outerCone);
        float cosTheta = dot(-wi, spotDirection);
        float coneWeight = clamp((cosTheta - cosOuter) / max(cosInner - cosOuter, 1.0e-4), 0.0, 1.0);
        coneWeight *= coneWeight;

        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz * coneWeight;
        lightNormalOut = spotDirection;
        emission = sampleRadiance / max(distSq, 1e-4);
        pdfSolidAngle = selectedPdf;
        return coneWeight > 0.0 && has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 4u) {
        vec3 center = selected.data1.xyz;
        vec3 lightNormal = normalize(vec3(selected.data1.w, selected.data2.w, selected.data3.x));
        vec3 tangent = normalize(abs(lightNormal.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), lightNormal) : cross(vec3(1.0, 0.0, 0.0), lightNormal));
        vec3 bitangent = normalize(cross(lightNormal, tangent));
        float halfSize = selected.data0.z * 0.5;
        vec3 samplePos = center + tangent * ((rand_f32(rng) * 2.0 - 1.0) * halfSize) + bitangent * ((rand_f32(rng) * 2.0 - 1.0) * halfSize);
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;
        float cosLight = max(dot(-wi, lightNormal), 0.0);
        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz;
        lightNormalOut = lightNormal;
        emission = sampleRadiance;
        float selectionPdf = selectedPdf;
        float area = max(selected.data0.w, 1e-6);
        pdfSolidAngle = cosLight > 1e-6 ? selectionPdf * distSq / (cosLight * area) : 0.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x != 0u) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    uint ti = selected.metadata.y * TRI_STRIDE;
    uint instanceIndex = selected.metadata.w;
    if (instanceIndex >= mesh_params.instance_count || ti + 3u >= mesh_params.local_triangle_count * TRI_STRIDE) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    InstanceRecord instance = instance_records[instanceIndex];
    vec4 td3 = local_triangle_data[ti + 3u];
    Material material = decode_material(selected.metadata.z);
    vec3 v0 = (instance.transform * vec4(local_triangle_data[ti + 0u].xyz, 1.0)).xyz;
    vec3 v1 = (instance.transform * vec4(local_triangle_data[ti + 1u].xyz, 1.0)).xyz;
    vec3 v2 = (instance.transform * vec4(local_triangle_data[ti + 2u].xyz, 1.0)).xyz;
    float r1 = rand_f32(rng);
    float r2 = rand_f32(rng);
    float sr1 = sqrt(r1);
    vec3 bary = vec3(1.0 - sr1, sr1 * (1.0 - r2), sr1 * r2);
    vec3 samplePos = v0 * bary.x + v1 * bary.y + v2 * bary.z;
    vec3 lightNormal = normalize(mat3(instance.normal_transform) * td3.xyz);
    vec4 uv01 = local_triangle_data[ti + 4u];
    vec4 uv2 = local_triangle_data[ti + 5u];
    vec2 sampleUv = uv01.xy * bary.x + uv01.zw * bary.y + uv2.xy * bary.z;
    apply_material_textures(material, sampleUv, sampleUv);
    sampleRadiance = material.emissive;
    emission = sampleRadiance;

    vec3 toLight = samplePos - hitPos;
    float distSq = dot(toLight, toLight);
    distanceToLight = sqrt(max(distSq, 1e-8));
    wi = toLight / distanceToLight;
    float cosLight = max(dot(-wi, lightNormal), 0.0);
    samplePosition = samplePos;
    lightNormalOut = lightNormal;
    float selectionPdf = selectedPdf;
    float area = max(selected.data0.w, 1e-6);
    pdfSolidAngle = cosLight > 1e-6 ? selectionPdf * distSq / (cosLight * area) : 0.0;
    return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
}

bool sample_sun_light(inout uint rng, out vec3 wi, out vec3 radiance, out float pdfSolidAngle) {
    if (analytical_sun_visibility() <= 0.0) {
        wi = vec3(0.0, 1.0, 0.0);
        radiance = vec3(0.0);
        pdfSolidAngle = 0.0;
        return false;
    }
    vec3 sunDir = analytical_sun_direction();
    float radius = clamp(camera.sun_color_angular_radius.w, 0.0001, 0.08);
    float cosRadius = cos(radius);
    float u1 = rand_f32(rng);
    float u2 = rand_f32(rng);
    float cosTheta = mix(cosRadius, 1.0, u1);
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float phi = 2.0 * PI * u2;
    vec3 axis = abs(sunDir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(axis, sunDir));
    vec3 bitangent = cross(sunDir, tangent);
    wi = tangent * cos(phi) * sinTheta + bitangent * sin(phi) * sinTheta + sunDir * cosTheta;
    pdfSolidAngle = 1.0 / analytical_sun_solid_angle();
    radiance = analytical_sun_center_radiance();
    return luminance(radiance) > 0.0 && pdfSolidAngle > 0.0;
}

float emissive_hit_pdf(RayPayload hit, Material material, vec3 previousOrigin) {
    float totalArea = mesh_params.emissive_total_area;
    if (totalArea <= 1e-8 || !has_positive_radiance(material.emissive)) {
        return 0.0;
    }
    vec3 toLight = hit.world_pos - previousOrigin;
    float distSq = dot(toLight, toLight);
    float dist = sqrt(max(distSq, 1e-8));
    vec3 wi = toLight / dist;
    float cosLight = max(dot(-wi, hit.geom_normal), 0.0);
    return cosLight > 1e-6 ? distSq / (cosLight * totalArea) : 0.0;
}

float terminal_emissive_hit_pdf(TerminalRayPayload hit, Material material, vec3 hitWorldPos, vec3 previousOrigin) {
    float totalArea = mesh_params.emissive_total_area;
    if (totalArea <= 1e-8 || !has_positive_radiance(material.emissive)) {
        return 0.0;
    }
    vec3 toLight = hitWorldPos - previousOrigin;
    float distSq = dot(toLight, toLight);
    vec3 wi = toLight / sqrt(max(distSq, 1e-8));
    float cosLight = max(dot(-wi, hit.geom_normal), 0.0);
    return cosLight > 1e-6 ? distSq / (cosLight * totalArea) : 0.0;
}

vec3 estimate_direct_lighting(
    inout uint rng,
    RayPayload hit,
    Material material,
    vec3 wo,
    float rayTime,
    out vec3 emissiveContribution,
    out vec3 environmentContribution,
    out float sampledLightPdf,
    out float sampledRawLightPdf,
    out float sampledEffectiveLightPdf,
    out float sampledBsdfPdf,
    out float sampledMisWeight,
    out uint sampledType,
    out uint sampledLightIndex,
    out uint sampledLightKind,
    out vec3 sampledLightPosition,
    out float sampledLightDistance,
    out vec3 sampledLightDirection,
    out vec3 sampledLightRadiance,
    out vec3 sampledLightNormal,
    out uint causticTransmissiveHits,
    out uint causticVisiblePaths,
    out uint causticBlockedPaths) {
    emissiveContribution = vec3(0.0);
    environmentContribution = vec3(0.0);
    sampledLightPdf = 0.0;
    sampledRawLightPdf = 0.0;
    sampledEffectiveLightPdf = 0.0;
    sampledBsdfPdf = 0.0;
    sampledMisWeight = 0.0;
    sampledType = 0u;
    sampledLightIndex = 0u;
    sampledLightKind = 0u;
    sampledLightPosition = vec3(0.0);
    sampledLightDistance = 0.0;
    sampledLightDirection = vec3(0.0, 1.0, 0.0);
    sampledLightRadiance = vec3(0.0);
    sampledLightNormal = vec3(0.0, 1.0, 0.0);
    causticTransmissiveHits = 0u;
    causticVisiblePaths = 0u;
    causticBlockedPaths = 0u;
    bool isDelta = material_is_delta(material);
    if (camera.direct_lighting_enabled == 0u || isDelta || debug_params.view == 27u) {
        return vec3(0.0);
    }

    vec3 lightingNormal = normalize(hit.geom_normal);
    uint risCandidateCount = direct_light_ris_candidate_count();
    vec3 selectedWi = vec3(0.0, 1.0, 0.0);
    vec3 selectedEmission = vec3(0.0);
    vec3 selectedBsdf = vec3(0.0);
    float selectedLightPdf = 0.0;
    float selectedBsdfPdf = 0.0;
    float selectedDistance = 0.0;
    uint selectedLightIndex = 0u;
    uint selectedLightKind = 0u;
    vec3 selectedLightPosition = vec3(0.0);
    vec3 selectedLightRadiance = vec3(0.0);
    vec3 selectedLightNormal = vec3(0.0, 1.0, 0.0);
    float selectedProxy = 0.0;
    float proxyWeightSum = 0.0;
    bool selectedCandidate = false;
    if (mesh_params.light_count != 0u && mesh_params.emissive_total_area > 1.0e-8) {
        for (uint candidate = 0u; candidate < risCandidateCount; ++candidate) {
            vec3 candidateWi;
            vec3 candidateEmission;
            float candidateLightPdf;
            float candidateDistance;
            uint candidateLightIndex;
            uint candidateLightKind;
            vec3 candidateLightPosition;
            vec3 candidateLightRadiance;
            vec3 candidateLightNormal;
            if (!sample_emissive_light(
                    rng,
                    hit.world_pos,
                    candidateWi,
                    candidateEmission,
                    candidateLightPdf,
                    candidateDistance,
                    candidateLightIndex,
                    candidateLightKind,
                    candidateLightPosition,
                    candidateLightRadiance,
                    candidateLightNormal)) {
                continue;
            }
            float candidateCos = max(dot(lightingNormal, candidateWi), 0.0);
            if (candidateCos <= 0.0 || candidateLightPdf <= 1.0e-6) {
                continue;
            }
            vec3 candidateBsdf = eval_brdf(material, wo, candidateWi, lightingNormal, hit.tangent, hit.bitangent);
            float candidateBsdfPdf = pdf_brdf(material, wo, candidateWi, lightingNormal, hit.tangent, hit.bitangent);
            vec3 candidateEstimate = candidateBsdf * candidateEmission * candidateCos / max(candidateLightPdf, 1.0e-6);
            float candidateProxy = max(dot(candidateEstimate, vec3(0.2126, 0.7152, 0.0722)), 0.0);
            proxyWeightSum += candidateProxy;
            if (candidateProxy > 0.0 && rand_f32(rng) * proxyWeightSum <= candidateProxy) {
                selectedWi = candidateWi;
                selectedEmission = candidateEmission;
                selectedBsdf = candidateBsdf;
                selectedLightPdf = candidateLightPdf;
                selectedBsdfPdf = candidateBsdfPdf;
                selectedDistance = candidateDistance;
                selectedLightIndex = candidateLightIndex;
                selectedLightKind = candidateLightKind;
                selectedLightPosition = candidateLightPosition;
                selectedLightRadiance = candidateLightRadiance;
                selectedLightNormal = candidateLightNormal;
                selectedProxy = candidateProxy;
                selectedCandidate = true;
            }
        }
    }
    if (selectedCandidate && selectedProxy > 0.0) {
        float shadowDistance = max(selectedDistance - shadow_distance_bias(), 0.0);
        uint transmissiveHits;
        uint visiblePath;
        uint blockedPath;
        record_rt_counter(RT_DIAG_EMISSIVE_DIRECT_SHADOW_RAYS);
        vec3 shadowT = direct_shadow_transmittance_stats(
            shadow_origin(hit, selectedWi),
            selectedWi,
            shadowDistance,
            rayTime,
            material,
            transmissiveHits,
            visiblePath,
            blockedPath);
        causticTransmissiveHits += transmissiveHits;
        causticVisiblePaths += visiblePath;
        causticBlockedPaths += blockedPath;
        float cosSurface = max(dot(lightingNormal, selectedWi), 0.0);
        if (luminance(shadowT) > 0.0 && cosSurface > 0.0) {
            float effectiveLightPdf = selectedLightPdf;
            if (risCandidateCount > 1u && proxyWeightSum > 1.0e-8 && selectedProxy > 1.0e-8) {
                effectiveLightPdf = selectedLightPdf * float(risCandidateCount) * selectedProxy / proxyWeightSum;
            }
            float weight = power_heuristic(effectiveLightPdf, selectedBsdfPdf);
            sampledLightPdf = effectiveLightPdf;
            sampledRawLightPdf = selectedLightPdf;
            sampledEffectiveLightPdf = effectiveLightPdf;
            sampledBsdfPdf = selectedBsdfPdf;
            sampledMisWeight = weight;
            sampledType = 1u;
            sampledLightIndex = selectedLightIndex;
            sampledLightKind = selectedLightKind;
            sampledLightPosition = selectedLightPosition;
            sampledLightDistance = selectedDistance;
            sampledLightDirection = selectedWi;
            sampledLightRadiance = selectedLightRadiance;
            sampledLightNormal = selectedLightNormal;
            emissiveContribution = selectedBsdf * selectedEmission * shadowT * cosSurface * weight / max(effectiveLightPdf, 1e-6);
        }
    }

    uint envSampleCount = clamp(camera.environment_direct_samples, 1u, 8u);
    for (uint envSample = 0u; envSample < envSampleCount; ++envSample) {
        vec3 envDir;
        float envPdf;
        vec3 envRadiance = sample_environment_direction(rng, envDir, envPdf);
        if (envPdf <= 0.0) {
            continue;
        }
        float cosSurface = max(dot(lightingNormal, envDir), 0.0);
        if (cosSurface <= 0.0) {
            continue;
        }
        uint transmissiveHits;
        uint visiblePath;
        uint blockedPath;
        record_rt_counter(RT_DIAG_ENV_DIRECT_SHADOW_RAYS);
        vec3 shadowT = direct_shadow_transmittance_stats(
            shadow_origin(hit, envDir),
            envDir,
            10000.0,
            rayTime,
            material,
            transmissiveHits,
            visiblePath,
            blockedPath);
        causticTransmissiveHits += transmissiveHits;
        causticVisiblePaths += visiblePath;
        causticBlockedPaths += blockedPath;
        if (luminance(shadowT) > 0.0) {
            vec3 bsdf = eval_brdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent);
            float bsdfPdf = pdf_brdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent);
            float weight = power_heuristic(envPdf, bsdfPdf);
            if (sampledLightPdf <= 0.0) {
                sampledLightPdf = envPdf;
                sampledRawLightPdf = envPdf;
                sampledEffectiveLightPdf = envPdf;
                sampledBsdfPdf = bsdfPdf;
                sampledMisWeight = weight;
                sampledType = 2u;
                sampledLightDirection = envDir;
                sampledLightDistance = 10000.0;
            }
            environmentContribution += bsdf * envRadiance * shadowT * cosSurface * weight / max(envPdf, 1e-6);
        }
    }
    environmentContribution /= float(envSampleCount);

    vec3 sunContribution = vec3(0.0);
    {
        vec3 sunWi;
        vec3 sunRadiance;
        float sunPdf;
        if (sample_sun_light(rng, sunWi, sunRadiance, sunPdf)) {
            float ndl = max(dot(lightingNormal, sunWi), 0.0);
            if (ndl <= 0.0) {
                return emissiveContribution + environmentContribution;
            }
            uint transmissiveHits;
            uint visiblePath;
            uint blockedPath;
            record_rt_counter(RT_DIAG_SUN_DIRECT_SHADOW_RAYS);
            vec3 shadowT = direct_shadow_transmittance_stats(
                shadow_origin(hit, sunWi),
                sunWi,
                10000.0,
                rayTime,
                material,
                transmissiveHits,
                visiblePath,
                blockedPath);
            causticTransmissiveHits += transmissiveHits;
            causticVisiblePaths += visiblePath;
            causticBlockedPaths += blockedPath;
            if (luminance(shadowT) > 0.0) {
                vec3 bsdf = eval_brdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent);
                float bsdfPdf = pdf_brdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent);
                float weight = power_heuristic(sunPdf, bsdfPdf);
                if (sampledLightPdf <= 0.0) {
                    sampledLightPdf = sunPdf;
                    sampledRawLightPdf = sunPdf;
                    sampledEffectiveLightPdf = sunPdf;
                    sampledBsdfPdf = bsdfPdf;
                    sampledMisWeight = weight;
                    sampledType = 3u;
                    sampledLightDirection = sunWi;
                    sampledLightDistance = 10000.0;
                }
                sunContribution = bsdf * sunRadiance * sun_transmittance(hit.world_pos, sunWi) * shadowT * ndl * weight / max(sunPdf, 1e-6);
            }
        }
    }

    return emissiveContribution + environmentContribution + sunContribution;
}

#if RTV_NATIVE2B_COMPACT_PRIMARY_LIGHTS
vec3 estimate_direct_lighting_env_sun_only(
    inout uint rng,
    RayPayload hit,
    Material material,
    vec3 wo,
    float rayTime,
    out vec3 environmentContribution,
    out float sampledLightPdf,
    out float sampledRawLightPdf,
    out float sampledEffectiveLightPdf,
    out float sampledBsdfPdf,
    out float sampledMisWeight,
    out uint sampledType,
    out vec3 sampledLightDirection,
    out float sampledLightDistance,
    out uint causticTransmissiveHits,
    out uint causticVisiblePaths,
    out uint causticBlockedPaths) {
    environmentContribution = vec3(0.0);
    sampledLightPdf = 0.0;
    sampledRawLightPdf = 0.0;
    sampledEffectiveLightPdf = 0.0;
    sampledBsdfPdf = 0.0;
    sampledMisWeight = 0.0;
    sampledType = 0u;
    sampledLightDirection = vec3(0.0, 1.0, 0.0);
    sampledLightDistance = 0.0;
    causticTransmissiveHits = 0u;
    causticVisiblePaths = 0u;
    causticBlockedPaths = 0u;
    bool isDelta = material_is_delta(material);
    if (camera.direct_lighting_enabled == 0u || isDelta || debug_params.view == 27u) {
        return vec3(0.0);
    }

    vec3 lightingNormal = normalize(hit.geom_normal);
    uint envSampleCount = clamp(camera.environment_direct_samples, 1u, 8u);
    for (uint envSample = 0u; envSample < envSampleCount; ++envSample) {
        vec3 envDir;
        float envPdf;
        vec3 envRadiance = sample_environment_direction(rng, envDir, envPdf);
        if (envPdf <= 0.0) {
            continue;
        }
        float cosSurface = max(dot(lightingNormal, envDir), 0.0);
        if (cosSurface <= 0.0) {
            continue;
        }
        uint transmissiveHits;
        uint visiblePath;
        uint blockedPath;
        record_rt_counter(RT_DIAG_ENV_DIRECT_SHADOW_RAYS);
        vec3 shadowT = direct_shadow_transmittance_stats(
            shadow_origin(hit, envDir),
            envDir,
            10000.0,
            rayTime,
            material,
            transmissiveHits,
            visiblePath,
            blockedPath);
        causticTransmissiveHits += transmissiveHits;
        causticVisiblePaths += visiblePath;
        causticBlockedPaths += blockedPath;
        if (luminance(shadowT) > 0.0) {
            vec3 bsdf = eval_brdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent);
            float bsdfPdf = pdf_brdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent);
            float weight = power_heuristic(envPdf, bsdfPdf);
            if (sampledLightPdf <= 0.0) {
                sampledLightPdf = envPdf;
                sampledRawLightPdf = envPdf;
                sampledEffectiveLightPdf = envPdf;
                sampledBsdfPdf = bsdfPdf;
                sampledMisWeight = weight;
                sampledType = 2u;
                sampledLightDirection = envDir;
                sampledLightDistance = 10000.0;
            }
            environmentContribution += bsdf * envRadiance * shadowT * cosSurface * weight / max(envPdf, 1e-6);
        }
    }
    environmentContribution /= float(envSampleCount);

    vec3 sunContribution = vec3(0.0);
    vec3 sunWi;
    vec3 sunRadiance;
    float sunPdf;
    if (sample_sun_light(rng, sunWi, sunRadiance, sunPdf)) {
        float ndl = max(dot(lightingNormal, sunWi), 0.0);
        if (ndl <= 0.0) {
            return environmentContribution;
        }
        uint transmissiveHits;
        uint visiblePath;
        uint blockedPath;
        record_rt_counter(RT_DIAG_SUN_DIRECT_SHADOW_RAYS);
        vec3 shadowT = direct_shadow_transmittance_stats(
            shadow_origin(hit, sunWi),
            sunWi,
            10000.0,
            rayTime,
            material,
            transmissiveHits,
            visiblePath,
            blockedPath);
        causticTransmissiveHits += transmissiveHits;
        causticVisiblePaths += visiblePath;
        causticBlockedPaths += blockedPath;
        if (luminance(shadowT) > 0.0) {
            vec3 bsdf = eval_brdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent);
            float bsdfPdf = pdf_brdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent);
            float weight = power_heuristic(sunPdf, bsdfPdf);
            if (sampledLightPdf <= 0.0) {
                sampledLightPdf = sunPdf;
                sampledRawLightPdf = sunPdf;
                sampledEffectiveLightPdf = sunPdf;
                sampledBsdfPdf = bsdfPdf;
                sampledMisWeight = weight;
                sampledType = 3u;
                sampledLightDirection = sunWi;
                sampledLightDistance = 10000.0;
            }
            sunContribution = bsdf * sunRadiance * sun_transmittance(hit.world_pos, sunWi) * shadowT * ndl * weight / max(sunPdf, 1e-6);
        }
    }

    return environmentContribution + sunContribution;
}
#endif

bool native2b_terminal_direct_fast_supported(Material material) {
    if (!native2b_kernel_enabled()) {
        return false;
    }

    bool supported = true;
    if ((camera.path_trace_controls.z & 8u) == 0u) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_FLAG_DISABLED);
        supported = false;
    }
    bool terminalCanSkipImportedEmissiveNee =
        compact_imported_emissive_direct_enabled() &&
        mesh_params.authored_light_count == 0u;
    if ((mesh_params.light_count != 0u || mesh_params.emissive_total_area > 1.0e-8) &&
        !terminalCanSkipImportedEmissiveNee) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_SCENE_LIGHTS);
        supported = false;
    }
    if (mesh_params.transmissive_shadow_caster_count != 0u) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_TRANSMISSIVE_SCENE);
    }
    if (homogeneous_volume_enabled()) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_VOLUME);
        supported = false;
    }
    if (camera.path_trace_controls.w != 0u || debug_params.view == 27u) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_DEBUG);
        supported = false;
    }
    if (material_is_transmissive(material)) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_MATERIAL_TRANSMISSIVE);
        supported = false;
    }
    return supported;
}

bool native2b_terminal_direct_tile_selected(float probability) {
    if (probability >= 0.999) {
        return true;
    }
    if (probability <= 0.0) {
        return false;
    }

    // Raygen launches are X-major on the target NVIDIA path. Sharing one
    // decision across 32 adjacent pixels keeps the expensive shadow branch
    // coherent while the frame term rotates coverage temporally.
    uint warpTile = gl_LaunchIDEXT.x >> 5u;
    uint tileSeed = warpTile * 0x9e3779b9u;
    tileSeed ^= gl_LaunchIDEXT.y * 0x85ebca6bu;
    tileSeed ^= camera.temporal_frame_index * 0xc2b2ae35u;
    float selector = float(pcg_hash(tileSeed) >> 8u) * (1.0 / 16777216.0);
    return selector < probability;
}

bool native2b_terminal_simple_direct_brdf_supported(Material material) {
    if (material_is_transmissive(material)) {
        return false;
    }
    if (material.clearcoat_factor > 1.0e-5 ||
        dot(max(material.sheen_color, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)) > 1.0e-5) {
        return false;
    }
    return material.mat_type == 0u || material.mat_type == 1u || material.mat_type == 3u;
}

void native2b_terminal_direct_brdf_pdf(
    Material material,
    vec3 wo,
    vec3 wi,
    vec3 n,
    vec3 tangent,
    vec3 bitangent,
    out vec3 bsdf,
    out float bsdfPdf) {
    if (native2b_terminal_simple_direct_brdf_supported(material)) {
        if (material.mat_type == 0u) {
            bsdf = eval_diffuse_brdf(material, wo, wi, n);
            bsdfPdf = diffuse_pdf(n, wi);
        } else {
            bsdf = eval_ggx_brdf(material, wo, wi, n, tangent, bitangent);
            bsdfPdf = pdf_pbr_brdf(material, wo, wi, n, tangent, bitangent);
        }
        return;
    }

    bsdf = eval_brdf(material, wo, wi, n, tangent, bitangent);
    bsdfPdf = pdf_brdf(material, wo, wi, n, tangent, bitangent);
}

vec3 estimate_native2b_terminal_env_sun_direct(
    inout uint rng,
    RayPayload hit,
    Material material,
    vec3 wo,
    float rayTime,
    out vec3 environmentContribution,
    out uint sampledType) {
    environmentContribution = vec3(0.0);
    sampledType = 0u;
    if (camera.direct_lighting_enabled == 0u || material_is_delta(material) || debug_params.view == 27u) {
        return vec3(0.0);
    }

    vec3 lightingNormal = normalize(hit.geom_normal);
    const float relaxedTerminalDirectProbability = clamp(camera.native2b_controls.x, 0.0, 1.0);
    const bool relaxedTerminalDirectSampling = relaxedTerminalDirectProbability < 0.999;
    const bool risOneSample = native2b_direct_reuse_mode() == NATIVE2B_DIRECT_REUSE_RIS;
    const bool environmentActive = env_params.enabled != 0u;
    const bool sunActive = camera.sunlight_enabled != 0u;
    if (relaxedTerminalDirectSampling &&
        !native2b_terminal_direct_tile_selected(relaxedTerminalDirectProbability)) {
        return vec3(0.0);
    }
    float environmentTechniqueProbability = 1.0;
    float sunTechniqueProbability = 1.0;
    bool sampleEnvironmentTechnique = environmentActive;
    bool sampleSunTechnique = sunActive;
    if (risOneSample) {
        if (environmentActive && sunActive) {
            environmentTechniqueProbability = 0.5;
            sunTechniqueProbability = 0.5;
            sampleEnvironmentTechnique = rand_f32(rng) < environmentTechniqueProbability;
            sampleSunTechnique = !sampleEnvironmentTechnique;
        } else {
            environmentTechniqueProbability = environmentActive ? 1.0 : 0.0;
            sunTechniqueProbability = sunActive ? 1.0 : 0.0;
        }
    }

    uint envSampleCount = clamp(camera.environment_direct_samples, 1u, 8u);
    uint effectiveEnvSampleCount = risOneSample ? 1u : envSampleCount;
    for (uint envSample = 0u; envSample < effectiveEnvSampleCount && sampleEnvironmentTechnique; ++envSample) {
        vec3 envDir;
        float envPdf;
        vec3 envRadiance = sample_environment_direction(rng, envDir, envPdf);
        if (envPdf <= 0.0) {
            continue;
        }
        float cosSurface = max(dot(lightingNormal, envDir), 0.0);
        if (cosSurface <= 0.0) {
            continue;
        }

        record_rt_counter(RT_DIAG_ENV_DIRECT_SHADOW_RAYS);
        bool blocked = trace_shadow(shadow_origin(hit, envDir), envDir, 10000.0, rayTime);
        if (blocked) {
            continue;
        }

        vec3 bsdf;
        float bsdfPdf;
        native2b_terminal_direct_brdf_pdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent, bsdf, bsdfPdf);
        float techniqueProbability = risOneSample ? environmentTechniqueProbability : 1.0;
        float samplingProbability = relaxedTerminalDirectSampling ? relaxedTerminalDirectProbability : 1.0;
        float effectiveEnvPdf = envPdf * techniqueProbability * samplingProbability;
        float weight = power_heuristic(effectiveEnvPdf, bsdfPdf);
        if (sampledType == 0u) {
            sampledType = 2u;
        }
        environmentContribution += bsdf * envRadiance * cosSurface * weight /
            max(effectiveEnvPdf, 1e-6);
    }
    environmentContribution /= float(effectiveEnvSampleCount);

    vec3 sunContribution = vec3(0.0);
    vec3 sunWi;
    vec3 sunRadiance;
    float sunPdf;
    if (sampleSunTechnique && sample_sun_light(rng, sunWi, sunRadiance, sunPdf)) {
        float ndl = max(dot(lightingNormal, sunWi), 0.0);
        if (ndl > 0.0) {
            record_rt_counter(RT_DIAG_SUN_DIRECT_SHADOW_RAYS);
            bool blocked = trace_shadow(shadow_origin(hit, sunWi), sunWi, 10000.0, rayTime);
            if (!blocked) {
                vec3 bsdf;
                float bsdfPdf;
                native2b_terminal_direct_brdf_pdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent, bsdf, bsdfPdf);
                float techniqueProbability = risOneSample ? sunTechniqueProbability : 1.0;
                float samplingProbability = relaxedTerminalDirectSampling ? relaxedTerminalDirectProbability : 1.0;
                float effectiveSunPdf = sunPdf * techniqueProbability * samplingProbability;
                float weight = power_heuristic(effectiveSunPdf, bsdfPdf);
                if (sampledType == 0u) {
                    sampledType = 3u;
                }
                sunContribution = bsdf * sunRadiance * sun_transmittance(hit.world_pos, sunWi) * ndl * weight /
                    max(effectiveSunPdf, 1e-6);
            }
        }
    }

    return environmentContribution + sunContribution;
}

float restir_di_light_pdf(uint lightIndex, uint lightKind, vec3 receiverPos, vec3 samplePosOrDir, vec3 lightNormal, float distanceToLight) {
    if (lightIndex >= mesh_params.light_count || mesh_params.emissive_total_area <= 1.0e-8) {
        return 0.0;
    }
    LightRecord light = light_records[lightIndex];
    if (light.metadata.x != lightKind) {
        return 0.0;
    }
    float selectionPdf = light_record_selection_pdf(lightIndex);
    if (lightKind == 2u || lightKind == 3u || lightKind == 5u) {
        return selectionPdf;
    }

    vec3 toLight = samplePosOrDir - receiverPos;
    float distSq = dot(toLight, toLight);
    vec3 wi = toLight * inversesqrt(max(distSq, 1.0e-8));
    float cosLight = max(dot(-wi, normalize(lightNormal)), 0.0);
    float area = max(light.data0.w, 1.0e-6);
    return cosLight > 1.0e-6 ? selectionPdf * distSq / (cosLight * area) : 0.0;
}

RestirReservoir invalidate_restir_di_reservoir(RestirReservoir reservoir) {
    reservoir.metadata.x = 0u;
    restir_set_validity_visibility(reservoir, restir_pack_validity_visibility(false, RESTIR_VISIBILITY_INVALID));
    reservoir.sample_value_confidence = vec4(0.0);
    reservoir.sample_radiance_target.w = 0.0;
    reservoir.sample_normal_weight.w = 0.0;
    restir_set_source_pdf_and_previous_weight(reservoir, 1.0e-6, restir_previous_weight(reservoir));
    return reservoir;
}

RestirReservoir reevaluate_restir_di_reservoir(
    RestirReservoir reservoir,
    RayPayload hit,
    Material material,
    vec3 wo,
    vec3 throughput,
    vec3 atmosTransmittance,
    float rayTime,
    out uint causticTransmissiveHits,
    out uint causticVisiblePaths,
    out uint causticBlockedPaths) {
    causticTransmissiveHits = 0u;
    causticVisiblePaths = 0u;
    causticBlockedPaths = 0u;
    if (!restir_reservoir_valid(reservoir) || reservoir.metadata.x != 1u) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    uint lightIndex = reservoir.sample_metadata.x;
    uint lightKind = reservoir.sample_metadata.y;
    if (lightIndex >= mesh_params.light_count) {
        return invalidate_restir_di_reservoir(reservoir);
    }
    LightRecord selectedLight = light_records[lightIndex];
    if (selectedLight.metadata.x != lightKind) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    vec3 lightingNormal = normalize(hit.geom_normal);
    vec3 wi;
    float distanceToLight;
    vec3 incidentRadiance = max(reservoir.sample_radiance_target.rgb, vec3(0.0));
    vec3 lightNormal = normalize(reservoir.sample_normal_weight.xyz);
    if (lightKind == 2u) {
        wi = normalize(reservoir.sample_position_distance.xyz);
        distanceToLight = 10000.0;
    } else {
        vec3 toLight = reservoir.sample_position_distance.xyz - hit.world_pos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1.0e-8));
        wi = toLight / distanceToLight;
        if (lightKind == 3u) {
            incidentRadiance /= max(distSq, 1.0e-4);
            lightNormal = -wi;
        } else if (lightKind == 5u) {
            vec3 spotDirection = normalize(vec3(selectedLight.data1.w, selectedLight.data2.w, selectedLight.data3.x));
            float innerCone = clamp(selectedLight.data3.y, 0.0, PI);
            float outerCone = clamp(max(selectedLight.data3.z, innerCone), 0.0, PI);
            float cosInner = cos(innerCone);
            float cosOuter = cos(outerCone);
            float cosTheta = dot(-wi, spotDirection);
            float coneWeight = clamp((cosTheta - cosOuter) / max(cosInner - cosOuter, 1.0e-4), 0.0, 1.0);
            coneWeight *= coneWeight;
            incidentRadiance = selectedLight.data2.xyz * coneWeight / max(distSq, 1.0e-4);
            lightNormal = spotDirection;
        }
    }

    float cosSurface = max(dot(lightingNormal, wi), 0.0);
    float lightPdf = restir_di_light_pdf(lightIndex, lightKind, hit.world_pos, reservoir.sample_position_distance.xyz, lightNormal, distanceToLight);
    if (cosSurface <= 0.0 || lightPdf <= 1.0e-6 || !has_positive_radiance(incidentRadiance)) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    float shadowDistance = lightKind == 2u ? 10000.0 : max(distanceToLight - shadow_distance_bias(), 0.0);
    vec3 shadowT = caustic_shadow_transmittance_stats(
        shadow_origin(hit, wi),
        wi,
        shadowDistance,
        rayTime,
        causticTransmissiveHits,
        causticVisiblePaths,
        causticBlockedPaths);
    if (luminance(shadowT) <= 0.0) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    vec3 bsdf = eval_brdf(material, wo, wi, lightingNormal, hit.tangent, hit.bitangent);
    float bsdfPdf = pdf_brdf(material, wo, wi, lightingNormal, hit.tangent, hit.bitangent);
    float misWeight = power_heuristic(lightPdf, bsdfPdf);
    vec3 contribution = throughput * bsdf * incidentRadiance * shadowT * cosSurface * misWeight / max(lightPdf, 1.0e-6);
    contribution *= atmosTransmittance;
    contribution = clamp_luminance_preserve_hue(contribution, direct_contribution_luminance_limit(0u, 1u, material.roughness));
    float targetLum = luminance(contribution);
    if (targetLum <= 1.0e-6) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    reservoir.sample_value_confidence.rgb = contribution;
    reservoir.sample_value_confidence.a = clamp(reservoir.sample_value_confidence.a, 0.0, 1.0);
    reservoir.sample_position_distance.w = distanceToLight;
    reservoir.sample_radiance_target.w = targetLum;
    reservoir.sample_normal_weight.xyz = lightNormal;
    reservoir.sample_normal_weight.w = max(targetLum / max(lightPdf, 1.0e-6), 1.0e-6);
    restir_set_source_pdf_and_previous_weight(reservoir, lightPdf, restir_previous_weight(reservoir));
    restir_set_validity_visibility(reservoir, restir_pack_validity_visibility(true, RESTIR_VISIBILITY_VISIBLE));
    return reservoir;
}

RestirReservoir restir_temporal_resample(RestirReservoir current, RestirReservoir previous, float motionConfidence, inout uint rng) {
    bool currentValid = restir_reservoir_valid(current);
    bool previousValid = restir_reservoir_valid(previous);
    if (!currentValid && !previousValid) {
        return current;
    }
    if (!previousValid) {
        restir_set_source_pdf_and_previous_weight(current, restir_source_pdf(current), 0.0);
        restir_set_age(current, 0u);
        return current;
    }
    if (!currentValid) {
        float reuseConfidence = clamp(previous.sample_value_confidence.a, 0.0, 1.0) *
            clamp(motionConfidence, 0.0, 1.0) *
            restir_age_confidence(previous, 32.0);
        if (reuseConfidence <= 0.20) {
            return current;
        }
        previous.sample_value_confidence.a = reuseConfidence;
        restir_set_age(previous, min(restir_age(previous) + 1u, 255u));
        restir_set_source_pdf_and_previous_weight(previous, restir_source_pdf(previous), 1.0);
        return previous;
    }

    float currentMass = max(restir_weight_sum(current), restir_target_function(current));
    float previousMass = max(restir_weight_sum(previous), restir_target_function(previous)) *
        clamp(previous.sample_value_confidence.a, 0.0, 1.0) *
        clamp(motionConfidence, 0.0, 1.0) *
        restir_age_confidence(previous, 32.0);
    float totalMass = currentMass + previousMass;
    float previousWeight = totalMass > 1.0e-8 ? previousMass / totalMass : 0.0;
    float motionCap = mix(0.85, 0.10, 1.0 - clamp(motionConfidence, 0.0, 1.0));
    previousWeight = clamp(previousWeight, 0.0, motionCap);

    bool selectPrevious = rand_f32(rng) < previousWeight;
    RestirReservoir selected = selectPrevious ? previous : current;
    selected.sample_normal_weight.w = min(totalMass, 65504.0);
    selected.sample_value_confidence.a = clamp(selected.sample_value_confidence.a * clamp(motionConfidence, 0.0, 1.0), 0.0, 1.0);
    restir_set_age(selected, selectPrevious ? min(restir_age(previous) + 1u, 255u) : 0u);
    restir_set_sample_count(selected, min(restir_sample_count(current) + restir_sample_count(previous), 64.0));
    restir_set_source_pdf_and_previous_weight(selected, restir_source_pdf(selected), previousWeight);
    restir_set_validity_visibility(selected, restir_pack_validity_visibility(restir_reservoir_valid(selected), RESTIR_VISIBILITY_VISIBLE));
    return selected;
}

vec3 fast_preview_ambient(Material material, vec3 normal) {
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 sampleDir = normalize(normal + up * 0.45);
    float hemisphere = 0.35 + 0.65 * max(dot(normal, up), 0.0);
    return material.color * environment_radiance(sampleDir, ATMOSPHERE_RAY_QUALITY_FAST) * (0.16 + 0.24 * hemisphere);
}

vec3 estimate_volume_direct_lighting(inout uint rng, vec3 scatterPos, vec3 incidentDir, float rayTime) {
    if (camera.direct_lighting_enabled == 0u || !homogeneous_volume_enabled()) {
        return vec3(0.0);
    }

    float g = homogeneous_anisotropy();
    vec3 result = vec3(0.0);
    uint envSampleCount = clamp(camera.environment_direct_samples, 1u, 8u);
    for (uint envSample = 0u; envSample < envSampleCount; ++envSample) {
        vec3 envDir;
        float envPdf;
        vec3 envRadiance = sample_environment_direction(rng, envDir, envPdf);
        if (envPdf <= 1.0e-6) {
            continue;
        }
        vec3 shadowT = caustic_shadow_transmittance(scatterPos, envDir, 10000.0, rayTime);
        if (luminance(shadowT) > 0.0) {
            float phase = henyey_greenstein_phase(dot(normalize(incidentDir), envDir), g);
            result += envRadiance * shadowT * phase / max(envPdf, 1.0e-6);
        }
    }
    result /= float(envSampleCount);

    vec3 sunWi;
    vec3 sunRadiance;
    float sunPdf;
    if (camera.sunlight_enabled != 0u && sample_sun_light(rng, sunWi, sunRadiance, sunPdf) && sunPdf > 1.0e-6) {
        vec3 shadowT = caustic_shadow_transmittance(scatterPos, sunWi, 10000.0, rayTime);
        if (luminance(shadowT) > 0.0) {
            float phase = henyey_greenstein_phase(dot(normalize(incidentDir), sunWi), g);
            result += sunRadiance * sun_transmittance(scatterPos, sunWi) * shadowT * phase / max(sunPdf, 1.0e-6);
        }
    }
    return result;
}


#endif // RTV_PATHTRACE_LIGHTING_GLSL
