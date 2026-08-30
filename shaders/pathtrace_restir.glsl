#ifndef RTV_PATHTRACE_RESTIR_GLSL
#define RTV_PATHTRACE_RESTIR_GLSL

// ReSTIR DI/GI ray-generation integration; include after camera helpers.
RestirReservoir empty_restir_reservoir() {
    RestirReservoir reservoir;
    reservoir.metadata = uvec4(0u);
    reservoir.sample_value_confidence = vec4(0.0);
    reservoir.sample_position_distance = vec4(0.0);
    reservoir.sample_radiance_target = vec4(0.0);
    reservoir.sample_normal_weight = vec4(0.0);
    reservoir.sample_metadata = uvec4(0u);
    restir_set_source_pdf_and_previous_weight(reservoir, 1.0e-6, 0.0);
    return reservoir;
}

RestirReservoir make_initial_restir_reservoir(
    vec3 directLight,
    float lightPdf,
    uint sampleType,
    float roughness,
    uint materialId,
    uint instanceId,
    uint lightIndex,
    uint lightKind,
    vec3 samplePosition,
    float sampleDistance,
    vec3 sampleRadiance,
    vec3 sampleNormal) {
    bool hasCandidate = sampleType != 0u && lightPdf > 1.0e-6;
    vec3 sampleValue = hasCandidate ? max(directLight, vec3(0.0)) : vec3(0.0);
    sampleValue = clamp_luminance_preserve_hue(
        sampleValue,
        direct_contribution_luminance_limit(0u, sampleType, roughness));
    float targetLum = luminance(sampleValue);
    bool valid = hasCandidate && targetLum > 1.0e-6 && lightIndex < mesh_params.light_count;
    RestirReservoir current;
    current.metadata = uvec4(
        valid ? sampleType : 0u,
        0u,
        restir_pack_state(0u, restir_pack_validity_visibility(valid, RESTIR_VISIBILITY_VISIBLE), valid ? 1u : 0u),
        restir_temporal_signature(max(sampleType, 1u), roughness, materialId, instanceId));
    restir_set_source_pdf_and_previous_weight(current, valid ? lightPdf : 1.0e-6, 0.0);
    current.sample_value_confidence = vec4(sampleValue, valid ? 1.0 : 0.0);
    current.sample_position_distance = vec4(samplePosition, valid ? sampleDistance : 0.0);
    current.sample_radiance_target = vec4(valid ? max(sampleRadiance, vec3(0.0)) : vec3(0.0), valid ? targetLum : 0.0);
    current.sample_normal_weight = vec4(valid ? normalize(sampleNormal) : vec3(0.0, 1.0, 0.0), valid ? targetLum / max(lightPdf, 1.0e-6) : 0.0);
    current.sample_metadata = uvec4(valid ? lightIndex : 0u, valid ? lightKind : 0u, materialId, instanceId);
    return current;
}

void store_initial_restir_reservoir(uint pixelIndex, ivec2 coords, ivec2 dims, uint packedVelocity, PathComponents components) {
    if (restir_mode() == 0u) {
        return;
    }

    RestirReservoir current = make_initial_restir_reservoir(
        components.restir_initial_direct_light,
        components.first_light_pdf,
        components.first_direct_sample_type,
        components.first_roughness,
        components.first_material_id,
        components.instance_id,
        components.restir_di_light_index,
        components.restir_di_light_kind,
        components.restir_di_sample_position,
        components.restir_di_sample_distance,
        components.restir_di_sample_radiance,
        components.restir_di_sample_normal);

    restir_reservoirs[pixelIndex] = current;
}

