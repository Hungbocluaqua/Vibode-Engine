#ifndef RTV_PATHTRACE_RAY_QUERIES_GLSL
#define RTV_PATHTRACE_RAY_QUERIES_GLSL

// Surface, terminal, shadow, and transmittance ray helpers.
void reset_payload() {
    payload.hit = 0u;
    payload.t = 10000.0;
    payload.world_pos = vec3(0.0);
    payload.material_id = 0u;
    payload.local_pos = vec3(0.0);
    payload.geom_normal = vec3(0.0, 1.0, 0.0);
    payload.front_face = 1u;
    payload.normal = vec3(0.0, 1.0, 0.0);
    payload.instance_id = 0xffffffffu;
    payload.mesh_id = 0xffffffffu;
    payload.primitive_id = 0xffffffffu;
    payload.picking = 0u;
    payload.barycentrics = vec3(1.0, 0.0, 0.0);
    payload.uv = vec2(0.0);
    payload.uv1 = vec2(0.0);
    payload.tangent = vec3(1.0, 0.0, 0.0);
    payload.bitangent = vec3(0.0, 0.0, 1.0);
    payload.vertex_color = vec4(1.0);
}

RayPayload trace_surface_with_mode(vec3 origin, vec3 direction, float tMin, float tMax, float rayTime, uint picking) {
    record_rt_counter(RT_DIAG_SURFACE_TRACE_RAYS);
    record_rt_counter(RT_DIAG_PRIMARY_SURFACE_TRACE_RAYS);
    reset_payload();
    payload.picking = picking;
    uint cameraRayFlags = gl_RayFlagsCullBackFacingTrianglesEXT |
        (force_opaque_camera_rays_enabled() ? gl_RayFlagsOpaqueEXT : 0u);
#if RTV_MOTION_BLUR_ENABLED
    traceRayMotionNV(
        topLevelAS,
        cameraRayFlags,
        RAY_MASK_CAMERA,
        0,
        pathtrace_sbt_stride(),
        0,
        origin,
        tMin,
        direction,
        tMax,
        rayTime,
        0);
#else
    traceRayEXT(
        topLevelAS,
        cameraRayFlags,
        RAY_MASK_CAMERA,
        0,
        pathtrace_sbt_stride(),
        0,
        origin,
        tMin,
        direction,
        tMax,
        0);
#endif
    return payload;
}

RayPayload trace_surface(vec3 origin, vec3 direction, float tMin, float tMax, float rayTime) {
    return trace_surface_with_mode(origin, direction, tMin, tMax, rayTime, 0u);
}

RayPayload trace_shadow_surface(vec3 origin, vec3 direction, float tMin, float tMax, float rayTime) {
    record_rt_counter(RT_DIAG_SURFACE_TRACE_RAYS);
    record_rt_counter(RT_DIAG_SHADOW_SURFACE_TRACE_RAYS);
    reset_payload();
#if RTV_MOTION_BLUR_ENABLED
    traceRayMotionNV(
        topLevelAS,
        gl_RayFlagsCullBackFacingTrianglesEXT,
        RAY_MASK_SHADOW,
        0,
        pathtrace_sbt_stride(),
        0,
        origin,
        tMin,
        direction,
        tMax,
        rayTime,
        0);
#else
    traceRayEXT(
        topLevelAS,
        gl_RayFlagsCullBackFacingTrianglesEXT,
        RAY_MASK_SHADOW,
        0,
        pathtrace_sbt_stride(),
        0,
        origin,
        tMin,
        direction,
        tMax,
        0);
#endif
    return payload;
}

