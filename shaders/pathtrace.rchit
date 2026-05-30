#version 460
#extension GL_GOOGLE_include_directive : require
#include "rt_common.glsl"

hitAttributeEXT vec2 attribs;
layout(location = 0) rayPayloadInEXT RayPayload payload;

void main() {
    record_rt_counter(RT_DIAG_CLOSEST_HIT_INVOCATIONS);
    uint instanceIndex = gl_InstanceCustomIndexEXT;
    if (instanceIndex >= mesh_params.instance_count) {
        payload.hit = 1u;
        payload.t = gl_HitTEXT;
        payload.normal = vec3(0.0, 1.0, 0.0);
        payload.geom_normal = payload.normal;
        payload.material_id = 0u;
        payload.tangent = vec3(1.0, 0.0, 0.0);
        payload.bitangent = vec3(0.0, 0.0, 1.0);
        return;
    }

    InstanceRecord instance = instance_records[instanceIndex];
    uint meshIndex = instance.metadata.x;
    MeshRecord mesh = mesh_records[meshIndex];
    uint firstIndex = mesh.vertex_index_data.z;
    uint globalTriangleIndex = geometry_triangle_offset(meshIndex, gl_GeometryIndexEXT, firstIndex) + gl_PrimitiveID;
    uint triIndex = globalTriangleIndex * 3u;
    uint i0 = local_mesh_indices[triIndex + 0u];
    uint i1 = local_mesh_indices[triIndex + 1u];
    uint i2 = local_mesh_indices[triIndex + 2u];

    LocalVertex v0 = local_mesh_vertices[i0];
    LocalVertex v1 = local_mesh_vertices[i1];
    LocalVertex v2 = local_mesh_vertices[i2];
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
    vec3 localTangent = normalize(v0.tangent.xyz * bary.x + v1.tangent.xyz * bary.y + v2.tangent.xyz * bary.z);
    float tangentSign = v0.tangent.w * bary.x + v1.tangent.w * bary.y + v2.tangent.w * bary.z;
    vec3 worldTangent = normalize(mat3(instance.transform) * localTangent);
    vec3 worldBitangent = normalize(cross(worldNormal, worldTangent) * (tangentSign < 0.0 ? -1.0 : 1.0));

    uint materialIndex = material_for_triangle_index(globalTriangleIndex);
    Material materialForDiagnostics = decode_material(materialIndex);
    if (materialForDiagnostics.alpha_mode != ALPHA_MODE_OPAQUE) {
        record_rt_counter(RT_DIAG_CLOSEST_HIT_ALPHA_MATERIAL);
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
    payload.uv = uv;
    payload.tangent = worldTangent;
    payload.bitangent = worldBitangent;
}