void store_new_restir_di(uint pixelIndex, PathComponents components, vec3 hit_position, vec3 hit_normal, float hit_depth, bool did_hit) {
    // Checkerboard DI owns only one field per frame. The other full-resolution
    // pixel remains available to final shading through the initial reservoir.
    if (!restir_di_checkerboard_pixel_active(ivec2(gl_LaunchIDEXT.xy))) {
        return;
    }
    pixelIndex = restir_di_reservoir_index(ivec2(gl_LaunchIDEXT.xy), gl_LaunchSizeEXT.x);
    if (restir_di_raygen_params.counterEnabled != 0u) {
        atomicAdd(restir_di_counters[RESTIR_DI_COUNTER_INITIAL_PIXELS], 1u);
    }
    // Write receiver surface data
    RestirDiReceiver receiver;
    if (did_hit) {
        uint surfaceFlags = 0u;
        if ((components.first_mat_type == 1u &&
             components.first_roughness < MATERIAL_DELTA_ROUGHNESS_THRESHOLD) ||
            components.first_mat_type == 2u) {
            surfaceFlags |= RESTIR_DI_SURFACE_DELTA;
        }
        if (components.first_transmission > 1.0e-4) {
            surfaceFlags |= RESTIR_DI_SURFACE_DELTA;
        }
        if (components.first_mat_type == 5u) {
            surfaceFlags |= RESTIR_DI_SURFACE_UNLIT;
        }
        if (components.restir_di_material_pbr != 0u) {
            surfaceFlags |= RESTIR_DI_SURFACE_PBR;
        }
        if (components.restir_di_material_supported == 0u) {
            surfaceFlags |= RESTIR_DI_SURFACE_UNSUPPORTED;
        }
        receiver.worldPosition_depth = vec4(hit_position, hit_depth);
        receiver.normal_roughness = vec4(normalize(hit_normal), components.first_roughness);
#if RTV_RESTIR_DI_VALIDATION_FULL
        receiver.tangent_materialId = vec4(components.restir_di_base_color, float(components.first_material_id));
        receiver.bitangent_instanceId = vec4(components.restir_di_f0, float(components.instance_id));
        receiver.viewDirection_hitDist = vec4(normalize(components.first_view_direction), hit_depth);
        receiver.primitive_mesh_flags = uvec4(
            components.primitive_id,
            components.mesh_id,
            surfaceFlags,
            floatBitsToUint(clamp(components.first_metallic, 0.0, 1.0)));
#else
        uint surfaceSignature = components.first_material_id * 747796405u ^
            components.instance_id * 2891336453u;
        receiver.packedMaterialSurface = uvec4(
            packUnorm4x8(vec4(clamp(components.restir_di_base_color, 0.0, 1.0), clamp(components.first_metallic, 0.0, 1.0))),
            packUnorm4x8(vec4(clamp(components.restir_di_f0, 0.0, 1.0), float(surfaceFlags) / 255.0)),
            surfaceSignature,
            packSnorm2x16(restir_di_oct_encode(normalize(components.first_view_direction))));
#endif
    } else {
        receiver.worldPosition_depth = vec4(0.0, 0.0, 0.0, 9999.0);
        receiver.normal_roughness = vec4(0.0, 1.0, 0.0, 1.0);
#if RTV_RESTIR_DI_VALIDATION_FULL
        receiver.tangent_materialId = vec4(1.0, 0.0, 0.0, float(0xffffffffu));
        receiver.bitangent_instanceId = vec4(0.0, 1.0, 0.0, float(0xffffffffu));
        receiver.viewDirection_hitDist = vec4(0.0, 0.0, 1.0, 9999.0);
        receiver.primitive_mesh_flags = uvec4(0u, 0u, 1u /*RESTIR_DI_SURFACE_SKY*/, 0u);
#else
        receiver.packedMaterialSurface = uvec4(
            packUnorm4x8(vec4(0.0)),
            packUnorm4x8(vec4(0.0, 0.0, 0.0, float(RESTIR_DI_SURFACE_SKY) / 255.0)),
            0xffffffffu,
            packSnorm2x16(vec2(0.0)));
#endif
    }
    restir_di_receivers[pixelIndex] = receiver;

    // Write initial DI reservoir
    RestirDiReservoir initial = restir_di_empty_reservoir();
    // estimate_direct_lighting returns f / p. Recover the reusable integrand f;
    // the reservoir normalization below reconstructs the original NEE estimate.
    vec3 initialIntegrand = components.restir_initial_direct_light * components.first_light_pdf;
    float targetLum = restir_di_target_function(initialIntegrand);
    bool finiteCandidate = !any(isnan(components.restir_initial_direct_light)) &&
        !any(isinf(components.restir_initial_direct_light)) &&
        !any(isnan(components.restir_di_sample_radiance)) &&
        !any(isinf(components.restir_di_sample_radiance)) &&
        !isnan(components.first_light_pdf) && !isinf(components.first_light_pdf) &&
        !any(isnan(initialIntegrand)) && !any(isinf(initialIntegrand)) &&
        !isnan(targetLum) && !isinf(targetLum);
    bool validPdf = components.first_light_pdf > 1.0e-6;
    bool infiniteLightCandidate = restir_di_light_kind_infinite(components.restir_di_light_kind) &&
        components.restir_di_light_index == RESTIR_DI_PSEUDO_LIGHT_INDEX;
    bool validIdentity = infiniteLightCandidate ||
        (components.restir_di_light_index < mesh_params.light_count &&
         components.restir_di_light_index < 0x01000000u);
    float initialWeight = targetLum / max(components.first_light_pdf, 1.0e-6);
    bool validSurface = components.restir_di_material_supported != 0u &&
        components.caustic_transmissive_hits <= 0.0;
    bool proposalAttempted = did_hit && validSurface &&
        camera.direct_lighting_enabled != 0u;
    restir_di_set_m(initial, proposalAttempted ? 1u : 0u);
    if (did_hit && validSurface && targetLum > 1.0e-6 && finiteCandidate && validPdf && validIdentity &&
        !isnan(initialWeight) && !isinf(initialWeight)) {
        vec3 sampleDir = vec3(0.0, 1.0, 0.0);
        vec3 samplePosition = components.restir_di_sample_position;
        if (infiniteLightCandidate) {
            float dirLen2 = dot(components.restir_di_sample_position, components.restir_di_sample_position);
            sampleDir = dirLen2 > 1.0e-8
                ? components.restir_di_sample_position * inversesqrt(dirLen2)
                : vec3(0.0, 1.0, 0.0);
            samplePosition = sampleDir;
        } else {
            vec3 toLight = components.restir_di_sample_position - hit_position;
            float distToLight = length(toLight);
            sampleDir = distToLight > 1.0e-4 ? toLight / distToLight : vec3(0.0, 1.0, 0.0);
        }

        uint identityHash = 0u;
        uint identityVersion = 0u;
        if (infiniteLightCandidate) {
            identityHash = components.restir_di_light_kind == RESTIR_DI_LIGHT_ENVIRONMENT
                ? RESTIR_DI_ENVIRONMENT_ID_HASH
                : RESTIR_DI_SUN_ID_HASH;
            identityVersion = components.restir_di_light_kind == RESTIR_DI_LIGHT_ENVIRONMENT
                ? camera.gi_version_controls.w
                : camera.gi_version_controls.x;
        } else {
            LightRecord identityRecord = light_records[components.restir_di_light_index];
            identityHash = restir_di_identity_hash(identityRecord.identity.xy);
            identityVersion = identityRecord.identity.z;
        }
        initial.sampleMetadata = uvec4(
            identityHash,
            identityVersion,
            (components.restir_di_light_index << 8u) | (components.restir_di_light_kind & 0xffu),
            0u);
        initial.samplePosition_distance = vec4(
            samplePosition,
            components.restir_di_sample_distance);
        restir_di_set_direction(initial, sampleDir);
        restir_di_set_sample_radiance(initial, components.restir_di_sample_radiance);
        restir_di_set_target(initial, targetLum);
        restir_di_set_light_normal(initial, components.restir_di_sample_normal);
#if !RTV_RESTIR_DI_VALIDATION_FULL
        if (restir_di_raygen_params.counterEnabled != 0u && initialWeight > 65504.0) {
            atomicAdd(restir_di_counters[RESTIR_DI_COUNTER_INITIAL_WEIGHT_OVERFLOW], 1u);
        }
#endif
        restir_di_set_weight_sum(initial, initialWeight);
        restir_di_set_valid(initial, true);
        restir_di_set_age(initial, 0u);
        restir_di_set_m(initial, 1u);
        restir_di_set_visibility(initial, RESTIR_DI_VISIBILITY_VISIBLE);
        restir_di_set_source_pdf(initial, components.first_light_pdf);
        restir_di_set_previous_weight(initial, 0.0);
        restir_di_set_confidence(initial, 1.0);
        if (restir_di_raygen_params.counterEnabled != 0u) {
            atomicAdd(restir_di_counters[RESTIR_DI_COUNTER_INITIAL_VALID], 1u);
            uint lightKind = components.restir_di_light_kind;
            uint classCounter = lightKind <= RESTIR_DI_LIGHT_EMISSIVE_SPHERE ? RESTIR_DI_COUNTER_INITIAL_EMISSIVE :
                lightKind == RESTIR_DI_LIGHT_DIRECTIONAL ? RESTIR_DI_COUNTER_INITIAL_DIRECTIONAL :
                lightKind == RESTIR_DI_LIGHT_POINT ? RESTIR_DI_COUNTER_INITIAL_POINT :
                lightKind == RESTIR_DI_LIGHT_AREA ? RESTIR_DI_COUNTER_INITIAL_AREA :
                lightKind == RESTIR_DI_LIGHT_SPOT ? RESTIR_DI_COUNTER_INITIAL_SPOT :
                lightKind == RESTIR_DI_LIGHT_ENVIRONMENT ? RESTIR_DI_COUNTER_INITIAL_ENVIRONMENT :
                lightKind == RESTIR_DI_LIGHT_SUN ? RESTIR_DI_COUNTER_INITIAL_SUN :
                RESTIR_DI_COUNTER_INITIAL_INVALID_IDENTITY;
            atomicAdd(restir_di_counters[classCounter], 1u);
        }
    } else if (restir_di_raygen_params.counterEnabled != 0u) {
        uint invalidCounter = (!did_hit || !validSurface)
            ? RESTIR_DI_COUNTER_INITIAL_INVALID_SURFACE
            : (!finiteCandidate
                ? RESTIR_DI_COUNTER_INITIAL_NON_FINITE
                : (!validPdf
                    ? RESTIR_DI_COUNTER_INITIAL_INVALID_PDF
                    : (!validIdentity
                        ? RESTIR_DI_COUNTER_INITIAL_INVALID_IDENTITY
                        : RESTIR_DI_COUNTER_INITIAL_INVALID_TARGET)));
        atomicAdd(restir_di_counters[invalidCounter], 1u);
    }
    restir_di_initial_reservoirs[pixelIndex] = initial;
}