bool trace_shadow(vec3 origin, vec3 direction, float tMax, float rayTime) {
    record_rt_counter(RT_DIAG_SHADOW_TRACE_RAYS);
    shadow_occluded = 1u;
#if RTV_MOTION_BLUR_ENABLED
    traceRayMotionNV(
        topLevelAS,
        gl_RayFlagsSkipClosestHitShaderEXT |
            gl_RayFlagsTerminateOnFirstHitEXT |
            gl_RayFlagsCullBackFacingTrianglesEXT,
        RAY_MASK_SHADOW,
        1,
        pathtrace_sbt_stride(),
        1,
        origin,
        shadow_self_hit_epsilon(),
        direction,
        tMax,
        rayTime,
        1);
#else
    traceRayEXT(
        topLevelAS,
        gl_RayFlagsSkipClosestHitShaderEXT |
            gl_RayFlagsTerminateOnFirstHitEXT |
            gl_RayFlagsCullBackFacingTrianglesEXT,
        RAY_MASK_SHADOW,
        1,
        pathtrace_sbt_stride(),
        1,
        origin,
        shadow_self_hit_epsilon(),
        direction,
        tMax,
        1);
#endif
    return shadow_occluded != 0u;
}

TerminalRayPayload trace_terminal_surface(vec3 origin, vec3 direction, float tMin, float tMax) {
    record_rt_counter(RT_DIAG_SURFACE_TRACE_RAYS);
    record_rt_counter(RT_DIAG_TERMINAL_SURFACE_TRACE_RAYS);
    terminal_payload.hit = 0u;
    terminal_payload.t = tMax;
    terminal_payload.geom_normal = -direction;
    terminal_payload.normal = terminal_payload.geom_normal;
    terminal_payload.tangent = vec3(1.0, 0.0, 0.0);
    terminal_payload.bitangent = vec3(0.0, 0.0, 1.0);
    terminal_payload.uv = vec2(0.0);
    terminal_payload.uv1 = vec2(0.0);
    terminal_payload.vertex_color = vec4(1.0);
    terminal_payload.material_id = 0u;
    uint cameraRayFlags = gl_RayFlagsCullBackFacingTrianglesEXT |
        (force_opaque_terminal_rays_enabled() ? gl_RayFlagsOpaqueEXT : 0u);
    traceRayEXT(
        topLevelAS,
        cameraRayFlags,
        RAY_MASK_CAMERA,
        2,
        3,
        2,
        origin,
        tMin,
        direction,
        tMax,
        2);
    return terminal_payload;
}

vec3 shadow_transmittance(vec3 origin, vec3 direction, float tMax, float rayTime) {
    if (trace_shadow(origin, direction, tMax, rayTime)) {
        return vec3(0.0);
    }
    return vec3(homogeneous_transmittance_scalar(tMax));
}

vec3 shadow_origin(RayPayload hit, vec3 wi) {
    vec3 n = normalize(hit.geom_normal);
    float bias = shadow_self_hit_epsilon();
    return hit.world_pos + n * (dot(n, wi) >= 0.0 ? bias : -bias);
}

Material material_for_hit(inout RayPayload hit, vec3 rayDirection) {
    Material material = decode_material(hit.material_id);
    hit.uv = apply_material_height_parallax(material, hit.uv, hit.uv1, hit.normal, hit.tangent, hit.bitangent, rayDirection);
    apply_material_textures(material, hit.uv, hit.uv1);
    apply_material_vertex_color(material, hit.vertex_color);
    hit.normal = apply_normal_texture(material, hit.uv, hit.uv1, hit.normal, hit.tangent, hit.bitangent, rayDirection);
    apply_clearcoat_normal_texture(material, hit.uv, hit.uv1, hit.normal, hit.tangent, hit.bitangent, rayDirection);
    return apply_debug_material_mode(material);
}

Material terminal_material_for_hit_fast(inout RayPayload hit, vec3 rayDirection) {
    Material material = decode_material(hit.material_id);
    apply_terminal_material_textures(material, hit.uv, hit.uv1);
    apply_material_vertex_color(material, hit.vertex_color);
    material.normal_variance = 0.0;
    material.clearcoat_normal = hit.normal;
    material.clearcoat_normal_variance = 0.0;
    return apply_debug_material_mode(material);
}

