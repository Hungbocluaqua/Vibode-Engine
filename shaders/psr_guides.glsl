#ifndef RTV_PSR_GUIDES_GLSL
#define RTV_PSR_GUIDES_GLSL

struct PsrGuideRecord {
    // xy = packed camera-relative world position, z = packed normal, w = packed motion.
    uvec4 geometry;
    // x = diffuse albedo, y = specular F0, z = packed ray direction, w = flags/roughness/identity.
    uvec4 material;
    // x = reflected segment hit distance, y = replacement viewZ, z = primary hit distance.
    vec4 distances;
};

const uint PSR_FLAG_ACTIVE = 1u << 0u;
const uint PSR_FLAG_VALID = 1u << 1u;

vec2 psr_oct_encode(vec3 value) {
    vec3 n = normalize(value);
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
}

vec3 psr_oct_decode(vec2 value) {
    vec3 n = vec3(value, 1.0 - abs(value.x) - abs(value.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}

uint psr_pack_direction(vec3 direction) {
    return packSnorm2x16(psr_oct_encode(direction));
}

vec3 psr_unpack_direction(uint packed) {
    return psr_oct_decode(unpackSnorm2x16(packed));
}

uint psr_pack_metadata(bool valid, bool isActive, float roughness, uint identityHash) {
    uint packedRoughness = uint(round(clamp(roughness, 0.0, 1.0) * 255.0));
    return (isActive ? PSR_FLAG_ACTIVE : 0u) |
        (valid ? PSR_FLAG_VALID : 0u) |
        (packedRoughness << 2u) |
        ((identityHash & 0x003fffffu) << 10u);
}

bool psr_guide_valid(PsrGuideRecord guide) {
    return (guide.material.w & PSR_FLAG_VALID) != 0u;
}

bool psr_guide_active(PsrGuideRecord guide) {
    return (guide.material.w & PSR_FLAG_ACTIVE) != 0u;
}

float psr_guide_roughness(PsrGuideRecord guide) {
    return float((guide.material.w >> 2u) & 0xffu) / 255.0;
}

uint psr_guide_identity(PsrGuideRecord guide) {
    return guide.material.w >> 10u;
}

uint psr_guide_signature(PsrGuideRecord guide) {
    if (!psr_guide_valid(guide)) {
        return 0u;
    }
    uint signature = (psr_guide_identity(guide) << 2u) | 0x1u;
    if (psr_guide_active(guide)) {
        signature |= 0x2u;
    }
    return signature;
}

vec3 psr_guide_normal(PsrGuideRecord guide) {
    return psr_unpack_direction(guide.geometry.z);
}

vec3 psr_guide_diffuse_albedo(PsrGuideRecord guide) {
    return unpackUnorm4x8(guide.material.x).rgb;
}

vec3 psr_guide_specular_f0(PsrGuideRecord guide) {
    return unpackUnorm4x8(guide.material.y).rgb;
}

vec3 psr_guide_ray_direction(PsrGuideRecord guide) {
    return psr_unpack_direction(guide.material.z);
}

vec2 psr_guide_motion_pixels(PsrGuideRecord guide) {
    return unpackSnorm2x16(guide.geometry.w) * 512.0;
}

PsrGuideRecord psr_invalid_guide() {
    PsrGuideRecord guide;
    guide.geometry = uvec4(0u);
    guide.material = uvec4(0u);
    guide.distances = vec4(65504.0, 65504.0, 65504.0, 0.0);
    return guide;
}

#endif // RTV_PSR_GUIDES_GLSL