float restir_gi_target_function(RestirGiReservoir reservoir);

float restir_gi_temporal_compatibility(RestirGiReservoir current, RestirGiReservoir previous, float motionConfidence) {
    uint maxTemporalAge = max(camera.restir_gi_controls.x, 1u);
    if (!restir_gi_reservoir_valid(previous) || restir_gi_age(previous) >= maxTemporalAge) {
        return 0.0;
    }
    if (!restir_gi_visible(previous)) {
        return 0.0;
    }

    uint currentMaterialId = restir_gi_material_id(current);
    if (currentMaterialId != 0xffffffffu && currentMaterialId != restir_gi_material_id(previous)) {
        return 0.0;
    }

    float ageConfidence = 1.0 - clamp(float(restir_gi_age(previous)) / float(maxTemporalAge), 0.0, 1.0);
    if (!restir_gi_reservoir_valid(current)) {
        return clamp(motionConfidence, 0.0, 1.0) * ageConfidence;
    }

    float roughness = max(restir_gi_roughness(current), restir_gi_roughness(previous));
    float normalAgreement = dot(restir_gi_normal(current), restir_gi_normal(previous));
    float normalThreshold = mix(0.92, 0.70, clamp(roughness, 0.0, 1.0));
    if (normalAgreement < normalThreshold) {
        return 0.0;
    }

    float hitDistanceDelta = abs(restir_gi_hit_distance(current) - restir_gi_hit_distance(previous)) /
        max(restir_gi_hit_distance(current), 0.25);
    float hitDistanceConfidence = 1.0 - smoothstep(0.20, 0.85, hitDistanceDelta);

    float currentTarget = max(restir_gi_target_function(current), 1.0e-5);
    float previousTarget = max(restir_gi_target_function(previous), 1.0e-5);
    float targetFloor = 0.01;
    float targetConfidence = sqrt((min(currentTarget, previousTarget) + targetFloor) / (max(currentTarget, previousTarget) + targetFloor));

    return clamp(motionConfidence, 0.0, 1.0) *
        hitDistanceConfidence *
        smoothstep(normalThreshold, 1.0, normalAgreement) *
        targetConfidence *
        ageConfidence;
}