vec3 caustic_shadow_transmittance_stats(
    vec3 origin,
    vec3 direction,
    float tMax,
    float rayTime,
    out uint transmissiveHits,
    out uint visiblePath,
    out uint blockedPath) {
    transmissiveHits = 0u;
    visiblePath = 0u;
    blockedPath = 0u;
    bool causticDiagnosticsEnabled = camera.path_trace_controls.w != 0u;
    record_rt_counter(RT_DIAG_CAUSTIC_SHADOW_ATTEMPTS);
    if (mesh_params.transmissive_shadow_caster_count == 0u && !homogeneous_volume_enabled() && !causticDiagnosticsEnabled) {
        record_rt_counter(RT_DIAG_FAST_SHADOW_TRANSMITTANCE_USED);
        if (trace_shadow(origin, normalize(direction), tMax, rayTime)) {
            blockedPath = 1u;
            return vec3(0.0);
        }
        return vec3(1.0);
    }
    record_rt_counter(RT_DIAG_FULL_SHADOW_TRANSMITTANCE_USED);

    vec3 throughput = vec3(1.0);
    vec3 rayOrigin = origin;
    vec3 rayDir = normalize(direction);
    float remaining = tMax;
    for (uint interfaceIndex = 0u; interfaceIndex < 2u; ++interfaceIndex) {
        record_rt_counter(RT_DIAG_TRANSMISSIVE_SHADOW_SURFACE_TRACES);
        RayPayload shadowHit = trace_shadow_surface(rayOrigin, rayDir, shadow_self_hit_epsilon(), remaining, rayTime);
        if (shadowHit.hit == 0u) {
            if (transmissiveHits > 0u) {
                if (causticDiagnosticsEnabled) {
                    record_rt_counter(RT_DIAG_CAUSTIC_TRANSMISSIVE_VISIBLE);
                }
                visiblePath = 1u;
            }
            return throughput * vec3(homogeneous_transmittance_scalar(remaining));
        }

        Material material = material_for_hit(shadowHit, rayDir);
        if (!material_is_transmissive(material)) {
            MaterialRuntimeHeader blockerHeader = decode_material_runtime_header(shadowHit.material_id);
            record_rt_alpha_class_counter(
                blockerHeader.alpha_mode,
                RT_DIAG_CAUSTIC_BLOCKER_OPAQUE,
                RT_DIAG_CAUSTIC_BLOCKER_ALPHA_TESTED,
                RT_DIAG_CAUSTIC_BLOCKER_BLENDED);
            if (causticDiagnosticsEnabled) {
                record_rt_counter(RT_DIAG_CAUSTIC_SHADOW_BLOCKED);
            }
            blockedPath = 1u;
            return vec3(0.0);
        }
        transmissiveHits += 1u;
        if (causticDiagnosticsEnabled) {
            record_rt_counter(RT_DIAG_CAUSTIC_TRANSMISSIVE_HITS);
        }

        vec3 dispersionIor = material_dispersion_ior(material);
        vec3 etaRgb = shadowHit.front_face != 0u ? (vec3(1.0) / dispersionIor) : dispersionIor;
        float eta = etaRgb.g;
        float cosTheta = min(dot(-rayDir, shadowHit.normal), 1.0);
        float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
        if (eta * sinTheta > 1.0) {
            if (causticDiagnosticsEnabled) {
                record_rt_counter(RT_DIAG_CAUSTIC_SHADOW_BLOCKED);
            }
            blockedPath = 1u;
            return vec3(0.0);
        }

        float surfaceT = homogeneous_transmittance_scalar(shadowHit.t) * material_effective_transmission(material);
        vec3 fresnel = vec3(
            reflectance(cosTheta, etaRgb.r),
            reflectance(cosTheta, etaRgb.g),
            reflectance(cosTheta, etaRgb.b));
        vec3 volumeT = shadowHit.front_face == 0u
            ? material_volume_transmittance(material, shadowHit.t)
            : vec3(1.0);
        throughput *= material.color * surfaceT * volumeT * (vec3(1.0) - fresnel);
        remaining = max(remaining - shadowHit.t, 0.0);
        if (remaining <= shadow_distance_bias() || max(max(throughput.r, throughput.g), throughput.b) <= 1.0e-5) {
            if (causticDiagnosticsEnabled) {
                record_rt_counter(RT_DIAG_CAUSTIC_SHADOW_BLOCKED);
            }
            blockedPath = 1u;
            return vec3(0.0);
        }

        rayDir = normalize(refract(rayDir, shadowHit.normal, eta));
        rayOrigin = shadowHit.world_pos + (dot(rayDir, shadowHit.normal) > 0.0 ? shadowHit.normal : -shadowHit.normal) * shadow_self_hit_epsilon();
    }

    bool blocked = trace_shadow(rayOrigin, rayDir, remaining, rayTime);
    if (blocked) {
        if (causticDiagnosticsEnabled) {
            record_rt_counter(RT_DIAG_CAUSTIC_SHADOW_BLOCKED);
        }
        blockedPath = 1u;
        return vec3(0.0);
    }
    if (transmissiveHits > 0u) {
        if (causticDiagnosticsEnabled) {
            record_rt_counter(RT_DIAG_CAUSTIC_TRANSMISSIVE_VISIBLE);
        }
        visiblePath = 1u;
    }
    return throughput * vec3(homogeneous_transmittance_scalar(remaining));
}

