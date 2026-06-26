#version 460
#extension GL_GOOGLE_include_directive : require
#include "rt_common.glsl"

layout(location = 2) rayPayloadInEXT TerminalRayPayload payload;

void main() {
    payload.hit = 0u;
    payload.t = 10000.0;
    payload.geom_normal = -gl_WorldRayDirectionEXT;
    payload.normal = payload.geom_normal;
    payload.tangent = vec3(1.0, 0.0, 0.0);
    payload.bitangent = vec3(0.0, 0.0, 1.0);
    payload.material_id = 0u;
    payload.uv = vec2(0.0);
    payload.uv1 = vec2(0.0);
    payload.vertex_color = vec4(1.0);
}
