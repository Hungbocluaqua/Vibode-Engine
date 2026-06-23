// Shared full-resolution receiver ABI for ReSTIR GI compute passes.

#ifndef RTV_RESTIR_GI_COMMON_GLSL
#define RTV_RESTIR_GI_COMMON_GLSL

#ifndef RTV_RESTIR_GI_RECEIVER_DEFINED
#define RTV_RESTIR_GI_RECEIVER_DEFINED
struct RestirGiReceiver {
    vec4 positionDepth;
    vec4 normalRoughness;
    vec4 geometryNormalMetal;
    vec4 albedoOcclusion;
    uvec4 materialIds;
    uvec4 motion;
};
#endif

vec3 restir_gi_receiver_position(RestirGiReceiver r) { return r.positionDepth.xyz; }
float restir_gi_receiver_depth(RestirGiReceiver r) { return r.positionDepth.w; }
vec3 restir_gi_receiver_normal(RestirGiReceiver r) { return normalize(r.normalRoughness.xyz); }
float restir_gi_receiver_roughness(RestirGiReceiver r) { return clamp(r.normalRoughness.w, 0.0, 1.0); }
vec3 restir_gi_receiver_geometry_normal(RestirGiReceiver r) { return normalize(r.geometryNormalMetal.xyz); }
float restir_gi_receiver_metallic(RestirGiReceiver r) { return clamp(r.geometryNormalMetal.w, 0.0, 1.0); }
vec3 restir_gi_receiver_albedo(RestirGiReceiver r) { return max(r.albedoOcclusion.rgb, vec3(0.0)); }
float restir_gi_receiver_occlusion(RestirGiReceiver r) { return clamp(r.albedoOcclusion.a, 0.0, 1.0); }
uint restir_gi_receiver_material_id(RestirGiReceiver r) { return r.materialIds.x; }
uint restir_gi_receiver_instance_id(RestirGiReceiver r) { return r.materialIds.y; }
uint restir_gi_receiver_mesh_id(RestirGiReceiver r) { return r.materialIds.z; }
uint restir_gi_receiver_primitive_id(RestirGiReceiver r) { return r.materialIds.w; }
bool restir_gi_receiver_camera_cut(RestirGiReceiver r) { return r.motion.y != 0u; }
uint restir_gi_receiver_object_version(RestirGiReceiver r) { return r.motion.z; }
uint restir_gi_receiver_material_version(RestirGiReceiver r) { return r.motion.w; }

RestirGiReceiver empty_restir_gi_receiver() {
    RestirGiReceiver r;
    r.positionDepth = vec4(0.0, 0.0, 0.0, 65504.0);
    r.normalRoughness = vec4(0.0, 1.0, 0.0, 1.0);
    r.geometryNormalMetal = vec4(0.0, 1.0, 0.0, 0.0);
    r.albedoOcclusion = vec4(0.0);
    r.materialIds = uvec4(0xffffffffu);
    r.motion = uvec4(0u);
    return r;
}

vec2 restir_unpack_velocity_pixels(uint packed) {
    ivec2 encoded = ivec2(packed & 0xffffu, (packed >> 16u) & 0xffffu);
    if (encoded.x >= 32768) encoded.x -= 65536;
    if (encoded.y >= 32768) encoded.y -= 65536;
    return vec2(encoded) * (512.0 / 32767.0);
}

#endif
