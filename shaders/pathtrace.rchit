#version 460
#extension GL_GOOGLE_include_directive : require
#include "rt_common.glsl"

hitAttributeEXT vec2 attribs;
layout(location = 0) rayPayloadInEXT RayPayload payload;

void main() {
    record_rt_counter(RT_DIAG_CLOSEST_HIT_INVOCATIONS);
    record_rt_counter(RT_DIAG_CLOSEST_HIT_PRIMARY);
    uint tlasRecordIndex = gl_InstanceCustomIndexEXT;
    uint instanceIndex = scene_instance_index_from_tlas_record(tlasRecordIndex);
    if (instanceIndex >= mesh_params.instance_count) {
        payload.hit = 1u;
        payload.t = gl_HitTEXT;
        payload.normal = vec3(0.0, 1.0, 0.0);
        payload.geom_normal = payload.normal;
        payload.material_id = 0u;
        payload.tangent = vec3(1.0, 0.0, 0.0);
        payload.bitangent = vec3(0.0, 0.0, 1.0);
        payload.barycentrics = vec3(1.0, 0.0, 0.0);
        payload.uv = vec2(0.0);
        payload.uv1 = vec2(0.0);
        payload.vertex_color = vec4(1.0);
        return;
    }

    InstanceRecord instance = instance_records[instanceIndex];
    uint meshIndex = instance.metadata.x;
    MeshRecord mesh = mesh_records[meshIndex];
    uint firstIndex = mesh.vertex_index_data.z;
    uint globalTriangleIndex = geometry_triangle_offset(meshIndex, tlasRecordIndex, gl_GeometryIndexEXT, firstIndex) + gl_PrimitiveID;
    uvec3 indices;
    if (!ray_tracing_triangle_indices(globalTriangleIndex, indices)) {
        payload.hit = 0u;
        return;
    }

    uvec4 skinningBinding = ray_tracing_gpu_skinning_binding(meshIndex);
    LocalVertex v0 = ray_tracing_local_vertex_with_binding(skinningBinding, indices.x);
    LocalVertex v1 = ray_tracing_local_vertex_with_binding(skinningBinding, indices.y);
    LocalVertex v2 = ray_tracing_local_vertex_with_binding(skinningBinding, indices.z);
    vec3 p0 = v0.position_uv_x.xyz;
    vec3 p1 = v1.position_uv_x.xyz;
    vec3 p2 = v2.position_uv_x.xyz;

    vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    vec3 localNormal = normalize(v0.normal_uv_y.xyz * bary.x + v1.normal_uv_y.xyz * bary.y + v2.normal_uv_y.xyz * bary.z);
    vec3 localGeomNormal = normalize(cross(p1 - p0, p2 - p0));
    vec3 worldNormal = normalize(mat3(instance.normal_transform) * localNormal);
    vec3 worldGeomNormal = normalize(mat3(instance.normal_transform) * localGeomNormal);
    bool frontFace = dot(worldGeomNormal, gl_WorldRayDirectionEXT) < 0.0;
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
    vec4 vertexColor = clamp(v0.color * bary.x + v1.color * bary.y + v2.color * bary.z, vec4(0.0), vec4(1.0));

    uint materialIndex = material_for_triangle_index(globalTriangleIndex);
    MaterialRuntimeHeader materialHeader = decode_material_runtime_header(materialIndex);
    if (materialHeader.alpha_mode != ALPHA_MODE_OPAQUE) {
        record_rt_counter(RT_DIAG_CLOSEST_HIT_ALPHA_MATERIAL);
        record_rt_alpha_material_counter(materialIndex, RT_ALPHA_MATERIAL_COUNTER_CLOSEST_HIT);
    }

    vec3 localPos = p0 * bary.x + p1 * bary.y + p2 * bary.z;
    vec3 worldPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

    payload.hit = 1u;
    payload.t = gl_HitTEXT;
    payload.world_pos = worldPos;
    payload.material_id = materialIndex;
    payload.local_pos = localPos;
    payload.geom_normal = worldGeomNormal;
    payload.front_face = frontFace ? 1u : 0u;
    payload.normal = worldNormal;
    payload.instance_id = instanceIndex;
    payload.mesh_id = meshIndex;
    payload.primitive_id = globalTriangleIndex;
    payload.barycentrics = bary;
    payload.uv = uv;
    payload.uv1 = uv1;
    payload.tangent = worldTangent;
    payload.bitangent = worldBitangent;
    payload.vertex_color = vertexColor;
}