float restir_gi_target_function(RestirGiReservoir reservoir) {
    return max(restir_gi_target_pdf(reservoir), luminance(max(reservoir.radiance_weight_sum.rgb, vec3(0.0))));
}

float restir_gi_reservoir_mass(RestirGiReservoir reservoir) {
    if (!restir_gi_reservoir_valid(reservoir)) {
        return 0.0;
    }
    return max(restir_gi_target_function(reservoir), 0.0) *
        max(restir_gi_sample_count(reservoir), max(reservoir.radiance_weight_sum.w, 1.0));
}

RestirGiReservoir restir_gi_finalize_selected(
    RestirGiReservoir selected,
    float totalMass,
    float sampleCount,
    uint age,
    float roughness) {
    selected.radiance_weight_sum.w = min(max(totalMass, 1.0), 65504.0);
    restir_gi_set_metadata(
        selected,
        uint(clamp(ceil(sampleCount), 1.0, 255.0)),
        min(age, 255u),
        RESTIR_GI_FLAG_VALID | (restir_gi_visible(selected) ? RESTIR_GI_FLAG_VISIBLE : 0u),
        roughness);
    return selected;
}

RestirGiReservoir restir_gi_finalize_blended(
    RestirGiReservoir selected,
    vec3 radiance,
    float totalMass,
    float sampleCount,
    uint age,
    float roughness) {
    selected.radiance_weight_sum.rgb = max(radiance, vec3(0.0));
    restir_gi_set_hit_distance_target_pdf(
        selected,
        restir_gi_hit_distance(selected),
        max(luminance(selected.radiance_weight_sum.rgb), 1.0e-4));
    return restir_gi_finalize_selected(selected, totalMass, sampleCount, age, roughness);
}

