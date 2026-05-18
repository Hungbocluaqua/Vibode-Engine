#version 460
#extension GL_GOOGLE_include_directive : require
#include "rt_common.glsl"

layout(location = 1) rayPayloadInEXT uint shadow_occluded;

void main() {
    shadow_occluded = 0u;
}