vec3 caustic_shadow_transmittance(vec3 origin, vec3 direction, float tMax, float rayTime) {
    uint transmissiveHits;
    uint visiblePath;
    uint blockedPath;
    return caustic_shadow_transmittance_stats(origin, direction, tMax, rayTime, transmissiveHits, visiblePath, blockedPath);
}

bool direct_shadow_fast_visibility_supported(Material receiverMaterial) {
#if RTV_NATIVE2B_PIPELINE
    // Native2B terminal direct lighting fires many short-lived visibility rays.
    // A scene-global transmissive-caster count is too blunt for those rays: a few
    // glass/blended primitives should not force every opaque receiver through the
    // full transmissive closest-hit loop. Keep the exact transmittance path for
    // transmissive receivers, volumes, caustic diagnostics, debug modes, and
    // when the Native2B final-bounce fast path is explicitly disabled.
    return !material_is_transmissive(receiverMaterial) &&
        (camera.path_trace_controls.z & 8u) != 0u &&
        !homogeneous_volume_enabled() &&
        camera.path_trace_controls.w == 0u &&
        debug_params.view != 27u;
#else
    return false;
#endif
}

vec3 direct_shadow_transmittance_stats(
    vec3 origin,
    vec3 direction,
    float tMax,
    float rayTime,
    Material receiverMaterial,
    out uint transmissiveHits,
    out uint visiblePath,
    out uint blockedPath) {
    transmissiveHits = 0u;
    visiblePath = 0u;
    blockedPath = 0u;
    if (direct_shadow_fast_visibility_supported(receiverMaterial)) {
        record_rt_counter(RT_DIAG_CAUSTIC_SHADOW_ATTEMPTS);
        record_rt_counter(RT_DIAG_FAST_SHADOW_TRANSMITTANCE_USED);
        if (trace_shadow(origin, normalize(direction), tMax, rayTime)) {
            blockedPath = 1u;
            return vec3(0.0);
        }
        return vec3(1.0);
    }
    return caustic_shadow_transmittance_stats(
        origin,
        direction,
        tMax,
        rayTime,
        transmissiveHits,
        visiblePath,
        blockedPath);
}


#endif // RTV_PATHTRACE_RAY_QUERIES_GLSL