RestirGiReservoir restir_gi_temporal_merge(RestirGiReservoir current, RestirGiReservoir previous, float motionConfidence, inout uint rng) {
    bool currentValid = restir_gi_reservoir_valid(current);
    bool previousValid = restir_gi_reservoir_valid(previous);
    if (!currentValid && !previousValid) {
        return current;
    }
    if (!previousValid) {
        if (currentValid) {
            restir_gi_set_metadata(current, restir_gi_sample_count_u(current), 0u, restir_gi_flags(current), restir_gi_roughness(current));
        }
        return current;
    }

    float compatibility = restir_gi_temporal_compatibility(current, previous, motionConfidence);
    if (compatibility <= 0.0) {
        if (currentValid) {
            restir_gi_set_metadata(current, restir_gi_sample_count_u(current), 0u, restir_gi_flags(current), restir_gi_roughness(current));
        }
        return current;
    }
    if (!currentValid) {
        previous.radiance_weight_sum.w = min(max(restir_gi_reservoir_mass(previous) * compatibility, 1.0), 65504.0);
        restir_gi_set_metadata(
            previous,
            restir_gi_sample_count_u(previous),
            min(restir_gi_age(previous) + 1u, min(camera.restir_gi_controls.x, 255u)),
            restir_gi_flags(previous),
            restir_gi_roughness(previous));
        return previous;
    }

    float currentMass = max(restir_gi_reservoir_mass(current), 0.0);
    float previousMass = max(restir_gi_reservoir_mass(previous), 0.0) * compatibility;
    float totalMass = currentMass + previousMass;
    if (totalMass <= 1.0e-8) {
        return current;
    }

    float motionCap = mix(0.85, 0.10, 1.0 - clamp(motionConfidence, 0.0, 1.0));
    float previousProbability = clamp(previousMass / totalMass, 0.0, motionCap);
    bool selectPrevious = rand_f32(rng) < previousProbability;
    RestirGiReservoir selected = selectPrevious ? previous : current;
    vec3 blendedRadiance = mix(
        max(current.radiance_weight_sum.rgb, vec3(0.0)),
        max(previous.radiance_weight_sum.rgb, vec3(0.0)),
        previousProbability);
    float combinedSamples = restir_gi_sample_count(current) + restir_gi_sample_count(previous) * compatibility;
    uint selectedAge = selectPrevious ? min(restir_gi_age(previous) + 1u, min(camera.restir_gi_controls.x, 255u)) : 0u;
    return restir_gi_finalize_blended(
        selected,
        blendedRadiance,
        totalMass,
        min(combinedSamples, 255.0),
        selectedAge,
        restir_gi_roughness(selected));
}

