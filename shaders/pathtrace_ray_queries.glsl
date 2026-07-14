#ifndef RTV_PATHTRACE_RAY_QUERIES_GLSL
#define RTV_PATHTRACE_RAY_QUERIES_GLSL

#ifndef RTV_GENERIC_EXACT_SHADOW_RAY_QUERY
#define RTV_GENERIC_EXACT_SHADOW_RAY_QUERY 1
#endif

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

#if RTV_GENERIC_EXACT_SHADOW_RAY_QUERY && !RTV_NATIVE2B_PIPELINE
RayPayload ray_query_surface_payload_from_parts(
    float t,
    uint tlasRecordIndex,
    uint geometryIndex,
    uint primitiveIndex,
    vec2 bary2,
    vec3 origin,
    vec3 direction) {
    RayPayload hit;
    hit.hit = 1u;
    hit.t = t;
    hit.world_pos = origin + direction * hit.t;
    hit.material_id = 0u;
    hit.local_pos = vec3(0.0);
    hit.geom_normal = vec3(0.0, 1.0, 0.0);
    hit.front_face = 1u;
    hit.normal = vec3(0.0, 1.0, 0.0);
    hit.instance_id = 0xffffffffu;
    hit.mesh_id = 0xffffffffu;
    hit.primitive_id = 0xffffffffu;
    hit.picking = 0u;
    hit.barycentrics = vec3(1.0, 0.0, 0.0);
    hit.uv = vec2(0.0);
    hit.uv1 = vec2(0.0);
    hit.tangent = vec3(1.0, 0.0, 0.0);
    hit.bitangent = vec3(0.0, 0.0, 1.0);
    hit.vertex_color = vec4(1.0);

    uint instanceIndex = scene_instance_index_from_tlas_record(tlasRecordIndex);
    if (instanceIndex >= mesh_params.instance_count) {
        return hit;
    }

    InstanceRecord instance = instance_records[instanceIndex];
    uint meshIndex = instance.metadata.x;
    MeshRecord mesh = mesh_records[meshIndex];
    uint firstIndex = mesh.vertex_index_data.z;
    uint globalTriangleIndex = geometry_triangle_offset(meshIndex, tlasRecordIndex, geometryIndex, firstIndex) + primitiveIndex;
    uvec3 indices;
    if (!ray_tracing_triangle_indices(globalTriangleIndex, indices)) {
        hit.hit = 0u;
        return hit;
    }

    uvec4 skinningBinding = ray_tracing_gpu_skinning_binding(meshIndex);
    LocalVertex v0 = ray_tracing_local_vertex_with_binding(skinningBinding, indices.x);
    LocalVertex v1 = ray_tracing_local_vertex_with_binding(skinningBinding, indices.y);
    LocalVertex v2 = ray_tracing_local_vertex_with_binding(skinningBinding, indices.z);
    vec3 p0 = v0.position_uv_x.xyz;
    vec3 p1 = v1.position_uv_x.xyz;
    vec3 p2 = v2.position_uv_x.xyz;

    vec3 bary = vec3(1.0 - bary2.x - bary2.y, bary2.x, bary2.y);
    vec3 localNormal = normalize(v0.normal_uv_y.xyz * bary.x + v1.normal_uv_y.xyz * bary.y + v2.normal_uv_y.xyz * bary.z);
    vec3 localGeomNormal = normalize(cross(p1 - p0, p2 - p0));
    vec3 worldNormal = normalize(mat3(instance.normal_transform) * localNormal);
    vec3 worldGeomNormal = normalize(mat3(instance.normal_transform) * localGeomNormal);
    bool frontFace = dot(worldGeomNormal, direction) < 0.0;
    if (!frontFace) {
        worldGeomNormal = -worldGeomNormal;
        worldNormal = -worldNormal;
    }

    vec2 uv = vec2(
        v0.position_uv_x.w * bary.x + v1.position_uv_x.w * bary.y + v2.position_uv_x.w * bary.z,
        v0.normal_uv_y.w * bary.x + v1.normal_uv_y.w * bary.y + v2.normal_uv_y.w * bary.z);
    vec2 uv1 = v0.texcoord1.xy * bary.x + v1.texcoord1.xy * bary.y + v2.texcoord1.xy * bary.z;
    vec3 localTangent = normalize(v0.tangent.xyz * bary.x + v1.tangent.xyz * bary.y + v2.tangent.xyz * bary.z);
    float tangentSign = v0.tangent.w * bary.x + v1.tangent.w * bary.y + v2.tangent.w * bary.z;
    vec3 worldTangent = normalize(mat3(instance.transform) * localTangent);
    vec3 worldBitangent = normalize(cross(worldNormal, worldTangent) * (tangentSign < 0.0 ? -1.0 : 1.0));

    hit.material_id = material_for_triangle_index(globalTriangleIndex);
    hit.local_pos = p0 * bary.x + p1 * bary.y + p2 * bary.z;
    hit.geom_normal = worldGeomNormal;
    hit.front_face = frontFace ? 1u : 0u;
    hit.normal = worldNormal;
    hit.instance_id = instanceIndex;
    hit.mesh_id = meshIndex;
    hit.primitive_id = globalTriangleIndex;
    hit.barycentrics = bary;
    hit.uv = uv;
    hit.uv1 = uv1;
    hit.tangent = worldTangent;
    hit.bitangent = worldBitangent;
    hit.vertex_color = clamp(v0.color * bary.x + v1.color * bary.y + v2.color * bary.z, vec4(0.0), vec4(1.0));
    return hit;
}

