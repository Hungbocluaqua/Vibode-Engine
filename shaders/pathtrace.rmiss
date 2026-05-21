#version 460
#extension GL_GOOGLE_include_directive : require
#include "rt_common.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;

void main() {
    payload.t = 10000.0;
    payload.hit = 0u;
    payload.normal = -gl_WorldRayDirectionEXT;
    payload.geom_normal = -gl_WorldRayDirectionEXT;
    payload.world_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * 10000.0;
    payload.material_id = 0u;
    payload.front_face = 1u;
    payload.instance_id = 0xffffffffu;
    payload.mesh_id = 0xffffffffu;
    payload.primitive_id = 0xffffffffu;
    payload.picking = 0u;
    payload.uv = vec2(0.0);
    payload.tangent = vec3(1.0, 0.0, 0.0);
    payload.bitangent = vec3(0.0, 0.0, 1.0);
}