void store_initial_restir_gi_reservoir(uint pixelIndex, ivec2 coords, ivec2 dims, uint packedVelocity, PathComponents components) {
    bool restirGiDebugView = restir_gi_debug_view();
    if (!restir_gi_enabled() && !restirGiDebugView) {
        return;
    }

    RestirGiReservoir reservoir = empty_restir_gi_reservoir();
    if (components.restir_gi_candidate_valid != 0u) {
        vec3 selectedIntegrand = max(components.restir_gi_candidate_radiance, vec3(0.0));
        float targetPdf = max(components.restir_gi_candidate_target_pdf, 1.0e-4);
        float sourcePdf = max(components.restir_gi_candidate_source_pdf, 1.0e-8);
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
        reservoir.hit_position_target_pdf.xyz = components.restir_gi_candidate_position;
        reservoir.suffix_radiance_source_pdf = vec4(
            max(components.restir_gi_candidate_suffix_radiance, vec3(0.0)),
            sourcePdf);
#endif
        restir_gi_set_hit_distance_target_pdf(reservoir, components.restir_gi_candidate_hit_distance, targetPdf);
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
        reservoir.source_direction_distance = vec4(
            normalize(components.restir_gi_candidate_source_direction),
            components.restir_gi_candidate_hit_distance);
#endif
        restir_gi_set_normal(reservoir, components.restir_gi_candidate_normal);
        reservoir.radiance_weight_sum = vec4(selectedIntegrand, targetPdf / sourcePdf);
        restir_gi_set_metadata(
            reservoir,
            1u,
            0u,
            RESTIR_GI_FLAG_VALID | RESTIR_GI_FLAG_VISIBLE | components.restir_gi_candidate_reuse_flags,
            clamp(components.restir_gi_candidate_roughness, 0.0, 1.0));
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
        reservoir.metadata.z |= (components.restir_gi_candidate_path_class & 0xffu) << 8u;
        uint sceneVersion = camera.gi_version_controls.z ^
            (camera.gi_version_controls.x * 16777619u) ^
            (camera.gi_version_controls.w * 2166136261u);
        reservoir.metadata.w = restir_gi_hash_u32(
            sceneVersion ^ restir_gi_hash_u32(camera.gi_version_controls.y + 0x9e3779b9u));
#else
        restir_gi_set_material_id(reservoir, components.restir_gi_candidate_material_id);
#endif
    }

    if (restir_gi_legacy_cache_mode() &&
        restirGiDebugView &&
        renderer_debug_view() != 70u &&
        restir_gi_reservoir_valid(reservoir) &&
        temporal_history_available() &&
        !streaming_instance_reset_mask(components.instance_id, 2u)) {
        vec2 velocityPixels = restir_unpack_velocity_pixels(packedVelocity);
        bool velocityNotSaturated = max(abs(velocityPixels.x), abs(velocityPixels.y)) < SCREEN_VELOCITY_SATURATION_THRESHOLD;
        vec2 historyPos = vec2(coords) - velocityPixels;
        bool historyOnscreen =
            historyPos.x >= 0.0 && historyPos.y >= 0.0 &&
            historyPos.x < float(dims.x) && historyPos.y < float(dims.y) &&
            velocityNotSaturated;
        if (historyOnscreen) {
            ivec2 historyCoords = clamp(ivec2(round(historyPos)), ivec2(0), dims - ivec2(1));
            uint historyIndex = uint(historyCoords.y) * uint(dims.x) + uint(historyCoords.x);
            RestirGiReservoir previous = previous_restir_gi_reservoirs[historyIndex];
            float motionConfidence = 1.0 - clamp(length(velocityPixels) / 24.0, 0.0, 0.75);
            uint mergeRng = sample_dimension_seed(coords, camera.temporal_frame_index, 0u, SAMPLE_DIM_RESTIR_SPATIAL + 31u);
            reservoir = restir_gi_temporal_merge(reservoir, previous, motionConfidence, mergeRng);
        }
    }

    restir_gi_reservoirs[pixelIndex] = reservoir;
}