RayPayload ray_query_candidate_surface_payload(rayQueryEXT rayQuery, vec3 origin, vec3 direction) {
    return ray_query_surface_payload_from_parts(
        rayQueryGetIntersectionTEXT(rayQuery, false),
        rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false),
        rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false),
        rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false),
        rayQueryGetIntersectionBarycentricsEXT(rayQuery, false),
        origin,
        direction);
}

uint ray_query_material_index(uint tlasRecordIndex, uint geometryIndex, uint primitiveIndex) {
    if (mesh_params.instance_count == 1u && mesh_params.mesh_count == 1u &&
        tlasRecordIndex == 0u && geometryIndex == 0u) {
        uint triangleOffset = rt_geometry_triangle_offsets.length() > 0
            ? rt_geometry_triangle_offsets[0]
            : mesh_records[0].vertex_index_data.z / 3u;
        return material_for_triangle_index(triangleOffset + primitiveIndex);
    }

    uint instanceIndex = scene_instance_index_from_tlas_record(tlasRecordIndex);
    if (instanceIndex >= mesh_params.instance_count) {
        return 0u;
    }

    uint meshIndex = instance_records[instanceIndex].metadata.x;
    uint firstIndex = mesh_records[meshIndex].vertex_index_data.z;
    uint globalTriangleIndex = geometry_triangle_offset(meshIndex, tlasRecordIndex, geometryIndex, firstIndex) + primitiveIndex;
    return material_for_triangle_index(globalTriangleIndex);
}

bool ray_query_shadow_candidate_accepted(rayQueryEXT rayQuery, vec3 origin, vec3 direction) {
    uint tlasRecordIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
    uint geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
    uint primitiveIndex = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
    uint materialIndex = ray_query_material_index(tlasRecordIndex, geometryIndex, primitiveIndex);
    MaterialRuntimeHeader materialHeader = decode_material_runtime_header(materialIndex);
    if (materialHeader.double_sided == 0u &&
        !rayQueryGetIntersectionFrontFaceEXT(rayQuery, false)) {
        return false;
    }
    if (materialHeader.alpha_mode == ALPHA_MODE_OPAQUE) {
        return true;
    }

    RayPayload candidate = ray_query_candidate_surface_payload(rayQuery, origin, direction);
    if (candidate.hit == 0u) {
        return true;
    }
    Material material = decode_material(materialIndex);
    apply_material_alpha_texture(material, candidate.uv, candidate.uv1);
    material.alpha_factor *= candidate.vertex_color.a;
    return accept_material_alpha(material);
}

struct ShadowSurfaceHit {
    uint hit;
    float t;
    uint material_id;
    uint tlas_record_index;
    uint geometry_index;
    uint primitive_index;
    vec2 barycentrics;
};

