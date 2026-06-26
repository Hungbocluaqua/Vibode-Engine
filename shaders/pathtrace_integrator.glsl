#ifndef RTV_PATHTRACE_INTEGRATOR_GLSL
#define RTV_PATHTRACE_INTEGRATOR_GLSL

// Main path integration routine; include after all lighting and ray-query helpers.
vec3 trace_path(Ray ray, inout uint rng, uint pixelIndex, ivec2 coords, ivec2 dims, out bool did_hit, out float first_depth, out vec3 first_normal, out vec3 first_position, out PathComponents components) {
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    vec3 atmosTransmittance = vec3(1.0);
    components.direct_light = vec3(0.0);
    components.indirect_light = vec3(0.0);
    components.emissive_light = vec3(0.0);
    components.visible_emissive_light = vec3(0.0);
    components.environment_light = vec3(0.0);
    components.restir_initial_direct_light = vec3(0.0);
    components.first_albedo = vec3(1.0);
    components.first_specular_albedo = vec3(0.04);
    components.restir_di_base_color = vec3(1.0);
    components.restir_di_f0 = vec3(0.04);
    components.first_roughness = 1.0;
    components.first_occlusion = 1.0;
    components.first_metallic = 0.0;
    components.first_alpha = 1.0;
    components.first_alpha_mode = 0u;
    components.first_transmission = 0.0;
    components.first_mat_type = 0u;
    components.restir_di_material_supported = 0u;
    components.restir_di_material_pbr = 0u;
    components.restir_gi_receiver_supported = 0u;
    components.first_light_pdf = 0.0;
    components.first_bsdf_pdf = 0.0;
    components.first_mis_weight = 0.0;
    components.first_direct_sample_type = 0u;
    components.first_bounce_throughput = vec3(0.0);
    components.secondary_env_miss = 0.0;
    components.secondary_env_radiance = vec3(0.0);
    components.emissive_continuation = 0.0;
    components.sun_mis_weight = 0.0;
    components.sun_light_pdf = 0.0;
    components.sun_previous_bsdf_pdf = 0.0;
    components.ris_raw_light_pdf = 0.0;
    components.ris_effective_light_pdf = 0.0;
    components.ris_pdf_ratio = 0.0;
    components.restir_di_sample_position = vec3(0.0);
    components.restir_di_sample_radiance = vec3(0.0);
    components.restir_di_sample_normal = vec3(0.0, 1.0, 0.0);
    components.first_view_direction = -normalize(ray.direction);
    components.restir_di_sample_distance = 0.0;
    components.restir_di_light_index = 0u;
    components.restir_di_light_kind = 0u;
    components.caustic_transmissive_hits = 0.0;
    components.caustic_visible_paths = 0.0;
    components.caustic_blocked_paths = 0.0;
    components.first_specular_probability = 0.0;
    components.direct_light_hit_distance = 65504.0;
    components.diffuse_hit_distance = 65504.0;
    components.specular_hit_distance = 65504.0;
    components.secondary_hit_distance = 65504.0;
    components.direct_light_direction = normalize(ray.direction);
    components.diffuse_ray_direction = normalize(ray.direction);
    components.specular_ray_direction = normalize(ray.direction);
    components.secondary_ray_direction = normalize(ray.direction);
    components.first_local_pos = vec3(0.0);
    components.first_barycentrics = vec3(1.0, 0.0, 0.0);
    components.first_material_id = 0xffffffffu;
    components.restir_gi_candidate_valid = 0u;
    components.restir_gi_candidate_position = vec3(0.0);
    components.restir_gi_candidate_normal = vec3(0.0, 1.0, 0.0);
    components.restir_gi_candidate_radiance = vec3(0.0);
    components.restir_gi_candidate_suffix_radiance = vec3(0.0);
    components.restir_gi_candidate_source_direction = vec3(0.0, 1.0, 0.0);
    components.restir_gi_receiver_position = vec3(0.0);
    components.restir_gi_candidate_roughness = 1.0;
    components.restir_gi_candidate_hit_distance = 65504.0;
    components.restir_gi_candidate_source_pdf = 0.0;
    components.restir_gi_candidate_target_pdf = 0.0;
    components.restir_gi_candidate_path_class = PROD_PATH_CLASS_INVALID;
    components.restir_gi_candidate_reuse_flags = 0u;
    components.restir_gi_candidate_material_id = 0xffffffffu;
    components.bounce_count = 0u;
    components.instance_id = 0xffffffffu;
    components.mesh_id = 0xffffffffu;
    components.primitive_id = 0xffffffffu;
    components.packed_velocity = 0u;
    components.packed_velocity_valid = 0u;

    bool previousWasBrdfSample = false;
    uint previousEventType = PATH_EVENT_NONE;
    float previousBrdfPdf = 0.0;
    vec3 previousSampleOrigin = vec3(0.0);
    vec3 prevHitPos = ray.origin;
    vec3 prevRayDir = ray.direction;
    did_hit = false;
    first_depth = 1e30;
    first_normal = vec3(0.0, 1.0, 0.0);
    first_position = vec3(0.0);

    uint bounceLimit =
#if RTV_NATIVE2B_PIPELINE
        2u;
#else
        camera.path_tracing_enabled != 0u ? max(camera.max_bounces, 1u) : 1u;
#endif
    uint rrStartDepth = clamp(camera.max_bounces / 2u, 3u, 5u);
    for (uint bounce = 0u; bounce < bounceLimit; ++bounce) {
        rng ^= sample_dimension_seed(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_PATH_SEED);
        if (native2b_kernel_enabled() && bounce == 1u) {
            TerminalRayPayload terminalHit = trace_terminal_surface(ray.origin, ray.direction, 0.001, 10000.0);
            components.secondary_ray_direction = normalize(prevRayDir);
            if (terminalHit.hit == 0u) {
                vec3 sky = environment_radiance(ray.direction, ATMOSPHERE_RAY_QUALITY_FAST);
                vec3 sunDisk = environment_sun_disk_radiance(ray.direction);
                vec3 env = throughput * (sky + sunDisk);
                if (previousEventType == PATH_EVENT_BSDF && previousWasBrdfSample && env_params.enabled != 0u) {
                    float envPdf = environment_pdf(ray.direction);
                    if (envPdf > 1e-6) {
                        float skyWeight = power_heuristic(previousBrdfPdf, envPdf);
                        float sunPdf = analytical_sun_pdf(ray.direction);
                        float sunWeight = sunPdf > 1e-6 ? power_heuristic(previousBrdfPdf, sunPdf) : 1.0;
                        env = throughput * (sky * skyWeight + sunDisk * sunWeight);
                        if (sunPdf > 1e-6 && luminance(sunDisk) > 0.0) {
                            components.sun_mis_weight = sunWeight;
                            components.sun_light_pdf = sunPdf;
                            components.sun_previous_bsdf_pdf = previousBrdfPdf;
                        }
                    }
                }
                env *= camera.indirect_strength *
                    mix(components.first_occlusion, 1.0, components.first_specular_probability);
                radiance += env;
                components.indirect_light += env;
                components.environment_light += env;
                break;
            }

            vec3 terminalWorldPos = ray.origin + ray.direction * terminalHit.t;
            components.bounce_count += 1u;
            components.secondary_hit_distance = min(terminalHit.t, 65504.0);
            if (camera.sunlight_enabled != 0u) {
                float segDist = length(terminalWorldPos - prevHitPos);
                if (segDist > 0.1) {
                    atmosTransmittance *= atmosphere_segment_transmittance(prevHitPos, prevRayDir, segDist, 8);
                }
            }
            bool terminalCanContribute = (terminalHit.material_id & TERMINAL_MATERIAL_CONTRIBUTING_BIT) != 0u;
            uint terminalMaterialIndex = terminalHit.material_id & TERMINAL_MATERIAL_INDEX_MASK;
            if (!terminalCanContribute) {
                break;
            }
            RayPayload terminalRayHit;
            terminalRayHit.hit = 1u;
            terminalRayHit.t = terminalHit.t;
            terminalRayHit.world_pos = terminalWorldPos;
            terminalRayHit.material_id = terminalMaterialIndex;
            terminalRayHit.local_pos = terminalWorldPos;
            terminalRayHit.geom_normal = terminalHit.geom_normal;
            terminalRayHit.front_face = 1u;
            terminalRayHit.normal = terminalHit.normal;
            terminalRayHit.instance_id = 0xffffffffu;
            terminalRayHit.mesh_id = 0xffffffffu;
            terminalRayHit.primitive_id = 0xffffffffu;
            terminalRayHit.picking = 0u;
            terminalRayHit.barycentrics = vec3(1.0, 0.0, 0.0);
            terminalRayHit.uv = terminalHit.uv;
            terminalRayHit.uv1 = terminalHit.uv1;
            terminalRayHit.tangent = terminalHit.tangent;
            terminalRayHit.bitangent = terminalHit.bitangent;
            terminalRayHit.vertex_color = terminalHit.vertex_color;
#if RTV_NATIVE2B_COMPACT_PRIMARY_LIGHTS
            Material terminalMaterial = terminal_material_for_hit_fast(terminalRayHit, ray.direction);
#else
            Material terminalMaterial = material_for_hit(terminalRayHit, ray.direction);
#endif
            const float terminalScale = camera.indirect_strength *
                mix(components.first_occlusion, 1.0, components.first_specular_probability);
            if (material_is_unlit(terminalMaterial)) {
                vec3 unlit = throughput * terminalMaterial.color * atmosTransmittance * terminalScale;
                radiance += unlit;
                components.indirect_light += unlit;
                components.emissive_light += unlit;
            } else if (has_positive_radiance(terminalMaterial.emissive)) {
                vec3 emit = throughput * terminalMaterial.emissive * atmosTransmittance;
                float lightPdf = terminal_emissive_hit_pdf(terminalHit, terminalMaterial, terminalWorldPos, previousSampleOrigin);
                if (previousWasBrdfSample && lightPdf > 0.0) {
                    emit *= power_heuristic(previousBrdfPdf, lightPdf);
                }
                emit *= terminalScale;
                radiance += emit;
                components.indirect_light += emit;
                components.emissive_light += emit;
            }
            const bool allowSecondaryDirect = camera.restir_di_controls.y != 0u;
            if (allowSecondaryDirect && !material_is_unlit(terminalMaterial) && !has_positive_radiance(terminalMaterial.emissive)) {
                vec3 terminalEmissiveDirect = vec3(0.0);
                vec3 terminalEnvDirect = vec3(0.0);
                uint terminalDirectSampleType = 0u;
                uint terminalCausticTransmissiveHits = 0u;
                uint terminalCausticVisiblePaths = 0u;
                uint terminalCausticBlockedPaths = 0u;
                uint directRng = sample_dimension_seed(coords, camera.temporal_frame_index, 1u, SAMPLE_DIM_LIGHT_SELECT);
                vec3 terminalDirect = vec3(0.0);
                if (native2b_terminal_direct_fast_supported(terminalMaterial)) {
                    record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_USED);
                    terminalDirect = throughput * estimate_native2b_terminal_env_sun_direct(
                        directRng,
                        terminalRayHit,
                        terminalMaterial,
                        -ray.direction,
                        ray.time,
                        terminalEnvDirect,
                        terminalDirectSampleType);
                } else {
                    record_rt_counter(RT_DIAG_TERMINAL_GENERIC_DIRECT_USED);
                    float terminalLightPdf;
                    float terminalRawLightPdf;
                    float terminalEffectiveLightPdf;
                    float terminalBsdfPdfForLight;
                    float terminalMisWeight;
                    uint terminalDirectLightIndex;
                    uint terminalDirectLightKind;
                    vec3 terminalDirectLightPosition;
                    float terminalDirectLightDistance;
                    vec3 terminalDirectLightDirection;
                    vec3 terminalDirectLightRadiance;
                    vec3 terminalDirectLightNormal;
                    terminalDirect = throughput * estimate_direct_lighting(
                        directRng,
                        terminalRayHit,
                        terminalMaterial,
                        -ray.direction,
                        ray.time,
                        terminalEmissiveDirect,
                        terminalEnvDirect,
                        terminalLightPdf,
                        terminalRawLightPdf,
                        terminalEffectiveLightPdf,
                        terminalBsdfPdfForLight,
                        terminalMisWeight,
                        terminalDirectSampleType,
                        terminalDirectLightIndex,
                        terminalDirectLightKind,
                        terminalDirectLightPosition,
                        terminalDirectLightDistance,
                        terminalDirectLightDirection,
                        terminalDirectLightRadiance,
                        terminalDirectLightNormal,
                        terminalCausticTransmissiveHits,
                        terminalCausticVisiblePaths,
                        terminalCausticBlockedPaths);
                }
                rng ^= directRng;
                components.caustic_transmissive_hits += float(terminalCausticTransmissiveHits);
                components.caustic_visible_paths += float(terminalCausticVisiblePaths);
                components.caustic_blocked_paths += float(terminalCausticBlockedPaths);
                vec3 scaledTerminalDirect = terminalDirect * atmosTransmittance * terminalScale;
                scaledTerminalDirect = clamp_luminance_preserve_hue(
                    scaledTerminalDirect,
                    direct_contribution_luminance_limit(1u, terminalDirectSampleType, terminalMaterial.roughness));
                radiance += scaledTerminalDirect;
                components.indirect_light += scaledTerminalDirect;
                components.emissive_light += throughput * terminalEmissiveDirect * atmosTransmittance * terminalScale;
                components.environment_light += throughput * terminalEnvDirect * atmosTransmittance * terminalScale;
            } else if (allowSecondaryDirect) {
                record_rt_counter(RT_DIAG_TERMINAL_DIRECT_SKIPPED_EMISSIVE_OR_UNLIT);
            }
            break;
        }
        RayPayload hit = trace_surface(ray.origin, ray.direction, 0.001, 10000.0, ray.time);
        float segmentDistance = hit.hit != 0u ? hit.t : 10000.0;
#if !RTV_NATIVE2B_PIPELINE
        float mediumDistance = sample_homogeneous_medium_distance(coords, bounce);
        if (mediumDistance < segmentDistance) {
            vec3 scatterPos = ray.origin + ray.direction * mediumDistance;
            float sigmaT = homogeneous_sigma_t();
            float albedo = sigmaT > 1.0e-7 ? homogeneous_sigma_s() / sigmaT : 0.0;
            throughput *= albedo;
            vec3 volumeDirect = throughput * estimate_volume_direct_lighting(rng, scatterPos, ray.direction, ray.time);
            radiance += volumeDirect;
            components.environment_light += volumeDirect;
            float phasePdf;
            vec3 nextDir = sample_henyey_greenstein(ray.direction, coords, bounce, phasePdf);
            if (phasePdf <= 1.0e-6 || max(max(throughput.r, throughput.g), throughput.b) <= 1.0e-5) {
                break;
            }
            ray.origin = scatterPos;
            ray.direction = nextDir;
            prevHitPos = scatterPos;
            prevRayDir = ray.direction;
            previousWasBrdfSample = false;
            previousEventType = PATH_EVENT_NONE;
            previousBrdfPdf = phasePdf;
            components.bounce_count += 1u;
            continue;
        }
#endif
        if (hit.hit == 0u) {
            const uint envQuality = bounce == 0u ? ATMOSPHERE_RAY_QUALITY_FULL : ATMOSPHERE_RAY_QUALITY_FAST;
            vec3 sky = bounce == 0u
                ? environment_background_radiance(ray.direction, envQuality)
                : environment_radiance(ray.direction, envQuality);
            vec3 sunDisk = environment_sun_disk_radiance(ray.direction);
            if (bounce == 0u) {
                sky *= env_params.background_intensity;
            }
            vec3 env = throughput * (sky + sunDisk);
            if (previousEventType == PATH_EVENT_BSDF && previousWasBrdfSample && env_params.enabled != 0u && debug_params.view != 27u) {
                float envPdf = environment_pdf(ray.direction);
                if (envPdf > 1e-6) {
                    float skyWeight = power_heuristic(previousBrdfPdf, envPdf);
                    float sunPdf = analytical_sun_pdf(ray.direction);
                    float sunWeight = sunPdf > 1e-6 ? power_heuristic(previousBrdfPdf, sunPdf) : 1.0;
                    env = throughput * (sky * skyWeight + sunDisk * sunWeight);
                    if (sunPdf > 1e-6 && luminance(sunDisk) > 0.0) {
                        components.sun_mis_weight = sunWeight;
                        components.sun_light_pdf = sunPdf;
                        components.sun_previous_bsdf_pdf = previousBrdfPdf;
                    }
                }
            }
            float envContributionScale = bounce > 0u ? camera.indirect_strength : 1.0;
            if (bounce > 0u) {
                envContributionScale *= mix(components.first_occlusion, 1.0, components.first_specular_probability);
            }
            env *= envContributionScale;
            if (restir_gi_enabled() &&
                bounce == 1u && previousEventType == PATH_EVENT_BSDF && previousBrdfPdf > 1.0e-8 &&
                components.first_transmission <= 1.0e-5 && components.first_roughness >= 0.2 &&
                components.restir_gi_receiver_supported != 0u) {
                RestirGiReceiver sourceReceiver;
                sourceReceiver.positionDepth = vec4(components.restir_gi_receiver_position, first_depth);
                sourceReceiver.normalRoughness = vec4(first_normal, components.first_roughness);
                sourceReceiver.geometryNormalMetal = vec4(components.first_geom_normal, components.first_metallic);
                sourceReceiver.albedoOcclusion = vec4(components.restir_di_base_color, components.first_occlusion);
                sourceReceiver.materialIds = uvec4(components.first_material_id, components.instance_id, components.mesh_id, components.primitive_id);
                sourceReceiver.motion = uvec4(0u);
                vec3 sourceDirection = normalize(ray.direction);
                vec3 receiverFactor = restir_gi_receiver_factor(sourceReceiver, camera.pos.xyz, sourceDirection);
                vec3 selectedIntegrand = max(env, vec3(0.0)) * previousBrdfPdf;
                vec3 suffix = selectedIntegrand / max(receiverFactor, vec3(1.0e-6));
                float target = restir_gi_luma(selectedIntegrand);
                if (target > 1.0e-8 && restir_gi_finite3(suffix)) {
                    components.restir_gi_candidate_valid = 1u;
                    components.restir_gi_candidate_position = sourceDirection;
                    components.restir_gi_candidate_normal = -sourceDirection;
                    components.restir_gi_candidate_radiance = selectedIntegrand;
                    components.restir_gi_candidate_suffix_radiance = suffix;
                    components.restir_gi_candidate_source_direction = sourceDirection;
                    components.restir_gi_candidate_roughness = 1.0;
                    components.restir_gi_candidate_hit_distance = -1.0;
                    components.restir_gi_candidate_source_pdf = previousBrdfPdf;
                    components.restir_gi_candidate_target_pdf = target;
                    components.restir_gi_candidate_path_class = PROD_PATH_CLASS_ENVIRONMENT_REUSABLE;
                    components.restir_gi_candidate_reuse_flags = PROD_FLAG_ENVIRONMENT |
                        (components.first_alpha_mode != 0u ? PROD_FLAG_ALPHA_TESTED : 0u);
                    components.restir_gi_candidate_material_id = components.first_material_id;
                }
            }
            radiance += env;
            if (bounce > 0u) {
                components.indirect_light += env;
            }
            components.environment_light += env;
            break;
        }
        components.bounce_count += 1u;

        Material material = material_for_hit(hit, ray.direction);
        if (bounce == 0u) {
            did_hit = true;
            first_depth = hit.t;
            first_normal = hit.normal;
            first_position = hit.world_pos;
            components.first_geom_normal = hit.geom_normal;
            components.restir_gi_receiver_position = hit.world_pos;
            components.first_local_pos = hit.local_pos;
            components.first_barycentrics = hit.barycentrics;
            components.first_albedo = pbr_diffuse_reflectance(material);
            components.first_specular_albedo = pbr_specular_reflectance(material, max(dot(hit.normal, -ray.direction), 0.0));
            components.restir_di_base_color = clamp(material.color, vec3(0.0), vec3(1.0));
            components.restir_di_f0 = pbr_f0(material);
            components.first_roughness = material.roughness;
            components.first_occlusion = material.occlusion;
            components.first_metallic = material.metallic;
            components.first_alpha = material.alpha_factor;
            components.first_alpha_mode = material.alpha_mode;
            components.first_transmission = material_effective_transmission(material);
            components.first_mat_type = material.mat_type;
            bool restirDiSpecialClosure = material.mat_type == 2u || material.mat_type == 4u || material.mat_type == 5u ||
                material_effective_transmission(material) > 1.0e-5 ||
                material.clearcoat_factor > 1.0e-5 ||
                luminance(max(material.sheen_color, vec3(0.0))) > 1.0e-5 ||
                abs(material.anisotropy_strength) > 1.0e-4 ||
                material.use_conductor_optics != 0u ||
                material.iridescence_factor > 1.0e-5 ||
                (specular_aa_enabled() && material.normal_variance > 1.0e-5);
            components.restir_di_material_supported = restirDiSpecialClosure ? 0u : 1u;
            components.restir_di_material_pbr = material.mat_type == 1u || material.mat_type == 3u ? 1u : 0u;
            if (restir_gi_enabled() || restir_gi_debug_view()) {
                bool restirGiUnsupportedClosure = material.mat_type == 2u || material.mat_type == 4u || material.mat_type == 5u ||
                    material_effective_transmission(material) > 1.0e-5 ||
                    material.clearcoat_factor > 1.0e-5 ||
                    luminance(max(material.sheen_color, vec3(0.0))) > 1.0e-5 ||
                    abs(material.anisotropy_strength) > 1.0e-4 ||
                    material.use_conductor_optics != 0u ||
                    material.iridescence_factor > 1.0e-5 ||
                    homogeneous_volume_enabled();
                components.restir_gi_receiver_supported = restirGiUnsupportedClosure ? 0u : 1u;
            } else {
                components.restir_gi_receiver_supported = 0u;
            }
            components.first_specular_probability = pbr_specular_sample_probability(material, max(dot(hit.normal, -ray.direction), 0.0));
            components.first_material_id = hit.material_id;
            components.restir_gi_candidate_material_id = hit.material_id;
            components.instance_id = hit.instance_id;
            components.mesh_id = hit.mesh_id;
            components.primitive_id = hit.primitive_id;
        } else if (bounce == 1u) {
            components.secondary_hit_distance = min(hit.t, 65504.0);
            components.secondary_ray_direction = normalize(prevRayDir);
        }

        const bool finalBounceFastPath =
            (camera.path_trace_controls.z & 8u) != 0u &&
            native2b_kernel_enabled() &&
            bounce + 1u >= bounceLimit &&
            bounce > 0u &&
            previousEventType == PATH_EVENT_BSDF &&
            previousWasBrdfSample &&
            !restir_gi_enabled() &&
            !homogeneous_volume_enabled() &&
            debug_params.view != 27u;
        if (finalBounceFastPath) {
            Material terminalMaterial = decode_material(hit.material_id);
            const float terminalScale = camera.indirect_strength *
                mix(components.first_occlusion, 1.0, components.first_specular_probability);
            if (material_is_unlit(terminalMaterial)) {
                vec3 unlit = throughput * terminalMaterial.color * atmosTransmittance * terminalScale;
                radiance += unlit;
                components.indirect_light += unlit;
                components.emissive_light += unlit;
            } else if (has_positive_radiance(terminalMaterial.emissive)) {
                vec3 emit = throughput * terminalMaterial.emissive * atmosTransmittance;
                if (previousWasBrdfSample) {
                    float lightPdf = emissive_hit_pdf(hit, terminalMaterial, previousSampleOrigin);
                    if (lightPdf > 0.0) {
                        emit *= power_heuristic(previousBrdfPdf, lightPdf);
                    }
                }
                emit *= terminalScale;
                radiance += emit;
                components.indirect_light += emit;
                components.emissive_light += emit;
            }
            break;
        }

        if (camera.sunlight_enabled != 0u && debug_params.view != 27u) {
            float segDist = length(hit.world_pos - prevHitPos);
            if (bounce > 0u && segDist > 0.1) {
                vec3 segTrans = atmosphere_segment_transmittance(prevHitPos, prevRayDir, segDist, 8);
                atmosTransmittance *= segTrans;
            }
        }

        if (bounce == 0u && debug_params.view == 24u) {
            float testPdf;
            uint envTestRng = sample_dimension_seed(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_ENVIRONMENT);
            vec3 testDir = sample_cosine_hemisphere(envTestRng, hit.normal, testPdf);
            RayPayload testHit = trace_surface(hit.world_pos + hit.normal * shadow_self_hit_epsilon(), normalize(testDir), shadow_self_hit_epsilon(), 10000.0, ray.time);
            components.secondary_env_miss = testHit.hit == 0u ? 1.0 : 0.0;
            return vec3(components.secondary_env_miss);
        }
        if (bounce == 0u && debug_params.view == 26u) {
            float testPdf;
            uint envTestRng = sample_dimension_seed(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_ENVIRONMENT + 2u);
            vec3 testDir = sample_cosine_hemisphere(envTestRng, hit.normal, testPdf);
            RayPayload testHit = trace_surface(hit.world_pos + hit.normal * shadow_self_hit_epsilon(), normalize(testDir), shadow_self_hit_epsilon(), 10000.0, ray.time);
            components.secondary_env_radiance = testHit.hit == 0u ? environment_radiance(normalize(testDir), ATMOSPHERE_RAY_QUALITY_FAST) : vec3(0.0);
            return debug_display_tonemap(components.secondary_env_radiance);
        }

        vec3 wo = -ray.direction;
        if (bounce == 0u) {
            components.first_view_direction = normalize(wo);
        }
        if (material_is_unlit(material)) {
            float unlitContributionScale = bounce > 0u ? camera.indirect_strength : 1.0;
            if (bounce > 0u) {
                unlitContributionScale *= mix(components.first_occlusion, 1.0, components.first_specular_probability);
            }
            vec3 unlit = throughput * material.color * atmosTransmittance * unlitContributionScale;
            radiance += unlit;
            if (bounce > 0u) {
                components.indirect_light += unlit;
            }
            components.emissive_light += unlit;
            if (bounce == 0u) {
                components.visible_emissive_light += unlit;
            }
            break;
        }
        bool hitEmissiveSurface = has_positive_radiance(material.emissive);
        vec3 restirGiCandidateRadiance = vec3(0.0);
        if (hitEmissiveSurface) {
            vec3 emit = throughput * material.emissive * atmosTransmittance;
            if (previousWasBrdfSample) {
                float lightPdf = emissive_hit_pdf(hit, material, previousSampleOrigin);
                if (lightPdf > 0.0) {
                    emit *= power_heuristic(previousBrdfPdf, lightPdf);
                }
            }
            emit *= bounce > 0u ? camera.indirect_strength : 1.0;
            if (bounce > 0u) {
                emit *= mix(components.first_occlusion, 1.0, components.first_specular_probability);
            }
            radiance += emit;
            if (bounce > 0u) {
                components.indirect_light += emit;
                if (restir_gi_enabled() && bounce == 1u && previousEventType == PATH_EVENT_BSDF) {
                    restirGiCandidateRadiance += emit;
                }
            }
            components.emissive_light += emit;
            if (bounce == 0u) {
                components.visible_emissive_light += emit;
            }
        }

        vec3 emissiveDirect;
        vec3 envDirect;
        float lightPdf;
        float rawLightPdf;
        float effectiveLightPdf;
        float bsdfPdfForLight;
        float misWeight;
        uint directSampleType;
        uint directLightIndex;
        uint directLightKind;
        vec3 directLightPosition;
        float directLightDistance;
        vec3 directLightDirection;
        vec3 directLightRadiance;
        vec3 directLightNormal;
        uint causticTransmissiveHits;
        uint causticVisiblePaths;
        uint causticBlockedPaths;
        vec3 direct = vec3(0.0);
        emissiveDirect = vec3(0.0);
        envDirect = vec3(0.0);
        lightPdf = 0.0;
        rawLightPdf = 0.0;
        effectiveLightPdf = 0.0;
        bsdfPdfForLight = 0.0;
        misWeight = 0.0;
        directSampleType = 0u;
        directLightIndex = 0u;
        directLightKind = 0u;
        directLightPosition = vec3(0.0);
        directLightDistance = 0.0;
        directLightDirection = vec3(0.0, 1.0, 0.0);
        directLightRadiance = vec3(0.0);
        directLightNormal = vec3(0.0, 1.0, 0.0);
        causticTransmissiveHits = 0u;
        causticVisiblePaths = 0u;
        causticBlockedPaths = 0u;
        const bool allowSecondaryDirect = camera.restir_di_controls.y != 0u;
        if (!hitEmissiveSurface && (bounce == 0u || allowSecondaryDirect)) {
            uint directRng = sample_dimension_seed(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_LIGHT_SELECT);
#if RTV_NATIVE2B_COMPACT_PRIMARY_LIGHTS
            if (bounce == 0u && compact_imported_emissive_direct_enabled() && mesh_params.authored_light_count == 0u) {
                direct = throughput * estimate_direct_lighting_env_sun_only(
                    directRng,
                    hit,
                    material,
                    wo,
                    ray.time,
                    envDirect,
                    lightPdf,
                    rawLightPdf,
                    effectiveLightPdf,
                    bsdfPdfForLight,
                    misWeight,
                    directSampleType,
                    directLightDirection,
                    directLightDistance,
                    causticTransmissiveHits,
                    causticVisiblePaths,
                    causticBlockedPaths);
            } else {
#endif
                direct = throughput * estimate_direct_lighting(
                    directRng,
                    hit,
                    material,
                    wo,
                    ray.time,
                    emissiveDirect,
                    envDirect,
                    lightPdf,
                    rawLightPdf,
                    effectiveLightPdf,
                    bsdfPdfForLight,
                    misWeight,
                    directSampleType,
                    directLightIndex,
                    directLightKind,
                    directLightPosition,
                    directLightDistance,
                    directLightDirection,
                    directLightRadiance,
                    directLightNormal,
                    causticTransmissiveHits,
                    causticVisiblePaths,
                    causticBlockedPaths);
#if RTV_NATIVE2B_COMPACT_PRIMARY_LIGHTS
            }
#endif
            rng ^= directRng;
        }
        components.caustic_transmissive_hits += float(causticTransmissiveHits);
        components.caustic_visible_paths += float(causticVisiblePaths);
        components.caustic_blocked_paths += float(causticBlockedPaths);
        vec3 shadedDirect = direct;
        vec3 restirDirect = directSampleType == 1u ? throughput * emissiveDirect : vec3(0.0);
        vec3 nonRestirDirect = max(direct - restirDirect, vec3(0.0));
        if (bounce == 0u && restir_mode() != 0u) {
            bool historyAvailable = temporal_history_available() &&
                !streaming_instance_reset_mask(hit.instance_id, 2u);
            uint packedFirstVel = compute_surface_velocity(
                hit.world_pos,
                hit.local_pos,
                hit.instance_id,
                hit.mesh_id,
                hit.primitive_id,
                hit.barycentrics,
                dims);
            components.packed_velocity = packedFirstVel;
            components.packed_velocity_valid = 1u;
            RestirReservoir previousReservoir = empty_restir_reservoir();
            bool reprojectionValid = false;
            float restirMotionConfidence = 1.0;
            if (historyAvailable && packedFirstVel != 0u) {
                vec2 firstVel = restir_unpack_velocity_pixels(packedFirstVel);
                ivec2 reprojected = ivec2(round(vec2(coords) - firstVel));
                bool reprojectedInBounds = reprojected.x >= 0 && reprojected.y >= 0 &&
                    reprojected.x < dims.x && reprojected.y < dims.y;
                bool velocityNotSaturated = max(abs(firstVel.x), abs(firstVel.y)) < SCREEN_VELOCITY_SATURATION_THRESHOLD;
                reprojectionValid = reprojectedInBounds && velocityNotSaturated &&
                    length(firstVel) < float(max(dims.x, dims.y)) * 0.5;
                if (reprojectionValid) {
                    uint reprojectedIndex = uint(reprojected.y) * uint(dims.x) + uint(reprojected.x);
                    previousReservoir = previous_restir_reservoirs[reprojectedIndex];
                    restirMotionConfidence = 1.0 - clamp(length(firstVel) / 24.0, 0.0, 0.75);
                }
            } else if (historyAvailable) {
                previousReservoir = previous_restir_reservoirs[pixelIndex];
                reprojectionValid = true;
            }
            bool previousReservoirValid = restir_reservoir_valid(previousReservoir) &&
                historyAvailable &&
                reprojectionValid;
            bool useRestir = restir_mode() == 1u || (restir_mode() == 2u && coords.x >= dims.x / 2);
            if (useRestir) {
                RestirReservoir currentReservoir = make_initial_restir_reservoir(
                    restirDirect * atmosTransmittance,
                    lightPdf,
                    directSampleType == 1u ? 1u : 0u,
                    material.roughness,
                    hit.material_id,
                    hit.instance_id,
                    directLightIndex,
                    directLightKind,
                    directLightPosition,
                    directLightDistance,
                    directLightRadiance,
                    directLightNormal);
                if (previousReservoirValid && previousReservoir.metadata.w == currentReservoir.metadata.w) {
                    uint previousCausticHits;
                    uint previousVisiblePaths;
                    uint previousBlockedPaths;
                    previousReservoir = reevaluate_restir_di_reservoir(
                        previousReservoir,
                        hit,
                        material,
                        wo,
                        throughput,
                        atmosTransmittance,
                        ray.time,
                        previousCausticHits,
                        previousVisiblePaths,
                        previousBlockedPaths);
                    components.caustic_transmissive_hits += float(previousCausticHits);
                    components.caustic_visible_paths += float(previousVisiblePaths);
                    components.caustic_blocked_paths += float(previousBlockedPaths);
                } else {
                    previousReservoir = empty_restir_reservoir();
                }
                RestirReservoir mergedReservoir = restir_temporal_resample(
                    currentReservoir,
                    previousReservoir,
                    restirMotionConfidence,
                    rng);
                vec3 mergedRestirDirect = mergedReservoir.sample_value_confidence.rgb / max(atmosTransmittance, vec3(1.0e-3));
                mergedRestirDirect = clamp_luminance_preserve_hue(
                    mergedRestirDirect,
                    restir_temporal_luminance_limit(restirDirect));
                shadedDirect = nonRestirDirect + mergedRestirDirect;
            }
        }
        float bounceContributionScale = bounce > 0u ? camera.indirect_strength : 1.0;
        if (bounce > 0u) {
            bounceContributionScale *= mix(components.first_occlusion, 1.0, components.first_specular_probability);
        }
        vec3 unclampedShadedDirect = shadedDirect * atmosTransmittance * bounceContributionScale;
        vec3 scaledShadedDirect = unclampedShadedDirect;
        scaledShadedDirect = clamp_luminance_preserve_hue(
            scaledShadedDirect,
            direct_contribution_luminance_limit(bounce, directSampleType, material.roughness));
        radiance += scaledShadedDirect;
        if (bounce == 0u) {
            components.direct_light += scaledShadedDirect;
            vec3 unclampedInitialDirect = direct * atmosTransmittance;
            vec3 clampedInitialDirect = clamp_luminance_preserve_hue(
                unclampedInitialDirect,
                direct_contribution_luminance_limit(0u, directSampleType, material.roughness));
            float unclampedInitialLum = luminance(unclampedInitialDirect);
            float initialClampScale = unclampedInitialLum > 1.0e-8
                ? clamp(luminance(clampedInitialDirect) / unclampedInitialLum, 0.0, 1.0)
                : 1.0;
            components.restir_initial_direct_light += restirDirect * atmosTransmittance * initialClampScale;
            components.first_light_pdf = lightPdf;
            components.first_bsdf_pdf = bsdfPdfForLight;
            components.first_mis_weight = misWeight;
            components.first_direct_sample_type = directSampleType;
            components.restir_di_sample_position = directLightPosition;
            components.restir_di_sample_distance = directLightDistance;
            components.restir_di_sample_radiance = directLightRadiance;
            components.restir_di_sample_normal = directLightNormal;
            components.restir_di_light_index = directLightIndex;
            components.restir_di_light_kind = directLightKind;
            if (directSampleType != 0u) {
                components.direct_light_hit_distance = clamp(max(directLightDistance, 0.0), 0.0, 65504.0);
                components.direct_light_direction = normalize(directLightDirection);
            }
            components.ris_raw_light_pdf = rawLightPdf;
            components.ris_effective_light_pdf = effectiveLightPdf;
            components.ris_pdf_ratio = rawLightPdf > 1.0e-6 ? effectiveLightPdf / rawLightPdf : 0.0;
            if (directSampleType == 3u) {
                components.sun_mis_weight = misWeight;
                components.sun_light_pdf = lightPdf;
                components.sun_previous_bsdf_pdf = bsdfPdfForLight;
            }
        } else {
            components.indirect_light += scaledShadedDirect;
        }
        if (restir_gi_enabled() && bounce == 1u && previousEventType == PATH_EVENT_BSDF) {
            restirGiCandidateRadiance += scaledShadedDirect;
            bool receiverSupported = components.restir_gi_receiver_supported != 0u &&
                components.first_transmission <= 1.0e-5 &&
                components.first_roughness >= 0.2 && components.first_mat_type != 2u &&
                material_effective_transmission(material) <= 1.0e-5 &&
                !homogeneous_volume_enabled();
            float distance = max(length(hit.world_pos - first_position), 1.0e-4);
            vec3 sourceDirection = normalize(hit.world_pos - first_position);
            float measurePdf = restir_gi_source_measure_pdf(
                previousBrdfPdf, sourceDirection, hit.normal, distance, false);
            RestirGiReceiver sourceReceiver;
            sourceReceiver.positionDepth = vec4(first_position, first_depth);
            sourceReceiver.normalRoughness = vec4(first_normal, components.first_roughness);
            sourceReceiver.geometryNormalMetal = vec4(components.first_geom_normal, components.first_metallic);
            sourceReceiver.albedoOcclusion = vec4(components.restir_di_base_color, components.first_occlusion);
            sourceReceiver.materialIds = uvec4(components.first_material_id, components.instance_id, components.mesh_id, components.primitive_id);
            sourceReceiver.motion = uvec4(0u);
            vec3 receiverFactor = restir_gi_receiver_factor(sourceReceiver, camera.pos.xyz, sourceDirection);
            float measureFactor = max(dot(normalize(hit.normal), -sourceDirection), 0.0) / (distance * distance);
            vec3 selectedIntegrand = max(restirGiCandidateRadiance, vec3(0.0)) * measurePdf;
            vec3 suffix = selectedIntegrand / max(receiverFactor * measureFactor, vec3(1.0e-8));
            float candidateTargetPdf = luminance(selectedIntegrand);
            if (receiverSupported && previousBrdfPdf > 1.0e-8 && measurePdf > 1.0e-10 &&
                candidateTargetPdf > 1.0e-8 && restir_gi_finite3(suffix)) {
                components.restir_gi_candidate_valid = 1u;
                components.restir_gi_candidate_position = hit.world_pos;
                components.restir_gi_candidate_normal = hit.normal;
                components.restir_gi_candidate_radiance = selectedIntegrand;
                components.restir_gi_candidate_suffix_radiance = suffix;
                components.restir_gi_candidate_source_direction = sourceDirection;
                components.restir_gi_receiver_position = first_position;
                components.restir_gi_candidate_roughness = material.roughness;
                components.restir_gi_candidate_hit_distance = min(distance, 65504.0);
                components.restir_gi_candidate_source_pdf = measurePdf;
                components.restir_gi_candidate_target_pdf = candidateTargetPdf;
                components.restir_gi_candidate_path_class = hitEmissiveSurface
                    ? PROD_PATH_CLASS_EMISSIVE_REUSABLE
                    : (components.first_roughness < 0.65 || components.first_metallic > 0.35
                        ? PROD_PATH_CLASS_GLOSSY_REUSABLE
                        : PROD_PATH_CLASS_DIFFUSE_REUSABLE);
                components.restir_gi_candidate_reuse_flags =
                    (components.first_alpha_mode != 0u || material.alpha_mode != 0u)
                    ? PROD_FLAG_ALPHA_TESTED : 0u;
                components.restir_gi_candidate_material_id = components.first_material_id;
            }
        }
        components.emissive_light += throughput * emissiveDirect * atmosTransmittance * bounceContributionScale;
        components.environment_light += throughput * envDirect * atmosTransmittance * bounceContributionScale;

        if (camera.path_tracing_enabled == 0u) {
            vec3 previewNormal = normalize(hit.geom_normal);
            vec3 previewAmbient = throughput * fast_preview_ambient(material, previewNormal);
            vec3 previewSun = vec3(0.0);
            if (camera.sunlight_enabled != 0u && camera.direct_lighting_enabled != 0u) {
                vec3 sunDir = analytical_sun_direction();
                float ndl = max(dot(previewNormal, sunDir), 0.0);
                previewSun = throughput * material.color * (1.0 / PI) * analytical_sun_center_radiance() * analytical_sun_solid_angle() * sun_transmittance(hit.world_pos, sunDir) * ndl;
            }
            vec3 scaledPreviewSun = previewSun * atmosTransmittance * bounceContributionScale;
            vec3 scaledPreviewAmbient = previewAmbient * atmosTransmittance * bounceContributionScale;
            if (bounce == 0u) {
                scaledPreviewAmbient *= components.first_occlusion;
            }
            radiance += scaledPreviewSun;
            components.direct_light += scaledPreviewSun;
            radiance += scaledPreviewAmbient;
            components.indirect_light += scaledPreviewAmbient;
            break;
        }

        if (bounce + 1u >= bounceLimit) {
            break;
        }

        previousWasBrdfSample = false;
        previousEventType = PATH_EVENT_NONE;
        previousBrdfPdf = 0.0;
        bool continuedPath = false;
        if (material.mat_type == 1u && !material_is_transmissive(material) && material_is_delta(material)) {
            ray.direction = reflect(ray.direction, hit.normal);
            if (dot(ray.direction, hit.normal) <= 0.0) {
                break;
            }
            ray.origin = hit.world_pos + hit.normal * shadow_self_hit_epsilon();
            throughput *= material.color;
            throughput = clamp_path_throughput(throughput, bounce + 1u, material.roughness, true);
            if (bounce == 0u) {
                components.first_bounce_throughput = throughput;
            }
            previousEventType = PATH_EVENT_DELTA;
            continuedPath = true;
        } else if (material_is_transmissive(material)) {
            float dispersionSample = sample_dimension_1d(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_DIELECTRIC + 1u);
            uint dispersionChannel = material_dispersion_channel(material, dispersionSample);
            vec3 dispersionIor = material_dispersion_ior(material);
            float channelIor = material_channel_value(dispersionIor, dispersionChannel);
            float refractionRatio = hit.front_face != 0u ? (1.0 / channelIor) : channelIor;
            float cosTheta = min(dot(-ray.direction, hit.normal), 1.0);
            float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
            bool cannotRefract = refractionRatio * sinTheta > 1.0;
            float dielectricSample = sample_dimension_1d(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_DIELECTRIC);
            float fresnel = reflectance(cosTheta, refractionRatio);
            bool reflected = cannotRefract || fresnel > dielectricSample;
            vec3 scatteredDirection = reflected
                ? reflect(ray.direction, hit.normal)
                : refract(ray.direction, hit.normal, refractionRatio);
            ray.direction = scatteredDirection;
            ray.origin = hit.world_pos + (dot(ray.direction, hit.normal) > 0.0 ? hit.normal : -hit.normal) * shadow_self_hit_epsilon();
            throughput *= material.color * material_dispersion_channel_weight(material, dispersionChannel);
            if (!reflected) {
                throughput *= material_effective_transmission(material);
            }
            if (!reflected && hit.front_face == 0u) {
                throughput *= material_volume_transmittance(material, hit.t);
            }
            throughput = clamp_path_throughput(throughput, bounce + 1u, material.roughness, true);
            if (bounce == 0u) {
                components.first_bounce_throughput = throughput;
            }
            previousEventType = PATH_EVENT_DELTA;
            continuedPath = true;
        } else {
            float bsdfPdf;
            uint bsdfRng = sample_dimension_seed(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_BSDF);
            vec3 wi = sample_brdf(bsdfRng, material, wo, hit.normal, hit.tangent, hit.bitangent, bsdfPdf);
            rng ^= bsdfRng;
            float cosTheta = max(dot(hit.normal, wi), 0.0);
            if (bsdfPdf < 1e-6 || cosTheta <= 0.0) {
                break;
            }
            vec3 bsdf = eval_brdf(material, wo, wi, hit.normal, hit.tangent, hit.bitangent);
            ray.origin = hit.world_pos + hit.normal * shadow_self_hit_epsilon();
            ray.direction = wi;
            previousWasBrdfSample = true;
            previousEventType = PATH_EVENT_BSDF;
            previousBrdfPdf = bsdfPdf;
            previousSampleOrigin = ray.origin;
            throughput *= bsdf * cosTheta / max(bsdfPdf, 1e-6);
            throughput = clamp_path_throughput(throughput, bounce + 1u, material.roughness, false);
            if (bounce == 0u) {
                components.first_bounce_throughput = throughput;
            }
            continuedPath = true;
        }

        if (hitEmissiveSurface && continuedPath && bounce + 1u < bounceLimit) {
            components.emissive_continuation = 1.0;
        }

        prevHitPos = hit.world_pos;
        prevRayDir = ray.direction;

        uint completedDepth = bounce + 1u;
        if (completedDepth >= rrStartDepth && completedDepth < bounceLimit && debug_params.view != 27u) {
            float p = clamp(luminance(throughput), russian_roulette_min_survival(), 0.95);
            if (sample_dimension_1d(coords, camera.temporal_frame_index, bounce, SAMPLE_DIM_RUSSIAN_ROULETTE) > p) {
                break;
            }
            throughput /= p;
        }
    }
    return radiance;
}


#endif // RTV_PATHTRACE_INTEGRATOR_GLSL