// Phase 2: Populate the GI receiver buffer for reuse passes.
void store_restir_gi_receiver(uint pixelIndex, ivec2 coords, uint packedVelocity, PathComponents components, vec3 hitPosition, vec3 hitNormal, float hitDepth, bool didHit) {
    if ((camera.restir_gi_controls.y & 4u) == 0u) {
        return;
    }
    RestirGiReceiver receiver;
    if (didHit && components.first_material_id != 0xffffffffu) {
        receiver.positionDepth = vec4(hitPosition, hitDepth);
        receiver.normalRoughness = vec4(hitNormal, components.first_roughness);
        receiver.geometryNormalMetal = vec4(components.first_geom_normal, components.first_metallic);
        receiver.albedoOcclusion = vec4(components.restir_di_base_color, components.first_occlusion);
        receiver.materialIds = uvec4(
            components.first_material_id,
            components.instance_id,
            components.mesh_id,
            components.primitive_id);
    } else {
        receiver.positionDepth = vec4(0.0, 0.0, 0.0, 65504.0);
        receiver.normalRoughness = vec4(0.0, 1.0, 0.0, 1.0);
        receiver.geometryNormalMetal = vec4(0.0, 1.0, 0.0, 0.0);
        receiver.albedoOcclusion = vec4(0.0);
        receiver.materialIds = uvec4(0xffffffffu);
    }
    uint cutBit = temporal_history_available() ? 0u : 1u;
    uint sceneVersion = camera.gi_version_controls.z ^
        (camera.gi_version_controls.x * 16777619u) ^
        (camera.gi_version_controls.w * 2166136261u);
    receiver.motion = uvec4(
        packedVelocity,
        cutBit,
        sceneVersion,
        camera.gi_version_controls.y);
    restir_gi_receivers[pixelIndex] = receiver;
}


#endif // RTV_PATHTRACE_RESTIR_GLSL