ShadowSurfaceHit shadow_surface_miss() {
    ShadowSurfaceHit hit;
    hit.hit = 0u;
    hit.t = 10000.0;
    hit.material_id = 0u;
    hit.tlas_record_index = 0xffffffffu;
    hit.geometry_index = 0u;
    hit.primitive_index = 0u;
    hit.barycentrics = vec2(0.0);
    return hit;
}

RayPayload shadow_surface_payload(ShadowSurfaceHit compactHit, vec3 origin, vec3 direction) {
    return ray_query_surface_payload_from_parts(
        compactHit.t,
        compactHit.tlas_record_index,
        compactHit.geometry_index,
        compactHit.primitive_index,
        compactHit.barycentrics,
        origin,
        direction);
}
#endif

#if RTV_GENERIC_EXACT_SHADOW_RAY_QUERY && !RTV_MOTION_BLUR_ENABLED && !RTV_NATIVE2B_PIPELINE
ShadowSurfaceHit trace_shadow_surface_exact(vec3 origin, vec3 direction, float tMin, float tMax) {
    record_rt_counter(RT_DIAG_SURFACE_TRACE_RAYS);
    record_rt_counter(RT_DIAG_SHADOW_SURFACE_TRACE_RAYS);
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(
        rayQuery,
        topLevelAS,
        gl_RayFlagsNoneEXT,
        RAY_MASK_SHADOW,
        origin,
        tMin,
        direction,
        tMax);
    while (rayQueryProceedEXT(rayQuery)) {
        if (rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            ray_query_shadow_candidate_accepted(rayQuery, origin, direction)) {
            rayQueryConfirmIntersectionEXT(rayQuery);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        return shadow_surface_miss();
    }

    ShadowSurfaceHit hit;
    hit.hit = 1u;
    hit.t = rayQueryGetIntersectionTEXT(rayQuery, true);
    hit.tlas_record_index = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true);
    hit.geometry_index = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, true);
    hit.primitive_index = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
    hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    hit.material_id = ray_query_material_index(hit.tlas_record_index, hit.geometry_index, hit.primitive_index);
    return hit;
}
#endif

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

#if RTV_NATIVE2B_PIPELINE
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
#endif

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
#if RTV_GENERIC_EXACT_SHADOW_RAY_QUERY && !RTV_MOTION_BLUR_ENABLED && !RTV_NATIVE2B_PIPELINE
        ShadowSurfaceHit compactShadowHit = trace_shadow_surface_exact(
            rayOrigin,
            rayDir,
            shadow_self_hit_epsilon(),
            remaining);
        if (compactShadowHit.hit == 0u) {
#else
        RayPayload shadowHit = trace_shadow_surface(rayOrigin, rayDir, shadow_self_hit_epsilon(), remaining, rayTime);
        if (shadowHit.hit == 0u) {
#endif
            if (transmissiveHits > 0u) {
                if (causticDiagnosticsEnabled) {
                    record_rt_counter(RT_DIAG_CAUSTIC_TRANSMISSIVE_VISIBLE);
                }
                visiblePath = 1u;
            }
            return throughput * vec3(homogeneous_transmittance_scalar(remaining));
        }

#if RTV_GENERIC_EXACT_SHADOW_RAY_QUERY && !RTV_MOTION_BLUR_ENABLED && !RTV_NATIVE2B_PIPELINE
        if (!material_static_may_be_transmissive(compactShadowHit.material_id)) {
            MaterialRuntimeHeader blockerHeader = decode_material_runtime_header(compactShadowHit.material_id);
#else
        if (!material_static_may_be_transmissive(shadowHit.material_id)) {
            MaterialRuntimeHeader blockerHeader = decode_material_runtime_header(shadowHit.material_id);
#endif
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

#if RTV_GENERIC_EXACT_SHADOW_RAY_QUERY && !RTV_MOTION_BLUR_ENABLED && !RTV_NATIVE2B_PIPELINE
        RayPayload shadowHit = shadow_surface_payload(compactShadowHit, rayOrigin, rayDir);
#endif
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
        renderer_debug_view() != 27u;
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
