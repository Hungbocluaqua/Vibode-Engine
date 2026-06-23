#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require

#include "atmosphere_phase.glsl"
#define RTV_STBN_TEXTURE_ENABLED 1
#include "blue_noise.glsl"

#ifndef RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
#define RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT 0
#endif

layout(set = 0, binding = 0, std430) buffer AccumulationBuffer { vec4 accumulation_buffer[]; };

layout(set = 0, binding = 1, std140) uniform Camera {
    vec4 pos;
    vec4 forward;
    vec4 right;
    vec4 up;
    uint frame_count;
    uint temporal_frame_index;
    float effective_jitter_scale;
    uint camera_moving;
    float sun_intensity;
    float sky_intensity;
    float exposure;
    uint path_tracing_enabled;
    uint max_bounces;
    uint sunlight_enabled;
    uint direct_lighting_enabled;
    float fov_y;
    float sun_angular_radius;
    float indirect_strength;
    uint environment_direct_samples;
    vec4 jitter;
    vec4 atmosphere;
    vec4 render_controls;
    vec4 sun_direction_illuminance;
    vec4 sun_color_angular_radius;
    uvec4 restir_gi_controls;
    uvec4 path_trace_controls;
    vec4 dof_controls;
    vec4 motion_blur_controls;
    vec4 volume_controls;
    vec4 projection_controls;
    vec4 clip_controls;
    uvec4 gi_version_controls;   // Phase 8: x = light, y = material, z = object, w = environment version
    uvec4 restir_di_controls;     // x = new ReSTIR DI raygen writes enabled
} camera;

layout(set = 0, binding = 2, std430) buffer VarianceBuffer { uint variance_buffer[]; };
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D output_color;
layout(set = 0, binding = 4, std430) buffer DepthNormalBuffer { uvec4 depth_normal_buffer[]; };
layout(set = 0, binding = 5, std430) buffer WorldPositionBuffer { uvec2 world_position_buffer[]; };
layout(set = 0, binding = 35, std430) buffer EntityIdBuffer { uint entity_id_buffer[]; };
layout(set = 0, binding = 36, std430) buffer VelocityBuffer { uint velocity_buffer[]; };
#ifndef RTV_PATH_DATA_COMMON_GLSL
#define RTV_PATH_DATA_COMMON_GLSL
struct PathDataRecord {
    vec4 direct_diffuse;
    vec4 direct_specular;
    vec4 indirect_diffuse;
    vec4 indirect_specular;
    vec4 albedo_roughness_hit_confidence;
    vec4 material_specular_albedo;
    vec4 denoiser_hit_distance;
    vec4 diffuse_ray_direction_hit_distance;
    vec4 specular_ray_direction_hit_distance;
    vec4 emissive_residual;
    vec4 restir_gi_fallback_reactive;
};
#endif
layout(set = 0, binding = 42, std430) buffer PathDataBuffer { PathDataRecord path_data_buffer[]; };
layout(set = 0, binding = 37, std140) uniform PrevCamera {
    mat4 view_proj;
    mat4 inv_view_proj;
    mat4 prev_view_proj;
    vec4 current_pos;
    vec4 prev_pos;
    vec4 jitter;
} prev_camera;
struct RestirReservoir {
    uvec4 metadata; // x = sample type, y = packed source pdf/previous weight, z = age/validity/M, w = temporal compatibility signature
    vec4 sample_value_confidence; // rgb = selected/direct contribution, a = confidence
    vec4 sample_position_distance; // xyz = selected world-space light point or direction, w = current distance/tmax
    vec4 sample_radiance_target; // rgb = emitted radiance/intensity for the selected sample, w = re-evaluated target luminance
    vec4 sample_normal_weight; // xyz = selected light normal for finite area lights, w = reservoir weight sum
    uvec4 sample_metadata; // x = light index, y = light kind, z = receiver material id, w = receiver instance id
};
layout(set = 0, binding = 38, std430) buffer RestirReservoirBuffer { RestirReservoir restir_reservoirs[]; };
layout(set = 0, binding = 39, std430) readonly buffer PreviousRestirReservoirBuffer { RestirReservoir previous_restir_reservoirs[]; };
#ifndef RTV_RESTIR_GI_RESERVOIR_DEFINED
#define RTV_RESTIR_GI_RESERVOIR_DEFINED
struct RestirGiReservoir {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    vec4 hit_position_target_pdf; // xyz = selected indirect hit point, w = target pdf
    vec4 normal_roughness; // xyz = selected hit normal, w = roughness
    vec4 suffix_radiance_source_pdf; // rgb = receiver-independent suffix, w = proposal PDF in selected measure
    vec4 source_direction_distance; // xyz = source receiver direction, w = finite distance or -1 for environment
#endif
    vec4 radiance_weight_sum; // rgb = selected integrand, w = weight sum
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    uvec4 metadata; // x = sample count, y = age, z = flags | path class << 8, w = version hash
#else
    uvec4 metadata; // x = sample/age/flags/roughness, y = oct normal, z = material id, w = fp16(hit distance, target pdf)
#endif
};
#endif

// ReSTIR GI Receiver struct (Phase 2)
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

layout(set = 0, binding = 43, std430) buffer RestirGiReservoirBuffer { RestirGiReservoir restir_gi_reservoirs[]; };
layout(set = 0, binding = 44, std430) readonly buffer PreviousRestirGiReservoirBuffer { RestirGiReservoir previous_restir_gi_reservoirs[]; };
layout(set = 0, binding = 45, std430) buffer RestirGiSpatialReservoirBuffer { RestirGiReservoir restir_gi_spatial_reservoirs[]; };
layout(set = 0, binding = 58, std430) buffer RestirGiReceiverBuffer { RestirGiReceiver restir_gi_receivers[]; };
layout(set = 0, binding = 59, std430) readonly buffer PreviousRestirGiReceiverBuffer { RestirGiReceiver previous_restir_gi_receivers[]; };
// New ReSTIR DI data model (Phase 1-2) — guard macros so restir_di_common.glsl
// (included by compute passes) can define the same structs without conflict.
#ifndef RTV_RESTIR_DI_RECEIVER_DEFINED
#define RTV_RESTIR_DI_RECEIVER_DEFINED
struct RestirDiReceiver {
    vec4 worldPosition_depth;
    vec4 normal_roughness;
#if RTV_RESTIR_DI_VALIDATION_FULL
    vec4 tangent_materialId;
    vec4 bitangent_instanceId;
    vec4 viewDirection_hitDist;
    uvec4 primitive_mesh_flags;
#else
    uvec4 packedMaterialSurface;
#endif
};
#endif
#ifndef RTV_RESTIR_DI_RESERVOIR_DEFINED
#define RTV_RESTIR_DI_RESERVOIR_DEFINED
struct RestirDiReservoir {
    uvec4 sampleMetadata;
    uvec4 reservoirMetadata;
    vec4 samplePosition_distance;
#if RTV_RESTIR_DI_VALIDATION_FULL
    vec4 sampleDirection_pdf;
    vec4 sampleRadiance_target;
    vec4 sampleNormal_weightSum;
    vec4 contribution_confidence;
#endif
};
#endif
#ifndef RTV_RESTIR_DI_SURFACE_CONSTANTS_DEFINED
#define RTV_RESTIR_DI_SURFACE_CONSTANTS_DEFINED
const uint RESTIR_DI_SURFACE_SKY       = 1u << 0u;
const uint RESTIR_DI_SURFACE_INVALID   = 1u << 1u;
const uint RESTIR_DI_SURFACE_DELTA     = 1u << 2u;
const uint RESTIR_DI_SURFACE_ALPHA     = 1u << 3u;
const uint RESTIR_DI_SURFACE_UNLIT     = 1u << 4u;
const uint RESTIR_DI_SURFACE_PBR       = 1u << 5u;
const uint RESTIR_DI_SURFACE_UNSUPPORTED = 1u << 6u;
#endif
#ifndef RTV_RESTIR_DI_VISIBILITY_CONSTANTS_DEFINED
#define RTV_RESTIR_DI_VISIBILITY_CONSTANTS_DEFINED
const uint RESTIR_DI_VISIBILITY_UNKNOWN  = 0u;
const uint RESTIR_DI_VISIBILITY_VISIBLE  = 1u;
const uint RESTIR_DI_VISIBILITY_OCCLUDED = 2u;
const uint RESTIR_DI_VISIBILITY_INVALID  = 3u;
#endif
const uint RESTIR_DI_COUNTER_INITIAL_PIXELS = 0u;
const uint RESTIR_DI_COUNTER_INITIAL_VALID = 1u;
const uint RESTIR_DI_COUNTER_INITIAL_INVALID_SURFACE = 2u;
const uint RESTIR_DI_COUNTER_INITIAL_INVALID_TARGET = 3u;
const uint RESTIR_DI_COUNTER_INITIAL_NON_FINITE = 4u;
const uint RESTIR_DI_COUNTER_INITIAL_EMISSIVE = 5u;
const uint RESTIR_DI_COUNTER_INITIAL_DIRECTIONAL = 6u;
const uint RESTIR_DI_COUNTER_INITIAL_POINT = 7u;
const uint RESTIR_DI_COUNTER_INITIAL_AREA = 8u;
const uint RESTIR_DI_COUNTER_INITIAL_SPOT = 9u;
const uint RESTIR_DI_COUNTER_INITIAL_INVALID_PDF = 10u;
const uint RESTIR_DI_COUNTER_INITIAL_INVALID_IDENTITY = 11u;
const uint RESTIR_DI_COUNTER_INITIAL_WEIGHT_OVERFLOW = 12u;
const uint RESTIR_DI_COUNTER_TEMPORAL_NON_FINITE = 13u;
uint restir_di_pack_radiance(vec3 value) {
    uvec3 encoded = uvec3(round(clamp(log2(vec3(1.0) + max(value, vec3(0.0))) / 16.0, 0.0, 1.0) * 1023.0));
    return encoded.x | (encoded.y << 10u) | (encoded.z << 20u);
}
vec3 restir_di_unpack_radiance(uint packed) {
    uvec3 encoded = uvec3(packed & 1023u, (packed >> 10u) & 1023u, (packed >> 20u) & 1023u);
    return exp2(vec3(encoded) * (16.0 / 1023.0)) - vec3(1.0);
}
layout(set = 0, binding = 52, std430) buffer RestirDiReceiverBuffer { RestirDiReceiver restir_di_receivers[]; };
layout(set = 0, binding = 53, std430) buffer RestirDiInitialReservoirBuffer { RestirDiReservoir restir_di_initial_reservoirs[]; };
layout(set = 0, binding = 54, std430) readonly buffer PreviousRestirDiReservoirBuffer { RestirDiReservoir previous_restir_di_reservoirs[]; };
layout(set = 0, binding = 60, std430) buffer RestirDiCounterBuffer { uint restir_di_counters[]; };
layout(set = 0, binding = 61, std140) uniform RestirDiRaygenParams {
    uint width;
    uint height;
    uint frameIndex;
    uint enabled;
    uint temporalMaxAge;
    uint spatialRounds;
    uint spatialMaxM;
    uint visibilityPolicy;
    float spatialRadius;
    float normalThreshold;
    float depthThreshold;
    float temporalLuminanceLimitFactor;
    float confidenceDecay;
    float lumClampNeighborAvgFactor;
    float lumClampNeighborMaxFactor;
    float fireflyClamp;
    float productionClampLuminance;
    uint useFallbackInitial;
    uint spatialResultValid;
    uint lightVersion;
    uint distributionVersion;
} restir_di_raygen_params;

layout(set = 0, binding = 10, std430) readonly buffer MeshMaterials { vec4 mesh_materials[]; };

layout(set = 0, binding = 11, std140) uniform MeshParams {
    uint vertex_count;
    uint triangle_count;
    uint bvh_node_count;
    uint material_count;
    uint enabled;
    uint sphere_count;
    uint primitive_count;
    uint instance_count;
    uint light_count;
    float emissive_total_area;
    uint mesh_count;
    uint local_vertex_count;
    uint local_index_count;
    uint local_bvh_node_count;
    uint local_triangle_count;
    uint tlas_node_count;
    uint tlas_instance_index_count;
    uint authored_light_offset;
    uint authored_light_count;
    uint _padding4;
} mesh_params;

layout(set = 0, binding = 12) uniform texture2D env_map;
layout(set = 0, binding = 13) uniform sampler env_sampler;
layout(set = 0, binding = 14, std430) readonly buffer EnvAliasRows { vec2 env_alias_rows[]; };
layout(set = 0, binding = 15, std430) readonly buffer EnvAliasCols { vec2 env_alias_cols[]; };
layout(set = 0, binding = 16, std140) uniform EnvParams {
    uint enabled;
    float intensity;
    float rotation;
    uint width;
    uint height;
    float background_intensity;
    uint procedural;
    uint sky_cdf_width;
    float inv_total_lum;
    uint sky_cdf_height;
    float _pad4;
    float _pad5;
} env_params;

layout(set = 0, binding = 17, std430) readonly buffer SceneSpheres { vec4 scene_spheres[]; };

layout(set = 0, binding = 18, std140) uniform RendererDebug {
    uint view;
    uint flags;
    uint selected_instance;
    float scale;
} debug_params;

layout(set = 0, binding = 41) uniform sampler2D material_textures[];

struct PrimitiveRecord {
    uvec4 index_data;
    uvec4 metadata;
};
layout(set = 0, binding = 21, std430) readonly buffer ScenePrimitiveRecords {
    PrimitiveRecord primitive_records[];
};

struct InstanceRecord {
    mat4 transform;
    mat4 inverse_transform;
    mat4 normal_transform;
    mat4 prev_transform;
    uvec4 metadata;
};
layout(set = 0, binding = 22, std430) readonly buffer SceneInstanceRecords {
    InstanceRecord instance_records[];
};

struct LightRecord {
    uvec4 metadata;
    uvec4 identity;
    vec4 data0;
    vec4 data1;
    vec4 data2;
    vec4 data3;
};
layout(set = 0, binding = 24, std430) readonly buffer SceneLightRecords {
    LightRecord light_records[];
};

struct MeshRecord {
    uvec4 vertex_index_data;
    uvec4 primitive_data;
    uvec4 bvh_data;
    uvec4 flags;
};
layout(set = 0, binding = 25, std430) readonly buffer SceneMeshRecords {
    MeshRecord mesh_records[];
};

struct LocalVertex {
    vec4 position_uv_x;
    vec4 normal_uv_y;
    vec4 tangent;
    vec4 color;
    vec4 texcoord1;
};
layout(set = 0, binding = 26, std430) readonly buffer LocalMeshVertices {
    LocalVertex local_mesh_vertices[];
};
layout(set = 0, binding = 27, std430) readonly buffer LocalMeshIndices {
    uint local_mesh_indices[];
};
layout(set = 0, binding = 28, std430) readonly buffer GpuSkinningRtMeshBindings {
    uvec4 gpu_skinning_rt_mesh_bindings[];
};
layout(set = 0, binding = 29, std430) readonly buffer GpuSkinningRtVertices {
    LocalVertex gpu_skinning_rt_vertices[];
};
layout(set = 0, binding = 31, std430) readonly buffer GpuSkinningRtPreviousVertices {
    LocalVertex gpu_skinning_rt_previous_vertices[];
};
layout(set = 0, binding = 34, std430) readonly buffer RtTriangleMaterialIds {
    uint rt_triangle_material_ids[];
};
layout(set = 0, binding = 46, std430) readonly buffer RtGeometryTriangleOffsets {
    uint rt_geometry_triangle_offsets[];
};
layout(set = 0, binding = 47, std430) readonly buffer RtMeshGeometryRanges {
    uvec2 rt_mesh_geometry_ranges[];
};
layout(set = 0, binding = 48, std430) buffer RtDiagnosticCounters {
    uint rt_diagnostic_counters[];
};
layout(set = 0, binding = 57, std430) readonly buffer StreamingResetInstanceMasks {
    uint streaming_reset_instance_masks[];
};

bool streaming_instance_reset_mask(uint instanceId, uint flag) {
    return instanceId != 0xffffffffu &&
        instanceId < streaming_reset_instance_masks.length() &&
        (streaming_reset_instance_masks[instanceId] & flag) != 0u;
}

const uint RTV_DEBUG_FLAG_RAY_TRACING_COUNTERS = 1u << 0u;
const uint RT_DIAG_CAMERA_ANY_HIT_INVOCATIONS = 0u;
const uint RT_DIAG_CAMERA_ANY_HIT_IGNORED = 1u;
const uint RT_DIAG_CAMERA_ANY_HIT_ACCEPTED = 2u;
const uint RT_DIAG_SHADOW_ANY_HIT_INVOCATIONS = 3u;
const uint RT_DIAG_SHADOW_ANY_HIT_IGNORED = 4u;
const uint RT_DIAG_SHADOW_ANY_HIT_ACCEPTED = 5u;
const uint RT_DIAG_SURFACE_TRACE_RAYS = 6u;
const uint RT_DIAG_SHADOW_TRACE_RAYS = 7u;
const uint RT_DIAG_CLOSEST_HIT_INVOCATIONS = 8u;
const uint RT_DIAG_CLOSEST_HIT_ALPHA_MATERIAL = 9u;
const uint RT_DIAG_CAUSTIC_SHADOW_ATTEMPTS = 10u;
const uint RT_DIAG_CAUSTIC_TRANSMISSIVE_HITS = 11u;
const uint RT_DIAG_CAUSTIC_TRANSMISSIVE_VISIBLE = 12u;
const uint RT_DIAG_CAUSTIC_SHADOW_BLOCKED = 13u;

void record_rt_counter(uint counterIndex) {
    if ((camera.path_trace_controls.z & 1u) != 0u) {
        atomicAdd(rt_diagnostic_counters[counterIndex], 1u);
    }
}

layout(set = 0, binding = 40, std430) readonly buffer LightBvhNodes {
    vec4 light_bvh_nodes[];
};

layout(set = 0, binding = 30, std430) readonly buffer LocalTriangleData {
    vec4 local_triangle_data[];
};

layout(set = 1, binding = 0) uniform texture2D atmosphere_transmittance_lut;
layout(set = 1, binding = 1) uniform texture2D atmosphere_sky_view_lut;
layout(set = 1, binding = 2) uniform sampler atmosphere_sampler;
layout(set = 1, binding = 3) uniform texture3D atmosphere_aerial_perspective_lut;
layout(set = 1, binding = 4) uniform texture2D atmosphere_multi_scatter_lut;
layout(set = 1, binding = 5, std430) readonly buffer SkyCdfRows { float sky_cdf_rows[]; };
layout(set = 1, binding = 6, std430) readonly buffer SkyCdfCols { float sky_cdf_cols[]; };

const float SKY_CDF_HALF_RCP_PI_SQ = 0.0506605918210689; // 1/(2*PI*PI)

struct Material {
    uint material_data_index;
    vec3 color;
    float roughness;
    float ior;
    uint mat_type;
    float metallic;
    float pad2;
    vec3 emissive;
    float alpha_factor;
    int base_color_texture;
    int normal_texture;
    int metallic_roughness_texture;
    int emissive_texture;
    float alpha_cutoff;
    uint alpha_mode;
    uint double_sided;
    vec3 conductor_eta;
    uint use_conductor_optics;
    vec3 conductor_k;
    float anisotropy_strength;
    float anisotropy_rotation;
    int occlusion_texture;
    float occlusion_strength;
    vec3 sheen_color;
    float sheen_roughness;
    int sheen_color_texture;
    int sheen_roughness_texture;
    float iridescence_factor;
    float iridescence_ior;
    float iridescence_thickness_min;
    float iridescence_thickness_max;
    int iridescence_texture;
    int iridescence_thickness_texture;
    float occlusion;
    float normal_variance;
    float clearcoat_factor;
    float clearcoat_roughness;
    int clearcoat_texture;
    int clearcoat_roughness_texture;
    int clearcoat_normal_texture;
    float transmission_factor;
    int transmission_texture;
    float specular_factor;
    vec3 specular_color;
    int specular_texture;
    int specular_color_texture;
    int anisotropy_texture;
    float volume_thickness_factor;
    int volume_thickness_texture;
    vec3 volume_attenuation_color;
    float volume_attenuation_distance;
    float dispersion_factor;
    int opacity_texture;
    int height_texture;
    float height_scale;
    vec3 clearcoat_normal;
    float clearcoat_normal_variance;
};

const uint MATERIAL_CLOSURE_FLAG_DIFFUSE       = 1u << 0u;
const uint MATERIAL_CLOSURE_FLAG_SPECULAR      = 1u << 1u;
const uint MATERIAL_CLOSURE_FLAG_SSS           = 1u << 2u;
const uint MATERIAL_CLOSURE_FLAG_TRANSMISSION  = 1u << 3u;
const uint MATERIAL_CLOSURE_FLAG_CLEARCOAT     = 1u << 4u;
const uint MATERIAL_CLOSURE_FLAG_SHEEN         = 1u << 5u;
const uint MATERIAL_CLOSURE_FLAG_THIN_FILM     = 1u << 6u;
const uint MATERIAL_CLOSURE_FLAG_METAL         = 1u << 7u;
const uint MATERIAL_CLOSURE_FLAG_UNLIT         = 1u << 8u;
const uint MATERIAL_CLOSURE_FLAG_VOLUME        = 1u << 9u;
const uint MATERIAL_CLOSURE_FLAG_DISPERSION    = 1u << 10u;
const float SCREEN_VELOCITY_PACK_SCALE = 512.0;
const float SCREEN_VELOCITY_SATURATION_THRESHOLD = SCREEN_VELOCITY_PACK_SCALE - 0.5;
const float SCREEN_VELOCITY_DEBUG_SCALE = 128.0;

struct MaterialClosure {
    uint flags;
    float weight;
    vec3 color;
    float roughness;
    float metallic;
    float ior;
    float pad;
};

float material_effective_transmission(Material material) {
    float explicitTransmission = clamp(material.transmission_factor, 0.0, 1.0);
    if (explicitTransmission > 1.0e-5 || material.transmission_texture >= 0) {
        return explicitTransmission;
    }
    return material.mat_type == 2u ? 1.0 : 0.0;
}

MaterialClosure material_to_closure(Material m) {
    MaterialClosure c;
    c.color = m.color;
    c.roughness = m.roughness;
    c.metallic = m.metallic;
    c.ior = m.ior;
    c.weight = 1.0;
    c.pad = 0.0;
    c.flags = 0u;

    if (m.mat_type == 0u) {
        c.flags = MATERIAL_CLOSURE_FLAG_DIFFUSE;
    } else if (m.mat_type == 1u || m.mat_type == 3u) {
        c.flags = MATERIAL_CLOSURE_FLAG_SPECULAR | MATERIAL_CLOSURE_FLAG_METAL;
    } else if (m.mat_type == 2u) {
        c.flags = MATERIAL_CLOSURE_FLAG_SPECULAR | MATERIAL_CLOSURE_FLAG_TRANSMISSION;
    } else if (m.mat_type == 4u) {
        c.flags = MATERIAL_CLOSURE_FLAG_SPECULAR | MATERIAL_CLOSURE_FLAG_CLEARCOAT;
    } else if (m.mat_type == 5u) {
        c.flags = MATERIAL_CLOSURE_FLAG_UNLIT;
    }

    if ((c.flags & MATERIAL_CLOSURE_FLAG_UNLIT) != 0u) {
        return c;
    }

    if (material_effective_transmission(m) > 1.0e-5) {
        c.flags |= MATERIAL_CLOSURE_FLAG_TRANSMISSION;
    }
    if (m.clearcoat_factor > 1.0e-5) {
        c.flags |= MATERIAL_CLOSURE_FLAG_CLEARCOAT | MATERIAL_CLOSURE_FLAG_SPECULAR;
    }

    if (dot(max(m.sheen_color, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)) > 1.0e-5) {
        c.flags |= MATERIAL_CLOSURE_FLAG_SHEEN;
    }
    if (m.iridescence_factor > 1.0e-5) {
        c.flags |= MATERIAL_CLOSURE_FLAG_THIN_FILM;
    }
    if (m.volume_thickness_factor > 1.0e-6 && m.volume_attenuation_distance > 0.0) {
        c.flags |= MATERIAL_CLOSURE_FLAG_VOLUME;
    }
    if (m.dispersion_factor > 1.0e-6) {
        c.flags |= MATERIAL_CLOSURE_FLAG_DISPERSION;
    }

    return c;
}

bool material_is_unlit(Material material) {
    return material.mat_type == 5u;
}

bool material_is_transmissive(Material material) {
    return material_effective_transmission(material) > 1.0e-5;
}

bool closure_has_flag(MaterialClosure c, uint flag) {
    return (c.flags & flag) != 0u;
}

struct RayPayload {
    uint hit;
    float t;
    vec3 world_pos;
    uint material_id;
    vec3 local_pos;
    vec3 geom_normal;
    uint front_face;
    vec3 normal;
    uint instance_id;
    uint mesh_id;
    uint primitive_id;
    uint picking;
    vec3 barycentrics;
    vec2 uv;
    vec2 uv1;
    vec3 tangent;
    vec3 bitangent;
    vec4 vertex_color;
};

const float PI = 3.14159265358979323846;
const uint TRI_STRIDE = 12u;
const uint MATERIAL_PARAMETER_STRIDE = 18u;
const uint MATERIAL_TEXTURE_TRANSFORM_COUNT = 17u;
const uint MATERIAL_TEXTURE_TRANSFORM_BASE = MATERIAL_PARAMETER_STRIDE;
const uint MATERIAL_STRIDE = MATERIAL_PARAMETER_STRIDE + MATERIAL_TEXTURE_TRANSFORM_COUNT * 2u;
const uint MATERIAL_TEXTURE_TRANSFORM_BASE_COLOR = 0u;
const uint MATERIAL_TEXTURE_TRANSFORM_NORMAL = 1u;
const uint MATERIAL_TEXTURE_TRANSFORM_METALLIC_ROUGHNESS = 2u;
const uint MATERIAL_TEXTURE_TRANSFORM_EMISSIVE = 3u;
const uint MATERIAL_TEXTURE_TRANSFORM_OCCLUSION = 4u;
const uint MATERIAL_TEXTURE_TRANSFORM_SHEEN_COLOR = 5u;
const uint MATERIAL_TEXTURE_TRANSFORM_SHEEN_ROUGHNESS = 6u;
const uint MATERIAL_TEXTURE_TRANSFORM_IRIDESCENCE = 7u;
const uint MATERIAL_TEXTURE_TRANSFORM_IRIDESCENCE_THICKNESS = 8u;
const uint MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT = 9u;
const uint MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT_ROUGHNESS = 10u;
const uint MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT_NORMAL = 11u;
const uint MATERIAL_TEXTURE_TRANSFORM_TRANSMISSION = 12u;
const uint MATERIAL_TEXTURE_TRANSFORM_VOLUME_THICKNESS = 13u;
const uint MATERIAL_TEXTURE_TRANSFORM_SPECULAR = 14u;
const uint MATERIAL_TEXTURE_TRANSFORM_SPECULAR_COLOR = 15u;
const uint MATERIAL_TEXTURE_TRANSFORM_ANISOTROPY = 16u;
const int MATERIAL_TEXTURE_LIMIT = 1024;
const uint MATERIAL_FLAG_MANUAL_BASE_COLOR_SRGB = 1u << 0u;
const uint MATERIAL_FLAG_MANUAL_EMISSIVE_SRGB = 1u << 1u;
const uint MATERIAL_FLAG_NORMAL_MAP_DIRECTX = 1u << 2u;
const uint MATERIAL_FLAG_SPECULAR_GLOSSINESS_WORKFLOW = 1u << 3u;
const uint MATERIAL_FLAG_SPECULAR_ALPHA_GLOSSINESS = 1u << 4u;
const uint ALPHA_MODE_OPAQUE = 0u;
const uint ALPHA_MODE_MASK = 1u;
const uint ALPHA_MODE_BLEND = 2u;
const float MATERIAL_DELTA_ROUGHNESS_THRESHOLD = 0.001;
const float MATERIAL_MIN_GGX_ROUGHNESS = 0.001;

bool material_is_delta(Material material) {
    return material_is_transmissive(material) ||
        (material.mat_type == 1u && material.roughness < MATERIAL_DELTA_ROUGHNESS_THRESHOLD);
}

float ggx_safe_roughness(float roughness) {
    return clamp(roughness, MATERIAL_MIN_GGX_ROUGHNESS, 1.0);
}

float shadow_self_hit_epsilon() {
    return max(camera.render_controls.x, 0.00001);
}

float shadow_distance_bias() {
    return max(camera.render_controls.y, 0.0);
}

float firefly_clamp_luminance() {
    return max(camera.render_controls.z, 1.0);
}

float russian_roulette_min_survival() {
    return clamp(camera.render_controls.w, 0.02, 0.50);
}
const float DEBUG_WHITE_ENV_RADIANCE = 4.0;

uint pack_unorm2x16(vec2 v) {
    uint x = uint(round(clamp(v.x, 0.0, 1.0) * 65535.0));
    uint y = uint(round(clamp(v.y, 0.0, 1.0) * 65535.0));
    return x | (y << 16u);
}

uint pack_snorm2x16(vec2 v) {
    ivec2 quantized = ivec2(round(clamp(v, vec2(-1.0), vec2(1.0)) * 32767.0));
    return (uint(quantized.x) & 0xffffu) | ((uint(quantized.y) & 0xffffu) << 16u);
}

uint encode_octahedral_normal(vec3 n) {
    float denom = abs(n.x) + abs(n.y) + abs(n.z) + 1e-8;
    vec2 p = n.xy / denom;
    if (n.z < 0.0) {
        p = (vec2(1.0) - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    }
    return pack_snorm2x16(p);
}

vec2 unpack_unorm2x16(uint packedValue) {
    return vec2(float(packedValue & 0xffffu), float((packedValue >> 16u) & 0xffffu)) / 65535.0;
}

vec2 unpack_snorm2x16(uint packedValue) {
    ivec2 quantized = ivec2(int(packedValue & 0xffffu), int((packedValue >> 16u) & 0xffffu));
    if (quantized.x >= 32768) {
        quantized.x -= 65536;
    }
    if (quantized.y >= 32768) {
        quantized.y -= 65536;
    }
    return clamp(vec2(quantized) / 32767.0, vec2(-1.0), vec2(1.0));
}

vec3 decode_octahedral_normal(uint packedValue) {
    vec2 f = unpack_snorm2x16(packedValue);
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

uvec4 pack_depth_normal(float depth, vec3 normal, float roughness) {
    return uvec4(floatBitsToUint(depth), encode_octahedral_normal(normal), floatBitsToUint(roughness), 0u);
}

uint pack_variance(float v) {
    return pack_unorm2x16(vec2(clamp(v / 64.0, 0.0, 1.0), 0.0));
}

float temporal_normalized_hit_distance(float hitDistance) {
    return clamp(log2(max(hitDistance, 0.0) + 1.0) / 10.0, 0.0, 1.0);
}

float restir_luminance(vec3 value) {
    return dot(value, vec3(0.2126, 0.7152, 0.0722));
}

const uint RESTIR_VISIBILITY_UNKNOWN = 0u;
const uint RESTIR_VISIBILITY_VISIBLE = 1u;
const uint RESTIR_VISIBILITY_INVALID = 2u;
const uint RESTIR_GI_FLAG_VALID = 1u << 0u;
const uint RESTIR_GI_FLAG_VISIBLE = 1u << 1u;
const float WORLD_POSITION_PACK_RANGE = 64.0;

uint restir_pack_validity_visibility(bool valid, uint visibility) {
    return valid ? (1u | ((visibility & 3u) << 1u)) : 0u;
}

bool restir_validity_bit(uint value) {
    return (value & 1u) != 0u;
}

uint restir_pack_state(uint age, uint validityVisibility, uint sampleCount) {
    return min(age, 255u) |
        ((validityVisibility & 0xffu) << 8u) |
        (min(sampleCount, 255u) << 16u);
}

uint restir_age(RestirReservoir reservoir) {
    return reservoir.metadata.z & 0xffu;
}

void restir_set_age(inout RestirReservoir reservoir, uint age) {
    reservoir.metadata.z = restir_pack_state(age, (reservoir.metadata.z >> 8u) & 0xffu, (reservoir.metadata.z >> 16u) & 0xffu);
}

uint restir_validity_visibility_bits(RestirReservoir reservoir) {
    return (reservoir.metadata.z >> 8u) & 0xffu;
}

void restir_set_validity_visibility(inout RestirReservoir reservoir, uint validityVisibility) {
    reservoir.metadata.z = restir_pack_state(restir_age(reservoir), validityVisibility, (reservoir.metadata.z >> 16u) & 0xffu);
}

uint restir_sample_count_u(RestirReservoir reservoir) {
    return max((reservoir.metadata.z >> 16u) & 0xffu, 1u);
}

void restir_set_sample_count(inout RestirReservoir reservoir, float sampleCount) {
    reservoir.metadata.z = restir_pack_state(restir_age(reservoir), restir_validity_visibility_bits(reservoir), uint(clamp(ceil(sampleCount), 1.0, 255.0)));
}

void restir_set_source_pdf_and_previous_weight(inout RestirReservoir reservoir, float sourcePdf, float previousWeight) {
    reservoir.metadata.y = packHalf2x16(vec2(clamp(sourcePdf, 1.0e-6, 65504.0), clamp(previousWeight, 0.0, 1.0)));
}

float restir_previous_weight(RestirReservoir reservoir) {
    return unpackHalf2x16(reservoir.metadata.y).y;
}

uint restir_visibility_state(RestirReservoir reservoir) {
    return (restir_validity_visibility_bits(reservoir) >> 1u) & 3u;
}

bool restir_reservoir_valid(RestirReservoir reservoir) {
    return restir_validity_bit(restir_validity_visibility_bits(reservoir)) &&
        restir_sample_count_u(reservoir) > 0u &&
        reservoir.sample_value_confidence.a > 0.0 &&
        reservoir.sample_radiance_target.w > 0.0;
}

bool restir_gi_reservoir_valid(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return ((reservoir.metadata.z & 0xffu) & RESTIR_GI_FLAG_VALID) != 0u &&
        reservoir.radiance_weight_sum.w > 0.0 &&
        reservoir.hit_position_target_pdf.w > 0.0 &&
        reservoir.suffix_radiance_source_pdf.w > 0.0 &&
        reservoir.metadata.x > 0u;
#else
    return ((reservoir.metadata.x >> 16u) & RESTIR_GI_FLAG_VALID) != 0u &&
        reservoir.radiance_weight_sum.w > 0.0 &&
        unpackHalf2x16(reservoir.metadata.w).y > 0.0 &&
        (reservoir.metadata.x & 0xffu) > 0u;
#endif
}

uint restir_gi_sample_count_u(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return max(reservoir.metadata.x, 1u);
#else
    return max(reservoir.metadata.x & 0xffu, 1u);
#endif
}

float restir_gi_sample_count(RestirGiReservoir reservoir) {
    return float(restir_gi_sample_count_u(reservoir));
}

uint restir_gi_age(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.metadata.y;
#else
    return (reservoir.metadata.x >> 8u) & 0xffu;
#endif
}

uint restir_gi_flags(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.metadata.z & 0xffu;
#else
    return (reservoir.metadata.x >> 16u) & 0xffu;
#endif
}

bool restir_gi_visible(RestirGiReservoir reservoir) {
    return (restir_gi_flags(reservoir) & RESTIR_GI_FLAG_VISIBLE) != 0u;
}

uint restir_gi_material_id(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return 0xffffffffu;
#else
    return reservoir.metadata.z;
#endif
}

float restir_gi_roughness(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return clamp(reservoir.normal_roughness.w, 0.0, 1.0);
#else
    return float((reservoir.metadata.x >> 24u) & 0xffu) / 255.0;
#endif
}

vec3 restir_gi_normal(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return normalize(reservoir.normal_roughness.xyz);
#else
    return decode_octahedral_normal(reservoir.metadata.y);
#endif
}

bool restir_gi_has_positions(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return restir_gi_reservoir_valid(reservoir) &&
        reservoir.source_direction_distance.w > 0.0;
#else
    return false;
#endif
}

vec3 restir_gi_hit_position(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.hit_position_target_pdf.xyz;
#else
    return vec3(0.0);
#endif
}

vec3 restir_gi_receiver_position(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.hit_position_target_pdf.xyz -
        normalize(reservoir.source_direction_distance.xyz) * reservoir.source_direction_distance.w;
#else
    return vec3(0.0);
#endif
}

void restir_gi_set_receiver_position(inout RestirGiReservoir reservoir, vec3 receiverPosition) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    vec3 delta = reservoir.hit_position_target_pdf.xyz - receiverPosition;
    reservoir.source_direction_distance = vec4(normalize(delta), length(delta));
#endif
}

uint restir_gi_pack_metadata(uint sampleCount, uint age, uint flags, float roughness) {
    uint sampleBits = min(sampleCount, 255u);
    uint ageBits = min(age, 255u);
    uint flagBits = flags & 0xffu;
    uint roughnessBits = uint(clamp(round(clamp(roughness, 0.0, 1.0) * 255.0), 0.0, 255.0));
    return sampleBits | (ageBits << 8u) | (flagBits << 16u) | (roughnessBits << 24u);
}

void restir_gi_set_metadata(inout RestirGiReservoir reservoir, uint sampleCount, uint age, uint flags, float roughness) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.metadata.x = sampleCount;
    reservoir.metadata.y = age;
    reservoir.metadata.z = (reservoir.metadata.z & 0xffffff00u) | (flags & 0xffu);
    reservoir.normal_roughness.w = clamp(roughness, 0.0, 1.0);
#else
    reservoir.metadata.x = restir_gi_pack_metadata(sampleCount, age, flags, roughness);
#endif
}

void restir_gi_set_normal(inout RestirGiReservoir reservoir, vec3 normal) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.normal_roughness.xyz = normalize(normal);
#else
    reservoir.metadata.y = encode_octahedral_normal(normalize(normal));
#endif
}

void restir_gi_set_material_id(inout RestirGiReservoir reservoir, uint materialId) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    // The full estimator layout uses metadata.w for the scene version hash.
#else
    reservoir.metadata.z = materialId;
#endif
}

float restir_gi_hit_distance(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return abs(reservoir.source_direction_distance.w);
#else
    return unpackHalf2x16(reservoir.metadata.w).x;
#endif
}

float restir_gi_target_pdf(RestirGiReservoir reservoir) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    return reservoir.hit_position_target_pdf.w;
#else
    return unpackHalf2x16(reservoir.metadata.w).y;
#endif
}

void restir_gi_set_hit_distance_target_pdf(inout RestirGiReservoir reservoir, float hitDistance, float targetPdf) {
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.hit_position_target_pdf.w = max(targetPdf, 1.0e-4);
    reservoir.source_direction_distance.w = max(hitDistance, 0.0);
#else
    reservoir.metadata.w = packHalf2x16(vec2(clamp(hitDistance, 0.0, 65504.0), clamp(targetPdf, 1.0e-4, 65504.0)));
#endif
}

RestirGiReservoir empty_restir_gi_reservoir() {
    RestirGiReservoir reservoir;
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.hit_position_target_pdf = vec4(0.0);
    reservoir.normal_roughness = vec4(0.0, 1.0, 0.0, 1.0);
    reservoir.suffix_radiance_source_pdf = vec4(0.0);
    reservoir.source_direction_distance = vec4(0.0);
#endif
    reservoir.radiance_weight_sum = vec4(0.0);
#if RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT
    reservoir.metadata = uvec4(0u);
#else
    reservoir.metadata = uvec4(restir_gi_pack_metadata(0u, 0u, 0u, 1.0), encode_octahedral_normal(vec3(0.0, 1.0, 0.0)), 0u, 0u);
#endif
    return reservoir;
}

float restir_gi_age_normalized(RestirGiReservoir reservoir, float maxAge) {
    return clamp(float(restir_gi_age(reservoir)) / max(maxAge, 1.0), 0.0, 1.0);
}

float restir_target_function(RestirReservoir reservoir) {
    return max(reservoir.sample_radiance_target.w, max(restir_luminance(reservoir.sample_value_confidence.rgb), 0.0));
}

float restir_weight_sum(RestirReservoir reservoir) {
    return max(reservoir.sample_normal_weight.w, 0.0);
}

float restir_source_pdf(RestirReservoir reservoir) {
    return max(unpackHalf2x16(reservoir.metadata.y).x, 1.0e-6);
}

float restir_sample_count(RestirReservoir reservoir) {
    return float(restir_sample_count_u(reservoir));
}

uint restir_temporal_signature(uint sampleType, float roughness, uint materialId, uint instanceId) {
    uint roughnessBucket = uint(clamp(floor(clamp(roughness, 0.0, 1.0) * 15.0 + 0.5), 0.0, 15.0));
    uint surfaceHash = materialId * 747796405u ^ instanceId * 2891336453u;
    surfaceHash ^= surfaceHash >> 16u;
    return (sampleType & 0xffu) | (roughnessBucket << 8u) | ((surfaceHash & 0xfffffu) << 12u);
}

float restir_age_confidence(RestirReservoir reservoir, float maxAge) {
    return 1.0 - clamp(float(restir_age(reservoir)) / max(maxAge, 1.0), 0.0, 1.0);
}

float restir_pairwise_compatibility(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    if (!restir_reservoir_valid(current) || !restir_reservoir_valid(previous) || restir_age(previous) >= uint(maxAge)) {
        return 0.0;
    }

    if (current.metadata.x != previous.metadata.x) {
        return 0.0;
    }

    if (current.metadata.w != 0u && previous.metadata.w != 0u && current.metadata.w != previous.metadata.w) {
        return 0.0;
    }

    if (restir_visibility_state(previous) == RESTIR_VISIBILITY_INVALID) {
        return 0.0;
    }

    float currentPdf = restir_source_pdf(current);
    float previousPdf = restir_source_pdf(previous);
    float pdfRatio = min(currentPdf, previousPdf) / max(currentPdf, previousPdf);

    float currentTarget = max(restir_target_function(current), 1.0e-5);
    float previousTarget = max(restir_target_function(previous), 1.0e-5);
    float targetRatio = min(currentTarget, previousTarget) / max(currentTarget, previousTarget);

    return clamp(motionConfidence, 0.0, 1.0) *
        restir_age_confidence(previous, maxAge) *
        sqrt(clamp(pdfRatio, 0.0, 1.0)) *
        sqrt(clamp(targetRatio, 0.0, 1.0));
}

float restir_pairwise_previous_weight(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    float compatibility = restir_pairwise_compatibility(current, previous, motionConfidence, maxAge);
    if (compatibility <= 0.0) {
        return 0.0;
    }

    float currentMass = max(restir_target_function(current), 0.0) / restir_source_pdf(current);
    float previousMass = max(restir_target_function(previous), 0.0) /
        restir_source_pdf(previous) *
        min(restir_sample_count(previous), 32.0) *
        clamp(previous.sample_value_confidence.a, 0.0, 1.0) *
        compatibility;
    float combined = currentMass + previousMass;
    if (combined <= 1.0e-8) {
        return 0.0;
    }

    float motionCap = mix(0.85, 0.10, 1.0 - clamp(motionConfidence, 0.0, 1.0));
    return clamp(previousMass / combined, 0.0, motionCap);
}

RestirReservoir restir_pairwise_temporal_merge(RestirReservoir current, RestirReservoir previous, float motionConfidence, float maxAge) {
    float previousWeight = restir_pairwise_previous_weight(current, previous, motionConfidence, maxAge);
    RestirReservoir selected = previousWeight > 0.5 ? previous : current;
    bool selectedPrevious = previousWeight > 0.5;
    uint selectedVisibility = restir_visibility_state(selected);
    restir_set_age(selected, selectedPrevious ? min(restir_age(previous) + 1u, 255u) : 0u);
    restir_set_validity_visibility(selected, restir_pack_validity_visibility(restir_reservoir_valid(selected), selectedVisibility));
    selected.sample_value_confidence.a = clamp(selected.sample_value_confidence.a * clamp(motionConfidence, 0.0, 1.0), 0.0, 1.0);
    selected.sample_normal_weight.w = min(
        max(restir_weight_sum(current), restir_target_function(current)) +
            max(restir_weight_sum(previous), restir_target_function(previous)) * previousWeight,
        65504.0);
    restir_set_source_pdf_and_previous_weight(selected, restir_source_pdf(selected), previousWeight);
    restir_set_sample_count(selected, min(
        restir_sample_count(current) + restir_sample_count(previous) * previousWeight,
        64.0));
    return selected;
}

uvec2 pack_world_position(vec3 world_pos) {
    vec3 relative = clamp((world_pos - camera.pos.xyz) / WORLD_POSITION_PACK_RANGE, vec3(-1.0), vec3(1.0));
    return uvec2(pack_snorm2x16(relative.xy), pack_snorm2x16(vec2(relative.z, 0.0)));
}

bool project_unjittered_to_pixels_checked(mat4 viewProj, vec2 jitterPixels, vec3 worldPos, ivec2 dims, out vec2 pixels) {
    vec4 clip = viewProj * vec4(worldPos, 1.0);
    if (clip.w <= 0.0) {
        pixels = vec2(0.0);
        return false;
    }
    vec2 invDims = 1.0 / vec2(max(dims, ivec2(1)));
    clip.xy -= jitterPixels * 2.0 * invDims * clip.w;
    vec3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0001) {
        pixels = vec2(0.0);
        return false;
    }
    pixels = vec2(
        (ndc.x * 0.5 + 0.5) * float(dims.x) - 0.5,
        (ndc.y * 0.5 + 0.5) * float(dims.y) - 0.5);
    return true;
}

vec2 project_unjittered_to_pixels(mat4 viewProj, vec2 jitterPixels, vec3 worldPos, ivec2 dims) {
    vec2 pixels;
    return project_unjittered_to_pixels_checked(viewProj, jitterPixels, worldPos, dims, pixels) ? pixels : vec2(-1.0);
}

uint pack_velocity_pixels(vec2 velocityPixels) {
    ivec2 encoded = ivec2(round(clamp(velocityPixels / SCREEN_VELOCITY_PACK_SCALE, vec2(-1.0), vec2(1.0)) * 32767.0));
    return (uint(encoded.x) & 0xffffu) | ((uint(encoded.y) & 0xffffu) << 16u);
}

uint pack_invalid_velocity_pixels() {
    return pack_velocity_pixels(vec2(SCREEN_VELOCITY_PACK_SCALE));
}

uint compute_surface_velocity_from_previous_world(vec3 currentWorldPos, vec3 previousWorldPos, ivec2 dims) {
    vec2 currentPos;
    vec2 previousPos;
    bool valid = project_unjittered_to_pixels_checked(prev_camera.view_proj, prev_camera.jitter.xy, currentWorldPos, dims, currentPos) &&
        project_unjittered_to_pixels_checked(prev_camera.prev_view_proj, prev_camera.jitter.zw, previousWorldPos, dims, previousPos);
    return valid ? pack_velocity_pixels(currentPos - previousPos) : pack_invalid_velocity_pixels();
}

vec3 previous_skinned_local_position(uint meshIndex, uint primitiveIndex, vec3 barycentrics, vec3 fallbackLocalPos) {
    if (meshIndex >= mesh_params.mesh_count) {
        return fallbackLocalPos;
    }

    uvec4 binding = gpu_skinning_rt_mesh_bindings[meshIndex];
    if (binding.z == 0u) {
        return fallbackLocalPos;
    }

    uint triangleCount = mesh_params.local_index_count / 3u;
    if (primitiveIndex >= triangleCount) {
        return fallbackLocalPos;
    }

    uint triIndex = primitiveIndex * 3u;
    uint i0 = local_mesh_indices[triIndex + 0u];
    uint i1 = local_mesh_indices[triIndex + 1u];
    uint i2 = local_mesh_indices[triIndex + 2u];
    bool inSkinnedRange = i0 >= binding.x && (i0 - binding.x) < binding.y &&
        i1 >= binding.x && (i1 - binding.x) < binding.y &&
        i2 >= binding.x && (i2 - binding.x) < binding.y;
    if (!inSkinnedRange) {
        return fallbackLocalPos;
    }

    LocalVertex v0 = gpu_skinning_rt_previous_vertices[i0];
    LocalVertex v1 = gpu_skinning_rt_previous_vertices[i1];
    LocalVertex v2 = gpu_skinning_rt_previous_vertices[i2];
    return v0.position_uv_x.xyz * barycentrics.x +
        v1.position_uv_x.xyz * barycentrics.y +
        v2.position_uv_x.xyz * barycentrics.z;
}

uint compute_surface_velocity(
    vec3 currentWorldPos,
    vec3 localPos,
    uint instanceId,
    uint meshId,
    uint primitiveId,
    vec3 barycentrics,
    ivec2 dims) {
    vec3 previousWorldPos = currentWorldPos;
    if (instanceId < mesh_params.instance_count) {
        InstanceRecord instance = instance_records[instanceId];
        vec3 previousLocalPos = previous_skinned_local_position(meshId, primitiveId, barycentrics, localPos);
        previousWorldPos = (instance.prev_transform * vec4(previousLocalPos, 1.0)).xyz;
    }
    return compute_surface_velocity_from_previous_world(currentWorldPos, previousWorldPos, dims);
}

uint compute_sky_velocity(vec3 rayDir, ivec2 dims) {
    vec3 currentWorldPos = prev_camera.current_pos.xyz + normalize(rayDir) * 512.0;
    vec3 previousWorldPos = prev_camera.prev_pos.xyz + normalize(rayDir) * 512.0;
    vec2 currentPos;
    vec2 previousPos;
    bool valid = project_unjittered_to_pixels_checked(prev_camera.view_proj, prev_camera.jitter.xy, currentWorldPos, dims, currentPos) &&
        project_unjittered_to_pixels_checked(prev_camera.prev_view_proj, prev_camera.jitter.zw, previousWorldPos, dims, previousPos);
    return valid ? pack_velocity_pixels(currentPos - previousPos) : pack_invalid_velocity_pixels();
}

uint pcg_hash(uint inputValue) {
    uint state = inputValue * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec3 id_color(uint id) {
    uint h = pcg_hash(id + 1u);
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

float rand_f32(inout uint state) {
    state = pcg_hash(state);
    return float(state) / float(0xffffffffu);
}

vec3 rand_unit_vector(inout uint state) {
    float z = rand_f32(state) * 2.0 - 1.0;
    float a = rand_f32(state) * 2.0 * 3.141592653589793;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(a), r * sin(a), z);
}

Material decode_material(uint mat_idx) {
    uint idx = min(mat_idx, max(mesh_params.material_count, 1u) - 1u) * MATERIAL_STRIDE;
    vec4 d0 = mesh_materials[idx + 0u];
    vec4 d1 = mesh_materials[idx + 1u];
    vec4 d2 = mesh_materials[idx + 2u];
    vec4 d3 = mesh_materials[idx + 3u];
    vec4 d4 = mesh_materials[idx + 4u];
    vec4 d5 = mesh_materials[idx + 5u];
    vec4 d6 = mesh_materials[idx + 6u];
    vec4 d7 = mesh_materials[idx + 7u];
    vec4 d8 = mesh_materials[idx + 8u];
    vec4 d9 = mesh_materials[idx + 9u];
    vec4 d10 = mesh_materials[idx + 10u];
    vec4 d11 = mesh_materials[idx + 11u];
    vec4 d12 = mesh_materials[idx + 12u];
    vec4 d13 = mesh_materials[idx + 13u];
    vec4 d14 = mesh_materials[idx + 14u];
    vec4 d15 = mesh_materials[idx + 15u];
    vec4 d16 = mesh_materials[idx + 16u];
    vec4 d17 = mesh_materials[idx + 17u];
    Material m;
    m.material_data_index = idx;
    m.color = d0.xyz;
    m.roughness = d0.w;
    m.ior = d1.x;
    m.mat_type = uint(d1.y);
    m.metallic = d1.z;
    m.pad2 = d1.w;
    m.emissive = d2.xyz;
    m.alpha_factor = d2.w;
    m.base_color_texture = int(round(d3.x));
    m.normal_texture = int(round(d3.y));
    m.metallic_roughness_texture = int(round(d3.z));
    m.emissive_texture = int(round(d3.w));
    m.alpha_cutoff = d4.x;
    m.alpha_mode = uint(round(d4.y));
    m.double_sided = uint(round(d4.z));
    m.conductor_eta = d5.xyz;
    m.use_conductor_optics = uint(round(d5.w));
    m.conductor_k = d6.xyz;
    m.anisotropy_strength = d7.x;
    m.anisotropy_rotation = d7.y;
    m.occlusion_texture = int(round(d7.z));
    m.occlusion_strength = clamp(d7.w, 0.0, 1.0);
    m.sheen_color = max(d8.xyz, vec3(0.0));
    m.sheen_roughness = clamp(d8.w, 0.0, 1.0);
    m.sheen_color_texture = int(round(d9.x));
    m.sheen_roughness_texture = int(round(d9.y));
    m.iridescence_factor = clamp(d9.z, 0.0, 1.0);
    m.iridescence_ior = max(d9.w, 1.01);
    m.iridescence_thickness_min = max(d10.x, 0.0);
    m.iridescence_thickness_max = max(d10.y, m.iridescence_thickness_min);
    m.iridescence_texture = int(round(d10.z));
    m.iridescence_thickness_texture = int(round(d10.w));
    m.occlusion = 1.0;
    m.normal_variance = 0.0;
    m.clearcoat_factor = clamp(d11.x, 0.0, 1.0);
    m.clearcoat_roughness = clamp(d11.y, 0.0, 1.0);
    m.clearcoat_texture = int(round(d11.z));
    m.clearcoat_roughness_texture = int(round(d11.w));
    m.clearcoat_normal_texture = int(round(d12.x));
    m.transmission_factor = clamp(d12.y, 0.0, 1.0);
    m.transmission_texture = int(round(d12.z));
    m.specular_factor = clamp(d12.w, 0.0, 1.0);
    m.specular_color = max(d13.xyz, vec3(0.0));
    m.specular_texture = int(round(d13.w));
    m.specular_color_texture = int(round(d14.x));
    m.anisotropy_texture = int(round(d14.y));
    m.volume_thickness_factor = max(d14.z, 0.0);
    m.volume_thickness_texture = int(round(d14.w));
    m.volume_attenuation_color = clamp(d15.xyz, vec3(0.0), vec3(1.0));
    m.volume_attenuation_distance = d15.w;
    m.dispersion_factor = max(d16.x, 0.0);
    m.opacity_texture = int(round(d17.x));
    m.height_texture = int(round(d17.y));
    m.height_scale = d17.z;
    m.clearcoat_normal = vec3(0.0);
    m.clearcoat_normal_variance = 0.0;
    return m;
}

uint material_for_triangle_index(uint triangleIndex) {
    uint triangleCount = mesh_params.local_index_count / 3u;
    if (triangleIndex >= triangleCount) {
        return 0u;
    }
    return rt_triangle_material_ids[triangleIndex];
}

uint material_for_raw_triangle(uint meshFirstIndex, uint primitiveId) {
    uint triangleIndex = meshFirstIndex / 3u + primitiveId;
    return material_for_triangle_index(triangleIndex);
}

uint geometry_triangle_offset(uint meshIndex, uint geometryIndex, uint meshFirstIndex) {
    if (meshIndex >= mesh_params.mesh_count) {
        return meshFirstIndex / 3u;
    }
    uvec2 range = rt_mesh_geometry_ranges[meshIndex];
    if (range.y == 0u) {
        return meshFirstIndex / 3u;
    }
    return rt_geometry_triangle_offsets[range.x + min(geometryIndex, range.y - 1u)];
}

LocalVertex ray_tracing_local_vertex(uint meshIndex, uint vertexIndex) {
    if (meshIndex < mesh_params.mesh_count) {
        uvec4 binding = gpu_skinning_rt_mesh_bindings[meshIndex];
        if (binding.z != 0u && vertexIndex >= binding.x && (vertexIndex - binding.x) < binding.y) {
            return gpu_skinning_rt_vertices[vertexIndex];
        }
    }
    return local_mesh_vertices[vertexIndex];
}

bool ray_tracing_mesh_has_gpu_skinning(uint meshIndex) {
    return meshIndex < mesh_params.mesh_count && gpu_skinning_rt_mesh_bindings[meshIndex].z != 0u;
}

vec2 material_texture_uv(Material material, uint slot, vec2 uv0, vec2 uv1) {
    if (slot >= MATERIAL_TEXTURE_TRANSFORM_COUNT) {
        return uv0;
    }
    uint transformBase = material.material_data_index + MATERIAL_TEXTURE_TRANSFORM_BASE + slot * 2u;
    vec4 transform1 = mesh_materials[transformBase + 1u];
    uint texCoord = uint(round(transform1.z));
    return texCoord == 1u ? uv1 : uv0;
}

vec2 apply_material_texture_transform(Material material, uint slot, vec2 uv0, vec2 uv1) {
    vec2 uv = material_texture_uv(material, slot, uv0, uv1);
    if (slot >= MATERIAL_TEXTURE_TRANSFORM_COUNT) {
        return uv;
    }
    uint transformBase = material.material_data_index + MATERIAL_TEXTURE_TRANSFORM_BASE + slot * 2u;
    vec4 transform0 = mesh_materials[transformBase + 0u];
    vec4 transform1 = mesh_materials[transformBase + 1u];
    if (uint(round(transform1.y)) == 0u) {
        return uv;
    }
    vec2 scaled = uv * transform0.zw;
    float c = cos(transform1.x);
    float s = sin(transform1.x);
    vec2 rotated = vec2(c * scaled.x - s * scaled.y, s * scaled.x + c * scaled.y);
    return transform0.xy + rotated;
}

vec2 apply_material_height_parallax(Material material, vec2 uv0, vec2 uv1, vec3 normal, vec3 tangent, vec3 bitangent, vec3 rayDirection) {
    if (material.height_texture < 0 || material.height_texture >= MATERIAL_TEXTURE_LIMIT) {
        return uv0;
    }
    vec3 n = normalize(normal);
    vec3 t = normalize(tangent);
    vec3 b = normalize(bitangent);
    vec3 viewDir = normalize(-rayDirection);
    vec3 viewTs = vec3(dot(viewDir, t), dot(viewDir, b), max(dot(viewDir, n), 0.08));
    float height = texture(material_textures[nonuniformEXT(material.height_texture)], uv0).r;
    float centeredHeight = clamp(height, 0.0, 1.0) - 0.5;
    return uv0 + (viewTs.xy / viewTs.z) * centeredHeight * clamp(material.height_scale, 0.0, 0.25);
}

void apply_material_textures(inout Material material, vec2 uv0, vec2 uv1) {
    uint flags = uint(round(material.pad2));
    if (material.base_color_texture >= 0 && material.base_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.base_color_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_BASE_COLOR, uv0, uv1);
        vec4 base = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv);
        vec3 baseColor = (flags & MATERIAL_FLAG_MANUAL_BASE_COLOR_SRGB) != 0u
            ? pow(max(base.rgb, vec3(0.0)), vec3(2.2))
            : base.rgb;
        material.color *= baseColor;
        material.alpha_factor *= base.a;
    }
    if (material.metallic_roughness_texture >= 0 && material.metallic_roughness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.metallic_roughness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_METALLIC_ROUGHNESS, uv0, uv1);
        vec4 mr = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv);
        material.roughness = clamp(material.roughness * mr.g, 0.0, 1.0);
        material.metallic = clamp(material.metallic * mr.b, 0.0, 1.0);
        if (material.mat_type == 0u || material.mat_type == 3u) {
            material.mat_type = 3u;
        }
    }
    if (material.emissive_texture >= 0 && material.emissive_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.emissive_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_EMISSIVE, uv0, uv1);
        vec4 emissive = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv);
        vec3 emissiveColor = (flags & MATERIAL_FLAG_MANUAL_EMISSIVE_SRGB) != 0u
            ? pow(max(emissive.rgb, vec3(0.0)), vec3(2.2))
            : emissive.rgb;
        material.emissive *= emissiveColor;
    }
    if (material.occlusion_texture >= 0 && material.occlusion_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.occlusion_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_OCCLUSION, uv0, uv1);
        float ao = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).r;
        material.occlusion = mix(1.0, clamp(ao, 0.0, 1.0), material.occlusion_strength);
    }
    if (material.sheen_color_texture >= 0 && material.sheen_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.sheen_color_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_SHEEN_COLOR, uv0, uv1);
        material.sheen_color *= texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).rgb;
    }
    if (material.sheen_roughness_texture >= 0 && material.sheen_roughness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.sheen_roughness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_SHEEN_ROUGHNESS, uv0, uv1);
        material.sheen_roughness = clamp(material.sheen_roughness * texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).a, 0.0, 1.0);
    }
    if (material.iridescence_texture >= 0 && material.iridescence_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.iridescence_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_IRIDESCENCE, uv0, uv1);
        material.iridescence_factor = clamp(material.iridescence_factor * texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).r, 0.0, 1.0);
    }
    if (material.iridescence_thickness_texture >= 0 && material.iridescence_thickness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.iridescence_thickness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_IRIDESCENCE_THICKNESS, uv0, uv1);
        float thicknessMix = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).g;
        material.iridescence_thickness_min = mix(material.iridescence_thickness_min, material.iridescence_thickness_max, clamp(thicknessMix, 0.0, 1.0));
    }
    if (material.clearcoat_texture >= 0 && material.clearcoat_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.clearcoat_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT, uv0, uv1);
        material.clearcoat_factor = clamp(material.clearcoat_factor * texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).r, 0.0, 1.0);
    }
    if (material.clearcoat_roughness_texture >= 0 && material.clearcoat_roughness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.clearcoat_roughness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT_ROUGHNESS, uv0, uv1);
        material.clearcoat_roughness = clamp(material.clearcoat_roughness * texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).g, 0.0, 1.0);
    }
    if (material.transmission_texture >= 0 && material.transmission_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.transmission_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_TRANSMISSION, uv0, uv1);
        material.transmission_factor = clamp(material.transmission_factor * texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).r, 0.0, 1.0);
    }
    if (material.specular_texture >= 0 && material.specular_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.specular_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_SPECULAR, uv0, uv1);
        vec4 specularSample = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv);
        if ((flags & MATERIAL_FLAG_SPECULAR_ALPHA_GLOSSINESS) != 0u) {
            float glossinessFactor = clamp(1.0 - material.roughness, 0.0, 1.0);
            material.roughness = clamp(1.0 - glossinessFactor * specularSample.a, 0.0, 1.0);
        } else {
            material.specular_factor = clamp(material.specular_factor * specularSample.a, 0.0, 1.0);
        }
        if ((flags & MATERIAL_FLAG_SPECULAR_GLOSSINESS_WORKFLOW) != 0u) {
            material.specular_color *= max(specularSample.rgb, vec3(0.0));
        }
    }
    if (material.specular_color_texture >= 0 && material.specular_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.specular_color_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_SPECULAR_COLOR, uv0, uv1);
        material.specular_color *= max(texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).rgb, vec3(0.0));
    }
    if (material.anisotropy_texture >= 0 && material.anisotropy_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.anisotropy_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_ANISOTROPY, uv0, uv1);
        vec3 anisotropySample = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).rgb;
        material.anisotropy_strength = clamp(material.anisotropy_strength * anisotropySample.b, -1.0, 1.0);
        vec2 direction = anisotropySample.rg * 2.0 - 1.0;
        if (dot(direction, direction) > 1.0e-6) {
            material.anisotropy_rotation += atan(direction.y, direction.x);
        }
    }
    if (material.volume_thickness_texture >= 0 && material.volume_thickness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.volume_thickness_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_VOLUME_THICKNESS, uv0, uv1);
        material.volume_thickness_factor *= max(texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).g, 0.0);
    }
    if (material.opacity_texture >= 0 && material.opacity_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.opacity_texture;
        float opacity = texture(material_textures[nonuniformEXT(textureIndex)], uv0).r;
        material.alpha_factor *= clamp(opacity, 0.0, 1.0);
    }
}

vec3 material_volume_transmittance(Material material, float distance) {
    float thickness = max(material.volume_thickness_factor, 0.0);
    if (thickness <= 1.0e-6) {
        return vec3(1.0);
    }
    float attenuationDistance = material.volume_attenuation_distance;
    if (attenuationDistance <= 0.0 || attenuationDistance >= 1.0e20) {
        return vec3(1.0);
    }
    vec3 color = clamp(material.volume_attenuation_color, vec3(1.0e-4), vec3(1.0));
    vec3 attenuationCoefficient = -log(color) / max(attenuationDistance, 1.0e-6);
    return exp(-attenuationCoefficient * max(distance * thickness, 0.0));
}

void apply_material_vertex_color(inout Material material, vec4 vertexColor) {
    material.color *= clamp(vertexColor.rgb, vec3(0.0), vec3(1.0));
    material.alpha_factor *= clamp(vertexColor.a, 0.0, 1.0);
}

void apply_material_alpha_texture(inout Material material, vec2 uv0, vec2 uv1) {
    if (material.base_color_texture >= 0 && material.base_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.base_color_texture;
        vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_BASE_COLOR, uv0, uv1);
        vec4 base = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv);
        material.alpha_factor *= base.a;
    }
    if (material.opacity_texture >= 0 && material.opacity_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.opacity_texture;
        float opacity = texture(material_textures[nonuniformEXT(textureIndex)], uv0).r;
        material.alpha_factor *= clamp(opacity, 0.0, 1.0);
    }
}

bool accept_material_alpha(Material material) {
    if (material.alpha_mode == ALPHA_MODE_MASK) {
        return material.alpha_factor >= material.alpha_cutoff;
    }
    if (material.alpha_mode == ALPHA_MODE_BLEND) {
        return material.alpha_factor >= 0.10;
    }
    return true;
}

vec3 apply_normal_texture(inout Material material, vec2 uv0, vec2 uv1, vec3 normal, vec3 tangent, vec3 bitangent, vec3 rayDirection) {
    if (material.normal_texture < 0 || material.normal_texture >= MATERIAL_TEXTURE_LIMIT) {
        return normal;
    }
    int textureIndex = material.normal_texture;
    vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_NORMAL, uv0, uv1);
    vec3 tangentSample = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).xyz * 2.0 - 1.0;
    uint flags = uint(round(material.pad2));
    if ((flags & MATERIAL_FLAG_NORMAL_MAP_DIRECTX) != 0u) {
        tangentSample.y = -tangentSample.y;
    }
    if (tangentSample.z < 0.0) {
        tangentSample.z = sqrt(max(1.0 - dot(tangentSample.xy, tangentSample.xy), 0.0));
    }
    material.normal_variance = clamp(dot(tangentSample.xy, tangentSample.xy), 0.0, 1.0);
    vec3 t = normalize(tangent - normal * dot(normal, tangent));
    vec3 b = normalize(bitangent - normal * dot(normal, bitangent));
    vec3 mapped = normalize(t * tangentSample.x + b * tangentSample.y + normal * max(tangentSample.z, 0.0));
    return dot(mapped, rayDirection) > 0.0 ? -mapped : mapped;
}

vec3 apply_clearcoat_normal_texture(inout Material material, vec2 uv0, vec2 uv1, vec3 normal, vec3 tangent, vec3 bitangent, vec3 rayDirection) {
    material.clearcoat_normal = normal;
    material.clearcoat_normal_variance = 0.0;
    if (material.clearcoat_normal_texture < 0 || material.clearcoat_normal_texture >= MATERIAL_TEXTURE_LIMIT) {
        return normal;
    }
    int textureIndex = material.clearcoat_normal_texture;
    vec2 sampleUv = apply_material_texture_transform(material, MATERIAL_TEXTURE_TRANSFORM_CLEARCOAT_NORMAL, uv0, uv1);
    vec3 tangentSample = texture(material_textures[nonuniformEXT(textureIndex)], sampleUv).xyz * 2.0 - 1.0;
    uint flags = uint(round(material.pad2));
    if ((flags & MATERIAL_FLAG_NORMAL_MAP_DIRECTX) != 0u) {
        tangentSample.y = -tangentSample.y;
    }
    if (tangentSample.z < 0.0) {
        tangentSample.z = sqrt(max(1.0 - dot(tangentSample.xy, tangentSample.xy), 0.0));
    }
    material.clearcoat_normal_variance = clamp(dot(tangentSample.xy, tangentSample.xy), 0.0, 1.0);
    vec3 t = normalize(tangent - normal * dot(normal, tangent));
    vec3 b = normalize(bitangent - normal * dot(normal, bitangent));
    vec3 mapped = normalize(t * tangentSample.x + b * tangentSample.y + normal * max(tangentSample.z, 0.0));
    material.clearcoat_normal = dot(mapped, rayDirection) > 0.0 ? -mapped : mapped;
    return material.clearcoat_normal;
}

vec3 material_clearcoat_normal(Material material, vec3 fallbackNormal) {
    return dot(material.clearcoat_normal, material.clearcoat_normal) > 1.0e-8
        ? normalize(material.clearcoat_normal)
        : fallbackNormal;
}

bool specular_aa_enabled() {
    return camera.restir_gi_controls.w != 0u;
}

float material_specular_roughness(Material material) {
    float roughness = material.roughness;
    if (!specular_aa_enabled() || material.normal_variance <= 1.0e-5) {
        return roughness;
    }
    float r2 = roughness * roughness;
    float aaVariance = material.normal_variance * 0.18;
    return clamp(sqrt(clamp(r2 + aaVariance, 0.0, 1.0)), roughness, 1.0);
}

Material apply_debug_material_mode(Material material) {
    if (debug_params.view == 22u || debug_params.view == 27u) {
        material.color = vec3(0.72, 0.70, 0.66);
        material.roughness = 0.85;
        material.metallic = 0.0;
        material.mat_type = 0u;
        material.emissive = vec3(0.0);
    }
    return material;
}

vec3 rotate_y(vec3 v, float angle);
vec2 env_uv_from_dir(vec3 dir);

vec3 environment_color(vec3 direction) {
    if (debug_params.view == 27u) {
        return vec3(DEBUG_WHITE_ENV_RADIANCE);
    }
    if (env_params.enabled != 0u && env_params.width > 1u && env_params.height > 1u) {
        vec3 localDir = rotate_y(direction, env_params.rotation);
        vec2 uv = env_uv_from_dir(localDir);
        float scale = env_params.procedural != 0u ? camera.sky_intensity : env_params.intensity;
        return texture(sampler2D(env_map, env_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb *
            scale * env_params.background_intensity;
    }
    float t = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.70, 0.74, 0.80), vec3(0.56, 0.68, 0.92), t) *
        camera.sky_intensity * env_params.background_intensity;
}

vec3 rotate_y(vec3 v, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

vec2 env_uv_from_dir(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

vec3 env_dir_from_uv(vec2 uv) {
    float phi = (uv.x - 0.5) * 2.0 * PI;
    float lat = (uv.y - 0.5) * PI;
    float cosLat = cos(lat);
    return normalize(vec3(cosLat * cos(phi), sin(lat), cosLat * sin(phi)));
}

vec3 analytical_sun_direction() {
    return normalize(camera.sun_direction_illuminance.xyz);
}

float analytical_sun_visibility() {
    if (camera.sunlight_enabled == 0u) {
        return 0.0;
    }
    return analytical_sun_direction().y > 0.0 ? 1.0 : 0.0;
}

float analytical_sun_solid_angle() {
    float radius = clamp(camera.sun_color_angular_radius.w, 0.0001, 0.08);
    return max(2.0 * PI * (1.0 - cos(radius)), 1.0e-8);
}

float analytical_sun_pdf(vec3 dir) {
    if (analytical_sun_visibility() <= 0.0) {
        return 0.0;
    }
    vec3 sunDir = analytical_sun_direction();
    float radius = clamp(camera.sun_color_angular_radius.w, 0.0001, 0.08);
    return dot(normalize(dir), sunDir) >= cos(radius) ? 1.0 / analytical_sun_solid_angle() : 0.0;
}

float atmosphere_planet_horizon_visibility(vec3 scenePos, vec3 dir, float width) {
    vec3 planetary = atmosphere_scene_to_planetary(scenePos);
    float radius = max(length(planetary), ATMOSPHERE_PLANET_RADIUS + 1.0);
    float horizonMu = -sqrt(max(1.0 - (ATMOSPHERE_PLANET_RADIUS * ATMOSPHERE_PLANET_RADIUS) / (radius * radius), 0.0));
    float viewMu = dot(normalize(dir), normalize(planetary));
    return smoothstep(horizonMu - width, horizonMu + width, viewMu);
}

vec3 analytical_sun_center_radiance() {
    if (analytical_sun_visibility() <= 0.0) {
        return vec3(0.0);
    }
    return max(camera.sun_color_angular_radius.rgb, vec3(0.0)) *
        max(camera.sun_direction_illuminance.w, 0.0) / analytical_sun_solid_angle();
}

vec3 analytical_sun_disk_radiance(vec3 dir) {
    if (analytical_sun_visibility() <= 0.0) {
        return vec3(0.0);
    }
    vec3 sunDir = analytical_sun_direction();
    float rayHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, dir, 0.003);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.006);
    if (rayHorizon <= 1.0e-4 || sunHorizon <= 1.0e-4) {
        return vec3(0.0);
    }
    float radius = clamp(camera.sun_angular_radius, 0.00465, 0.08);
    float cosAngle = dot(normalize(dir), sunDir);
    float cosRadius = cos(radius);
    float disk = smoothstep(cosRadius, mix(cosRadius, 1.0, 0.18), cosAngle);
    float limb = 0.62 + 0.38 * sqrt(clamp((cosAngle - cosRadius) / max(1.0 - cosRadius, 1.0e-5), 0.0, 1.0));
    return analytical_sun_center_radiance() * disk * limb * rayHorizon * sunHorizon;
}

float atmosphere_saturate(float v) {
    return clamp(v, 0.0, 1.0);
}

vec3 visible_sun_core(vec3 viewDir, vec3 sunDir, float sunVisibility, float sunHorizon, float horizonVisibility) {
    float cosTheta = clamp(dot(normalize(viewDir), normalize(sunDir)), -1.0, 1.0);
    float angle = acos(cosTheta);
    float core = 1.0 - smoothstep(0.010, 0.018, angle);
    float rim = 1.0 - smoothstep(0.018, 0.030, angle);
    float sunHeight = smoothstep(-0.08, 0.22, sunDir.y);
    vec3 lowTint = vec3(1.0, 0.56, 0.28);
    vec3 highTint = vec3(1.0, 0.93, 0.72);
    vec3 tint = mix(lowTint, highTint, sunHeight);
    return tint * (core * 18.0 + rim * 3.0) * sunVisibility * sunHorizon * horizonVisibility;
}

vec3 high_resolution_sun_disk_radiance(vec3 dir) {
    if (analytical_sun_visibility() <= 0.0) {
        return vec3(0.0);
    }
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    float rayHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, viewDir, 0.003);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.006);
    if (rayHorizon <= 1.0e-4 || sunHorizon <= 1.0e-4) {
        return vec3(0.0);
    }
    float sunVisibility = smoothstep(-0.08, 0.08, sunDir.y);
    return visible_sun_core(viewDir, sunDir, sunVisibility, sunHorizon, rayHorizon) * camera.sky_intensity;
}

vec3 environment_sun_disk_radiance(vec3 dir) {
    if (analytical_sun_visibility() <= 0.0) {
        return vec3(0.0);
    }
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    vec3 centerRadiance = analytical_sun_center_radiance();
    if (dot(centerRadiance, vec3(0.2126, 0.7152, 0.0722)) <= 1.0e-6) {
        return vec3(0.0);
    }
    float rayHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, viewDir, 0.003);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.006);
    if (rayHorizon <= 1.0e-4 || sunHorizon <= 1.0e-4) {
        return vec3(0.0);
    }

    float cosAngle = dot(viewDir, sunDir);
    float radius = clamp(camera.sun_angular_radius, 0.00465, 0.08);
    float cosRadius = cos(radius);
    float disk = smoothstep(cosRadius, mix(cosRadius, 1.0, 0.18), cosAngle);
    float limb = 0.62 + 0.38 * sqrt(clamp((cosAngle - cosRadius) / max(1.0 - cosRadius, 1.0e-5), 0.0, 1.0));
    float sunVisibility = smoothstep(-0.08, 0.08, sunDir.y);
    vec3 core = visible_sun_core(viewDir, sunDir, sunVisibility, sunHorizon, rayHorizon);
    return (centerRadiance * disk * limb + core) * rayHorizon * sunHorizon * camera.sky_intensity;
}

vec3 unreal_sky_grade(vec3 dir, vec3 physicalSky) {
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    float viewY = viewDir.y;
    float activeSun = analytical_sun_visibility();
    float sunUp = clamp(sunDir.y, -0.12, 1.0);
    float sunVisibility = activeSun * smoothstep(-0.08, 0.08, sunUp);
    float lowSun = 1.0 - smoothstep(0.18, 0.82, sunUp);
    float sunset = 1.0 - smoothstep(0.02, 0.34, sunUp);
    float horizonVisibility = atmosphere_planet_horizon_visibility(camera.pos.xyz, viewDir, 0.006);
    float sunHorizon = atmosphere_planet_horizon_visibility(camera.pos.xyz, sunDir, 0.010);
    float horizon = pow(1.0 - smoothstep(-0.02, 0.62, viewY), 1.65);
    float cosTheta = clamp(dot(viewDir, sunDir), -1.0, 1.0);

    vec3 dayZenith = vec3(0.32, 0.50, 0.78);
    vec3 dayHorizon = vec3(0.74, 0.84, 0.96);
    vec3 lowZenith = vec3(0.43, 0.47, 0.63);
    vec3 sunsetZenith = vec3(0.36, 0.34, 0.50);
    vec3 lowHorizon = vec3(1.0, 0.72, 0.46);
    vec3 sunsetHorizon = vec3(1.0, 0.52, 0.30);
    vec3 zenithColor = mix(dayZenith, mix(lowZenith, sunsetZenith, sunset), lowSun);
    vec3 horizonColor = mix(dayHorizon, mix(lowHorizon, sunsetHorizon, sunset), lowSun);
    vec3 palette = mix(zenithColor, horizonColor, horizon);

    float physicalLum = dot(max(physicalSky, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    vec3 sky = palette * (0.55 + 0.28 * atmosphere_saturate(physicalLum)) + physicalSky * 0.13;

    float haloTight = pow(atmosphere_saturate(cosTheta), mix(42.0, 16.0, lowSun));
    float haloWide = pow(atmosphere_saturate(cosTheta), mix(10.0, 5.5, lowSun));
    vec3 haloColor = mix(vec3(1.0, 0.94, 0.78), vec3(1.0, 0.62, 0.34), lowSun);
    sky += haloColor * sunVisibility * sunHorizon * horizonVisibility * (haloTight * 0.26 + haloWide * 0.045);

    return max(sky * camera.sky_intensity * horizonVisibility, vec3(0.0));
}

vec3 fast_sky_radiance(vec3 dir) {
    vec3 viewDir = normalize(dir);
    vec3 sunDir = analytical_sun_direction();
    float activeSun = analytical_sun_visibility();
    float viewUp = clamp(viewDir.y, -0.08, 1.0);
    float sunUp = clamp(sunDir.y, -0.08, 1.0);
    float cosTheta = clamp(dot(viewDir, sunDir), -1.0, 1.0);
    float sunVisibility = activeSun * smoothstep(-0.06, 0.08, sunUp);
    float viewMass = atmosphere_air_mass(viewUp);
    float sunMass = atmosphere_air_mass(sunUp);
    float horizon = pow(1.0 - clamp(viewUp, 0.0, 1.0), 2.0);

    vec3 rayleighBeta = vec3(0.170, 0.398, 0.970);
    vec3 mieBeta = vec3(0.82, 0.74, 0.62);
    vec3 transmittance = exp(-(rayleighBeta * 0.30 + mieBeta * 0.08) * (viewMass + sunMass * 0.65));
    vec3 sunsetScatter = vec3(1.0, 0.42, 0.12) * smoothstep(-0.08, 0.18, horizon) * (1.0 - smoothstep(0.05, 0.55, sunUp));

    vec3 rayleigh = rayleighBeta * atmosphere_rayleigh_phase(cosTheta) * (0.55 + horizon * 0.75);
    vec3 mie = mieBeta * atmosphere_mie_phase(cosTheta, 0.78) * (0.05 + horizon * 0.26);
    vec3 sky = (rayleigh + mie + sunsetScatter * 0.10) * transmittance * sunVisibility;

    vec3 night = vec3(0.004, 0.006, 0.012) * smoothstep(-0.25, -0.05, sunUp);
    return unreal_sky_grade(viewDir, sky * 5.5 * 0.70 + night);
}

vec3 analytical_sun_radiance(vec3 dir) {
    return analytical_sun_disk_radiance(dir);
}

vec2 atmosphere_latlong_uv(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

vec3 sample_sky_view_lut(vec3 dir) {
    vec2 uv = atmosphere_latlong_uv(dir);
    return texture(sampler2D(atmosphere_sky_view_lut, atmosphere_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
}

vec3 atmosphere_sky_radiance(vec3 dir, uint quality);

bool sky_cdf_available() {
    uint width = max(env_params.sky_cdf_width, 1u);
    uint height = max(env_params.sky_cdf_height, 1u);
    return env_params.procedural != 0u && sky_cdf_cols.length() >= width * height;
}

float sky_cdf_pixel_probability(uint idx) {
    float previous = idx > 0u ? sky_cdf_cols[idx - 1u] : 0.0;
    return max(sky_cdf_cols[idx] - previous, 0.0);
}

float sky_cdf_direction_pdf(vec3 dir) {
    vec2 uv = atmosphere_latlong_uv(dir);
    uint width = max(env_params.sky_cdf_width, 1u);
    uint height = max(env_params.sky_cdf_height, 1u);
    uint col = uint(clamp(fract(uv.x) * float(width), 0.0, float(width - 1u)));
    uint row = uint(clamp(clamp(uv.y, 0.0, 1.0) * float(height), 0.0, float(height - 1u)));
    uint idx = row * width + col;
    float lat = ((float(row) + 0.5) / float(height) - 0.5) * PI;
    float sinTheta = max(cos(lat), 0.001);
    return sky_cdf_pixel_probability(idx) * float(width * height) * SKY_CDF_HALF_RCP_PI_SQ / sinTheta;
}

vec3 sample_sky_cdf_direction(inout uint state, out vec3 out_dir, out float out_pdf) {
    float u = rand_f32(state);
    uint width = max(env_params.sky_cdf_width, 1u);
    uint height = max(env_params.sky_cdf_height, 1u);
    uint totalPixels = width * height;
    uint low = 0u;
    uint high = totalPixels - 1u;
    while (low < high) {
        uint mid = (low + high) / 2u;
        if (sky_cdf_cols[mid] < u) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    uint x = low % width;
    uint y = low / width;
    vec2 uv = (vec2(x, y) + vec2(rand_f32(state), rand_f32(state))) / vec2(width, height);
    out_dir = env_dir_from_uv(uv);
    out_pdf = sky_cdf_direction_pdf(out_dir);
    return atmosphere_sky_radiance(out_dir, ATMOSPHERE_RAY_QUALITY_FULL);
}

float atmosphere_horizon_visibility(vec3 scenePos, vec3 dir) {
    return atmosphere_planet_horizon_visibility(scenePos, dir, 0.004);
}

vec3 atmosphere_sky_radiance(vec3 dir, uint quality) {
    vec3 viewDir = normalize(dir);
    if (quality == ATMOSPHERE_RAY_QUALITY_MINIMAL) {
        return vec3(0.0);
    }
    if (camera.sunlight_enabled == 0u) {
        return fast_sky_radiance(viewDir);
    }
    if (quality == ATMOSPHERE_RAY_QUALITY_FAST) {
        return fast_sky_radiance(viewDir);
    }
    vec3 sampled = sample_sky_view_lut(viewDir);
    float sampledLuminance = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    if (sampledLuminance > 1.0e-5) {
        return sampled;
    }
    return fast_sky_radiance(viewDir);
}

vec3 sample_atmosphere_transmittance_lut(vec3 worldPos, vec3 dir) {
    vec3 planetary = atmosphere_scene_to_planetary(worldPos);
    float heightMeters = max(length(planetary) - ATMOSPHERE_PLANET_RADIUS, 0.0);
    float mu = dot(normalize(dir), normalize(planetary));
    float atmosphereHeight = max(ATMOSPHERE_TOP_RADIUS - ATMOSPHERE_PLANET_RADIUS, 1.0);
    vec2 uv = vec2(clamp((mu + 0.20) / 1.20, 0.0, 1.0), clamp(heightMeters / atmosphereHeight, 0.0, 1.0));
    vec3 sampled = texture(sampler2D(atmosphere_transmittance_lut, atmosphere_sampler), uv).rgb;
    float sampledLuminance = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    return sampledLuminance > 1.0e-5 ? sampled : vec3(1.0);
}

vec3 sample_multi_scatter_lut_debug(vec3 dir) {
    vec3 sunDir = analytical_sun_direction();
    float viewMu = clamp(normalize(dir).y, -0.20, 1.0);
    float sunMu = clamp(sunDir.y, -0.20, 1.0);
    vec2 uv = vec2(clamp((viewMu + 0.20) / 1.20, 0.0, 1.0), clamp((sunMu + 0.20) / 1.20, 0.0, 1.0));
    return texture(sampler2D(atmosphere_multi_scatter_lut, atmosphere_sampler), uv).rgb;
}

vec3 sun_transmittance(vec3 worldPos, vec3 sunDir) {
    return sample_atmosphere_transmittance_lut(worldPos, sunDir);
}

vec3 apply_analytical_aerial_perspective(vec3 radiance, vec3 origin, vec3 direction, float distanceMeters) {
    if (distanceMeters <= 0.0 || distanceMeters >= 100000.0) {
        return radiance;
    }
    vec3 dirNorm = normalize(direction);
    vec3 planetary = atmosphere_scene_to_planetary(origin);
    float cosZenith = clamp(dot(dirNorm, normalize(planetary)), -1.0, 1.0);
    float heightMeters = max(length(planetary) - ATMOSPHERE_PLANET_RADIUS, 0.0);
    float atmosphereHeight = max(ATMOSPHERE_TOP_RADIUS - ATMOSPHERE_PLANET_RADIUS, 1.0);
    float distanceNormalized = clamp((max(distanceMeters, 1.0) - 1.0) / (100000.0 - 1.0), 0.0, 1.0);
    float depthNormalized = pow(distanceNormalized, 1.0 / 3.0);
    vec3 uvw = vec3(cosZenith * 0.5 + 0.5, clamp(heightMeters / atmosphereHeight, 0.0, 1.0), clamp(depthNormalized, 0.0, 1.0));
    vec4 aerial = texture(sampler3D(atmosphere_aerial_perspective_lut, atmosphere_sampler), uvw);
    float aerialLuminance = dot(aerial.rgb, vec3(0.2126, 0.7152, 0.0722));
    if (aerial.a <= 1.0e-5 && aerialLuminance <= 1.0e-5) {
        return radiance;
    }
    return radiance * clamp(aerial.a, 0.0, 1.0) + max(aerial.rgb, vec3(0.0));
}

vec3 environment_radiance(vec3 dir, uint quality) {
    if (debug_params.view == 27u) {
        return vec3(DEBUG_WHITE_ENV_RADIANCE);
    }
    if (env_params.enabled != 0u) {
        if (env_params.procedural != 0u) {
            return atmosphere_sky_radiance(dir, quality);
        }
        vec3 localDir = rotate_y(dir, env_params.rotation);
        vec2 uv = env_uv_from_dir(localDir);
        vec3 sampled = texture(sampler2D(env_map, env_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
        return sampled * env_params.intensity;
    }
    return vec3(0.0);
}

vec3 environment_background_radiance(vec3 dir, uint quality) {
    if (debug_params.view == 27u) {
        return vec3(DEBUG_WHITE_ENV_RADIANCE);
    }
    if (env_params.enabled != 0u && env_params.procedural == 0u) {
        vec3 localDir = rotate_y(dir, env_params.rotation);
        vec2 uv = env_uv_from_dir(localDir);
        vec3 sampled = texture(sampler2D(env_map, env_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
        return sampled * env_params.intensity;
    }
    return atmosphere_sky_radiance(dir, quality);
}

vec3 debug_display_tonemap(vec3 color) {
    color = max(color, vec3(0.0));
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

float environment_pdf(vec3 dir) {
    if (env_params.enabled == 0u) {
        return 0.0;
    }
    if (sky_cdf_available()) {
        return sky_cdf_direction_pdf(dir);
    }
    if (env_params.width == 0u || env_params.height == 0u || env_params.inv_total_lum <= 0.0) {
        if (env_params.procedural != 0u) {
            vec3 radiance = atmosphere_sky_radiance(dir, ATMOSPHERE_RAY_QUALITY_FULL);
            float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
            if (lum <= 1.0e-5) {
                return 1.0 / (4.0 * PI);
            }
            float lat = asin(clamp(normalize(dir).y, -1.0, 1.0));
            float sinTheta = max(cos(lat), 0.001);
            return lum / (2.0 * PI * PI * max(sinTheta, 0.001));
        }
        return 1.0 / (4.0 * PI);
    }
    vec3 localDir = rotate_y(dir, env_params.rotation);
    vec2 uv = env_uv_from_dir(localDir);
    uint col = uint(clamp(uv.x * float(env_params.width), 0.0, float(env_params.width - 1u)));
    uint row = uint(clamp(uv.y * float(env_params.height), 0.0, float(env_params.height - 1u)));
    vec3 sampleValue = texelFetch(sampler2D(env_map, env_sampler), ivec2(int(col), int(row)), 0).rgb;
    float lum = dot(sampleValue, vec3(0.2126, 0.7152, 0.0722));
    float lat = ((float(row) + 0.5) / float(env_params.height) - 0.5) * PI;
    float sinTheta = max(cos(lat), 0.001);
    return max(lum * float(env_params.width) * float(env_params.height) * env_params.inv_total_lum / (2.0 * PI * PI * sinTheta), 0.0);
}

vec3 sample_environment_direction(inout uint state, out vec3 out_dir, out float out_pdf) {
    out_pdf = 0.0;
    out_dir = vec3(0.0, 1.0, 0.0);
    if (env_params.enabled == 0u) {
        return vec3(0.0);
    }
    if (sky_cdf_available()) {
        return sample_sky_cdf_direction(state, out_dir, out_pdf);
    }
    if (env_params.width == 0u || env_params.height == 0u || env_params.inv_total_lum <= 0.0) {
        float z = 1.0 - 2.0 * rand_f32(state);
        float phi = 2.0 * PI * rand_f32(state);
        float r = sqrt(max(1.0 - z * z, 0.0));
        out_dir = vec3(r * cos(phi), z, r * sin(phi));
        out_pdf = 1.0 / (4.0 * PI);
        return atmosphere_sky_radiance(out_dir, ATMOSPHERE_RAY_QUALITY_FULL);
    }

    float rowSample = rand_f32(state) * float(env_params.height);
    uint rowCandidate = min(uint(rowSample), env_params.height - 1u);
    vec2 rowAlias = env_alias_rows[rowCandidate];
    uint row = fract(rowSample) <= rowAlias.x ? rowCandidate : min(uint(rowAlias.y + 0.5), env_params.height - 1u);

    float colSample = rand_f32(state) * float(env_params.width);
    uint colCandidate = min(uint(colSample), env_params.width - 1u);
    uint colOffset = row * env_params.width;
    vec2 colAlias = env_alias_cols[colOffset + colCandidate];
    uint col = fract(colSample) <= colAlias.x ? colCandidate : min(uint(colAlias.y + 0.5), env_params.width - 1u);
    vec2 uv = vec2((float(col) + 0.5) / float(env_params.width), (float(row) + 0.5) / float(env_params.height));
    out_dir = rotate_y(env_dir_from_uv(uv), -env_params.rotation);
    if (env_params.procedural != 0u) {
        vec3 radiance = atmosphere_sky_radiance(out_dir, ATMOSPHERE_RAY_QUALITY_FULL);
        out_pdf = environment_pdf(out_dir);
        return radiance;
    }
    vec3 radiance = texelFetch(sampler2D(env_map, env_sampler), ivec2(int(col), int(row)), 0).rgb * env_params.intensity;
    out_pdf = environment_pdf(out_dir);
    return radiance;
}

float power_heuristic(float pdf_a, float pdf_b) {
    float a2 = pdf_a * pdf_a;
    float b2 = pdf_b * pdf_b;
    return a2 / max(a2 + b2, 1e-8);
}

uint decode_light_bvh_node_info(float packed, out uint childCount, out uint childOrLightOffset, out uint lightCount) {
    uint bits = floatBitsToUint(packed);
    if ((bits & 0x80000000u) != 0u) {
        lightCount = 1u;
        childOrLightOffset = bits & 0x7fffffffu;
        childCount = 0u;
        return 1u;
    }
    childCount = bits != 0u ? 2u : 0u;
    childOrLightOffset = bits & 0x7fffffffu;
    lightCount = 0u;
    return 0u;
}

bool sample_light_bvh(inout uint rng, out uint lightIndex) {
    if (mesh_params.light_count == 0u) {
        return false;
    }
    uint nodeIndex = 0u;
    for (uint guard = 0u; guard < 64u; ++guard) {
        vec4 data0 = light_bvh_nodes[nodeIndex * 2u];
        vec4 data1 = light_bvh_nodes[nodeIndex * 2u + 1u];
        float totalPower = data0.w;
        uint childCount;
        uint childOrLightOffset;
        uint lightCount;
        bool isLeaf = decode_light_bvh_node_info(data1.w, childCount, childOrLightOffset, lightCount) != 0u;
        if (isLeaf) {
            if (lightCount == 0u || lightCount > mesh_params.light_count || childOrLightOffset >= mesh_params.light_count) {
                return false;
            }
            if (lightCount == 1u) {
                lightIndex = childOrLightOffset;
            } else {
                uint localIndex = min(uint(rand_f32(rng) * float(lightCount)), lightCount - 1u);
                lightIndex = min(childOrLightOffset + localIndex, mesh_params.light_count - 1u);
            }
            return true;
        }
        uint maxNodeCount = max(mesh_params.light_count * 2u, 1u);
        if (childCount == 0u || childOrLightOffset + childCount > maxNodeCount) {
            return false;
        }
        float r = rand_f32(rng) * totalPower;
        float cumulativePower = 0.0;
        uint nextNodeIndex = childOrLightOffset + childCount - 1u;
        for (uint ci = 0u; ci < childCount; ++ci) {
            float childPower = light_bvh_nodes[(childOrLightOffset + ci) * 2u].w;
            cumulativePower += childPower;
            if (r <= cumulativePower) {
                nextNodeIndex = childOrLightOffset + ci;
                break;
            }
        }
        nodeIndex = nextNodeIndex;
    }
    return false;
}

bool light_record_is_authored(uint type) {
    return type >= 2u && type <= 5u;
}

bool light_record_is_emissive(uint type) {
    return type == 0u || type == 1u;
}

float authored_light_sample_probability() {
    if (mesh_params.authored_light_count == 0u || mesh_params.light_count == 0u) {
        return 0.0;
    }
    return mesh_params.light_count > mesh_params.authored_light_count ? 0.5 : 1.0;
}

float light_bvh_sample_probability() {
    if (mesh_params.light_count == 0u || mesh_params.emissive_total_area <= 1.0e-8) {
        return 0.0;
    }
    return 1.0 - authored_light_sample_probability();
}

bool sample_authored_light(inout uint rng, out uint lightIndex) {
    if (mesh_params.authored_light_count == 0u || mesh_params.authored_light_offset >= mesh_params.light_count) {
        return false;
    }
    uint localIndex = min(uint(rand_f32(rng) * float(mesh_params.authored_light_count)), mesh_params.authored_light_count - 1u);
    lightIndex = mesh_params.authored_light_offset + localIndex;
    return lightIndex < mesh_params.light_count && light_record_is_authored(light_records[lightIndex].metadata.x);
}

float light_record_selection_pdf(uint lightIndex) {
    if (lightIndex >= mesh_params.light_count || mesh_params.emissive_total_area <= 1.0e-8) {
        return 0.0;
    }
    LightRecord light = light_records[lightIndex];
    float pdf = light_bvh_sample_probability() * max(light.data0.x, 0.0) / max(mesh_params.emissive_total_area, 1.0e-6);
    if (light_record_is_authored(light.metadata.x) && mesh_params.authored_light_count > 0u) {
        pdf += authored_light_sample_probability() / float(mesh_params.authored_light_count);
    }
    return pdf;
}

bool sample_scene_light(inout uint rng, out uint lightIndex) {
    float authoredProbability = authored_light_sample_probability();
    if (authoredProbability > 0.0 && rand_f32(rng) < authoredProbability) {
        if (sample_authored_light(rng, lightIndex)) {
            return true;
        }
    }
    return sample_light_bvh(rng, lightIndex);
}

float reflectance(float cosine, float ref_idx) {
    float r0 = (1.0 - ref_idx) / (1.0 + ref_idx);
    r0 = r0 * r0;
    float t = clamp(1.0 - cosine, 0.0, 1.0);
    float t2 = t * t;
    return r0 + (1.0 - r0) * (t2 * t2 * t);
}

vec3 material_dispersion_ior(Material material) {
    float baseIor = max(material.ior, 1.01);
    float halfSpread = (baseIor - 1.0) * 0.025 * max(material.dispersion_factor, 0.0);
    return max(vec3(baseIor - halfSpread, baseIor, baseIor + halfSpread), vec3(1.01));
}

uint material_dispersion_channel(Material material, float sampleValue) {
    if (material.dispersion_factor <= 1.0e-6) {
        return 1u;
    }
    return min(uint(floor(clamp(sampleValue, 0.0, 0.999999) * 3.0)), 2u);
}

float material_channel_value(vec3 value, uint channel) {
    return channel == 0u ? value.r : (channel == 1u ? value.g : value.b);
}

vec3 material_dispersion_channel_weight(Material material, uint channel) {
    if (material.dispersion_factor <= 1.0e-6) {
        return vec3(1.0);
    }
    return channel == 0u ? vec3(3.0, 0.0, 0.0) : (channel == 1u ? vec3(0.0, 3.0, 0.0) : vec3(0.0, 0.0, 3.0));
}

float schlick_fresnel_pow5(float t) {
    float t2 = t * t;
    return t2 * t2 * t;
}

float diffuse_pdf(vec3 normal, vec3 wi) {
    return max(dot(normal, wi), 0.0) / PI;
}

bool use_lambert_diffuse(Material material) {
    return material.roughness <= 0.08 || camera.path_tracing_enabled == 0u;
}

float oren_nayar_diffuse_factor(Material material, vec3 wo, vec3 wi, vec3 n) {
    if (use_lambert_diffuse(material)) {
        return 1.0;
    }

    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v <= 0.0 || n_dot_l <= 0.0) {
        return 0.0;
    }

    float sigma = clamp(material.roughness, 0.0, 1.0) * 1.2217304764; // 70 degrees in radians.
    float sigma2 = sigma * sigma;
    float a = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    float b = 0.45 * sigma2 / (sigma2 + 0.09);

    vec3 viewTangent = wo - n * n_dot_v;
    vec3 lightTangent = wi - n * n_dot_l;
    float viewLen2 = dot(viewTangent, viewTangent);
    float lightLen2 = dot(lightTangent, lightTangent);
    float cosPhiDiff = viewLen2 > 1.0e-8 && lightLen2 > 1.0e-8
        ? dot(viewTangent, lightTangent) * inversesqrt(viewLen2 * lightLen2)
        : 0.0;
    float sinAlpha = sqrt(max(1.0 - max(n_dot_v, n_dot_l) * max(n_dot_v, n_dot_l), 0.0));
    float tanBeta = sqrt(max(1.0 - min(n_dot_v, n_dot_l) * min(n_dot_v, n_dot_l), 0.0)) /
        max(min(n_dot_v, n_dot_l), 1.0e-4);

    return clamp(a + b * max(0.0, cosPhiDiff) * sinAlpha * tanBeta, 0.0, 1.0);
}

vec3 eval_diffuse_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    return material.color * (1.0 / PI) * oren_nayar_diffuse_factor(material, wo, wi, n);
}

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

bool has_positive_radiance(vec3 color) {
    return any(greaterThan(color, vec3(0.0)));
}

vec3 pbr_f0(Material material) {
    if (material.use_conductor_optics != 0u) {
        vec3 eta = max(material.conductor_eta, vec3(0.0));
        vec3 k = max(material.conductor_k, vec3(0.0));
        vec3 etaMinusOne = eta - vec3(1.0);
        vec3 etaPlusOne = eta + vec3(1.0);
        vec3 k2 = k * k;
        return clamp((etaMinusOne * etaMinusOne + k2) / max(etaPlusOne * etaPlusOne + k2, vec3(1.0e-6)), vec3(0.0), vec3(1.0));
    }
    uint flags = uint(round(material.pad2));
    vec3 dielectricF0 = (flags & MATERIAL_FLAG_SPECULAR_GLOSSINESS_WORKFLOW) != 0u
        ? clamp(material.specular_factor * material.specular_color, vec3(0.0), vec3(1.0))
        : clamp(vec3(0.04) * material.specular_factor * material.specular_color, vec3(0.0), vec3(1.0));
    return mix(dielectricF0, material.color, clamp(material.metallic, 0.0, 1.0));
}

vec3 conductor_fresnel(Material material, float v_dot_h) {
    float cosTheta = clamp(v_dot_h, 0.0, 1.0);
    vec3 eta = max(material.conductor_eta, vec3(0.0));
    vec3 k = max(material.conductor_k, vec3(0.0));
    vec3 eta2 = eta * eta;
    vec3 k2 = k * k;
    float cos2 = cosTheta * cosTheta;
    float sin2 = max(1.0 - cos2, 0.0);
    vec3 t0 = eta2 - k2 - vec3(sin2);
    vec3 a2plusb2 = sqrt(max(t0 * t0 + 4.0 * eta2 * k2, vec3(0.0)));
    vec3 t1 = a2plusb2 + vec3(cos2);
    vec3 a = sqrt(max((a2plusb2 + t0) * 0.5, vec3(0.0)));
    vec3 t2 = 2.0 * cosTheta * a;
    vec3 rs = (t1 - t2) / max(t1 + t2, vec3(1.0e-6));
    vec3 t3 = cos2 * a2plusb2 + vec3(sin2 * sin2);
    vec3 t4 = t2 * sin2;
    vec3 rp = rs * (t3 - t4) / max(t3 + t4, vec3(1.0e-6));
    return clamp((rp + rs) * 0.5, vec3(0.0), vec3(1.0));
}

vec3 pbr_diffuse_reflectance(Material material) {
    return clamp(material.color * (1.0 - clamp(material.metallic, 0.0, 1.0)), vec3(0.0), vec3(1.0));
}

vec3 pbr_specular_reflectance(Material material, float n_dot_v) {
    float cosTheta = clamp(n_dot_v, 0.0, 1.0);
    if (material.use_conductor_optics != 0u) {
        return conductor_fresnel(material, cosTheta);
    }
    vec3 f0 = pbr_f0(material);
    return clamp(f0 + (vec3(1.0) - f0) * schlick_fresnel_pow5(1.0 - cosTheta), vec3(0.0), vec3(1.0));
}

vec3 pbr_average_fresnel(vec3 f0) {
    return f0 + (vec3(1.0) - f0) * (1.0 / 21.0);
}

vec3 pbr_diffuse_energy(Material material) {
    vec3 f0 = pbr_f0(material);
    return clamp(vec3(1.0) - pbr_average_fresnel(f0), vec3(0.0), vec3(1.0)) *
        (1.0 - clamp(material.metallic, 0.0, 1.0)) *
        (1.0 - material_effective_transmission(material));
}

float material_sheen_weight(Material material, float NdotV) {
    float sheenLum = luminance(max(material.sheen_color, vec3(0.0)));
    if (sheenLum <= 1.0e-5) {
        return 0.0;
    }
    float grazing = schlick_fresnel_pow5(1.0 - clamp(NdotV, 0.0, 1.0));
    return sheenLum * mix(0.35, 1.0, grazing);
}

void pbr_lobe_probabilities(Material material, float NdotV, out float diffuseProbability, out float specularProbability, out float sheenProbability, out float clearcoatProbability) {
    vec3 f0 = pbr_f0(material);
    vec3 fresnel = material.use_conductor_optics != 0u
        ? conductor_fresnel(material, NdotV)
        : f0 + (vec3(1.0) - f0) * schlick_fresnel_pow5(1.0 - max(NdotV, 0.0));
    float specularWeight = max(luminance(fresnel), 0.0);
    float diffuseWeight = max(luminance(pbr_diffuse_energy(material) * material.color * (1.0 / PI)), 0.0);
    float sheenWeight = material_sheen_weight(material, NdotV);
    float clearcoatWeight = clamp(material.clearcoat_factor, 0.0, 1.0) * 0.25;
    float totalWeight = max(diffuseWeight + specularWeight + sheenWeight + clearcoatWeight, 1.0e-6);
    diffuseProbability = diffuseWeight / totalWeight;
    specularProbability = specularWeight / totalWeight;
    sheenProbability = sheenWeight / totalWeight;
    clearcoatProbability = clearcoatWeight / totalWeight;
    if (clearcoatWeight > 0.0) {
        clearcoatProbability = clamp(clearcoatProbability, 0.03, 0.25);
        float remaining = 1.0 - clearcoatProbability;
        float baseTotal = max(diffuseWeight + specularWeight + sheenWeight, 1.0e-6);
        diffuseProbability = remaining * diffuseWeight / baseTotal;
        specularProbability = remaining * specularWeight / baseTotal;
        sheenProbability = remaining * sheenWeight / baseTotal;
    }
    if (sheenWeight > 0.0) {
        sheenProbability = clamp(sheenProbability, 0.05, 0.35 * (1.0 - clearcoatProbability));
        float remaining = 1.0 - clearcoatProbability - sheenProbability;
        float baseTotal = max(diffuseWeight + specularWeight, 1.0e-6);
        diffuseProbability = remaining * diffuseWeight / baseTotal;
        specularProbability = remaining * specularWeight / baseTotal;
    }
    specularProbability = clamp(specularProbability, 0.05, max(0.05, 0.95 - sheenProbability - clearcoatProbability));
    diffuseProbability = max(1.0 - specularProbability - sheenProbability - clearcoatProbability, 0.0);
}

float pbr_specular_sample_probability(Material material, float NdotV) {
    float diffuseProbability;
    float specularProbability;
    float sheenProbability;
    float clearcoatProbability;
    pbr_lobe_probabilities(material, NdotV, diffuseProbability, specularProbability, sheenProbability, clearcoatProbability);
    return specularProbability;
}

vec3 sample_cosine_hemisphere(inout uint state, vec3 normal, out float pdf) {
    float r1 = 2.0 * PI * rand_f32(state);
    float r2 = rand_f32(state);
    float r2s = sqrt(r2);
    vec3 axis = abs(normal.x) > 0.1 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(axis, normal));
    vec3 bitangent = cross(normal, tangent);
    vec3 dir = tangent * cos(r1) * r2s + bitangent * sin(r1) * r2s + normal * sqrt(max(1.0 - r2, 0.0));
    pdf = diffuse_pdf(normal, dir);
    return dir;
}

void tangent_frame(vec3 n, out vec3 tangent, out vec3 bitangent) {
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    tangent = normalize(cross(axis, n));
    bitangent = cross(n, tangent);
}

vec3 to_tangent_space(vec3 v, vec3 tangent, vec3 bitangent, vec3 n) {
    return vec3(dot(v, tangent), dot(v, bitangent), dot(v, n));
}

vec3 from_tangent_space(vec3 v, vec3 tangent, vec3 bitangent, vec3 n) {
    return tangent * v.x + bitangent * v.y + n * v.z;
}

float ggx_ndf(float roughness, float n_dot_h) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float a2 = a * a;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-10);
}

vec3 schlick_fresnel(vec3 f0, float v_dot_h) {
    float f = schlick_fresnel_pow5(clamp(1.0 - v_dot_h, 0.0, 1.0));
    return f0 + (vec3(1.0) - f0) * f;
}

bool material_uses_anisotropy(Material material);
void material_anisotropic_frame(Material material, vec3 n, inout vec3 tangent, inout vec3 bitangent);
void material_anisotropic_alpha(Material material, out float alphaX, out float alphaY);
float ggx_anisotropic_ndf(float alphaX, float alphaY, vec3 h, vec3 n, vec3 tangent, vec3 bitangent);
float smith_g1_anisotropic(float alphaX, float alphaY, vec3 v, vec3 n, vec3 tangent, vec3 bitangent);
vec3 sample_ggx_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent);

float smith_g1(float roughness, float n_dot_x) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float a2 = a * a;
    float n2 = n_dot_x * n_dot_x;
    return 2.0 * n_dot_x / max(n_dot_x + sqrt(a2 + (1.0 - a2) * n2), 1e-10);
}

float smith_ggx_lambda(float roughness, float n_dot_x) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float a2 = a * a;
    float n2 = max(n_dot_x * n_dot_x, 1e-8);
    float tan2Theta = max(1.0 - n2, 0.0) / n2;
    return 0.5 * (sqrt(1.0 + a2 * tan2Theta) - 1.0);
}

float ggx_visible_normal_pdf(Material material, vec3 wo, vec3 h, vec3 n, vec3 tangent, vec3 bitangent) {
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(wo, h), 0.0);
    if (n_dot_v < 1e-6 || n_dot_h < 1e-6 || v_dot_h < 1e-6) {
        return 0.0;
    }
    if (material_uses_anisotropy(material)) {
        vec3 t = tangent;
        vec3 b = bitangent;
        material_anisotropic_frame(material, n, t, b);
        float alphaX;
        float alphaY;
        material_anisotropic_alpha(material, alphaX, alphaY);
        return ggx_anisotropic_ndf(alphaX, alphaY, h, n, t, b) *
            smith_g1_anisotropic(alphaX, alphaY, wo, n, t, b) /
            max(4.0 * n_dot_v, 1e-10);
    }
    return ggx_ndf(material.roughness, n_dot_h) * smith_g1(material.roughness, n_dot_v) / max(4.0 * n_dot_v, 1e-10);
}

float smith_g(float roughness, float n_dot_v, float n_dot_l) {
    if (n_dot_v <= 0.0 || n_dot_l <= 0.0) {
        return 0.0;
    }
    float lambdaV = smith_ggx_lambda(roughness, n_dot_v);
    float lambdaL = smith_ggx_lambda(roughness, n_dot_l);
    return 1.0 / max(1.0 + lambdaV + lambdaL, 1e-8);
}

bool material_uses_anisotropy(Material material) {
    return abs(material.anisotropy_strength) > 1.0e-4;
}

void material_anisotropic_frame(Material material, vec3 n, inout vec3 tangent, inout vec3 bitangent) {
    tangent -= n * dot(n, tangent);
    float tangentLen2 = dot(tangent, tangent);
    if (tangentLen2 <= 1.0e-8) {
        tangent_frame(n, tangent, bitangent);
    } else {
        tangent *= inversesqrt(tangentLen2);
        bitangent = normalize(cross(n, tangent));
    }
    float s = sin(material.anisotropy_rotation);
    float c = cos(material.anisotropy_rotation);
    vec3 t = tangent * c + bitangent * s;
    vec3 b = bitangent * c - tangent * s;
    tangent = normalize(t);
    bitangent = normalize(b);
}

void material_anisotropic_alpha(Material material, out float alphaX, out float alphaY) {
    float a = max(ggx_safe_roughness(material.roughness) * ggx_safe_roughness(material.roughness), 1.0e-4);
    float strength = clamp(material.anisotropy_strength, -0.99, 0.99);
    float aspect = sqrt(max(1.0 - 0.9 * abs(strength), 0.1));
    alphaX = strength >= 0.0 ? a / aspect : a * aspect;
    alphaY = strength >= 0.0 ? a * aspect : a / aspect;
}

float ggx_anisotropic_ndf(float alphaX, float alphaY, vec3 h, vec3 n, vec3 tangent, vec3 bitangent) {
    float hx = dot(h, tangent);
    float hy = dot(h, bitangent);
    float hz = max(dot(h, n), 0.0);
    float denom = hx * hx / max(alphaX * alphaX, 1.0e-8) +
        hy * hy / max(alphaY * alphaY, 1.0e-8) +
        hz * hz;
    return 1.0 / max(PI * alphaX * alphaY * denom * denom, 1.0e-10);
}

float smith_ggx_anisotropic_lambda(float alphaX, float alphaY, vec3 v, vec3 n, vec3 tangent, vec3 bitangent) {
    float vx = dot(v, tangent);
    float vy = dot(v, bitangent);
    float vz = max(dot(v, n), 1.0e-6);
    float tan2 = (alphaX * vx) * (alphaX * vx) + (alphaY * vy) * (alphaY * vy);
    return 0.5 * (sqrt(1.0 + tan2 / max(vz * vz, 1.0e-8)) - 1.0);
}

float smith_g1_anisotropic(float alphaX, float alphaY, vec3 v, vec3 n, vec3 tangent, vec3 bitangent) {
    if (dot(v, n) <= 0.0) {
        return 0.0;
    }
    return 1.0 / max(1.0 + smith_ggx_anisotropic_lambda(alphaX, alphaY, v, n, tangent, bitangent), 1.0e-8);
}

float smith_g_anisotropic(float alphaX, float alphaY, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    if (dot(wo, n) <= 0.0 || dot(wi, n) <= 0.0) {
        return 0.0;
    }
    float lambdaV = smith_ggx_anisotropic_lambda(alphaX, alphaY, wo, n, tangent, bitangent);
    float lambdaL = smith_ggx_anisotropic_lambda(alphaX, alphaY, wi, n, tangent, bitangent);
    return 1.0 / max(1.0 + lambdaV + lambdaL, 1.0e-8);
}

float ggx_directional_albedo(float roughness, float n_dot_v) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float a2 = a * a;
    float mu = clamp(n_dot_v, 0.0, 1.0);
    return (mu * (1.0 + a2)) / (mu * (1.0 + a2) + a * (1.0 - mu));
}

vec3 ggx_energy_compensation(vec3 f0, float roughness, float n_dot_v) {
    float r = ggx_safe_roughness(roughness);
    float r2 = r * r;
    float singleScatterEnergy = clamp(1.0 - r2 * (0.45 + 0.25 * (1.0 - clamp(n_dot_v, 0.0, 1.0))), 0.35, 1.0);
    vec3 averageFresnel = f0 + (vec3(1.0) - f0) * (1.0 / 21.0);
    vec3 multiScatter = averageFresnel * (1.0 - singleScatterEnergy) / max(singleScatterEnergy, 1e-4);
    return vec3(1.0) + multiScatter;
}

vec3 heitz_ms_ggx(vec3 f0, float roughness, float n_dot_v, float n_dot_l) {
    float r = ggx_safe_roughness(roughness);
    float a = r * r;
    float E_v = ggx_directional_albedo(roughness, n_dot_v);
    float E_l = ggx_directional_albedo(roughness, n_dot_l);
    float E_avg = clamp(1.0 / (1.0 + a * 0.66), 0.0, 1.0);
    vec3 f_avg = f0 + (vec3(1.0) - f0) / 21.0;
    vec3 f_ms = f_avg * f_avg / max(vec3(1.0) - f_avg * (1.0 - E_avg), vec3(1e-4));
    return f_ms * (1.0 - E_v) * (1.0 - E_l) / max(PI * E_v * E_l, 1e-8);
}

float charlie_inv_alpha(float roughness) {
    float alpha = max(roughness * roughness, 0.001);
    return 1.0 / alpha;
}

float charlie_ndf(float roughness, float n_dot_h) {
    float invAlpha = charlie_inv_alpha(roughness);
    float sin2Theta = max(1.0 - n_dot_h * n_dot_h, 0.0);
    return (2.0 + invAlpha) * pow(sin2Theta, invAlpha * 0.5) / (2.0 * PI);
}

float sheen_visibility(float n_dot_v, float n_dot_l) {
    return 1.0 / max(4.0 * max(n_dot_v, 0.01) * max(n_dot_l, 0.01), 1.0e-4);
}

vec3 eval_sheen_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v <= 1.0e-6 || n_dot_l <= 1.0e-6 || luminance(material.sheen_color) <= 1.0e-6) {
        return vec3(0.0);
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1.0e-12) {
        return vec3(0.0);
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(n, h), 0.0);
    return material.sheen_color * charlie_ndf(material.sheen_roughness, n_dot_h) * sheen_visibility(n_dot_v, n_dot_l);
}

vec3 eval_clearcoat_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    float factor = clamp(material.clearcoat_factor, 0.0, 1.0);
    if (factor <= 1.0e-5) {
        return vec3(0.0);
    }
    vec3 clearcoatN = material_clearcoat_normal(material, n);
    float n_dot_v = max(dot(clearcoatN, wo), 0.0);
    float n_dot_l = max(dot(clearcoatN, wi), 0.0);
    if (n_dot_v <= 1.0e-6 || n_dot_l <= 1.0e-6) {
        return vec3(0.0);
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1.0e-12) {
        return vec3(0.0);
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(clearcoatN, h), 0.0);
    float v_dot_h = max(dot(wo, h), 0.0);
    vec3 f = schlick_fresnel(vec3(0.04), v_dot_h);
    float roughness = clamp(material.clearcoat_roughness, MATERIAL_MIN_GGX_ROUGHNESS, 1.0);
    float d = ggx_ndf(roughness, n_dot_h);
    float g = smith_g(roughness, n_dot_v, n_dot_l);
    return factor * f * d * g / max(4.0 * n_dot_v * n_dot_l, 1.0e-10);
}

float pdf_clearcoat_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    if (material.clearcoat_factor <= 1.0e-5) {
        return 0.0;
    }
    vec3 clearcoatN = material_clearcoat_normal(material, n);
    float n_dot_v = max(dot(clearcoatN, wo), 0.0);
    float n_dot_l = max(dot(clearcoatN, wi), 0.0);
    if (n_dot_v <= 1.0e-6 || n_dot_l <= 1.0e-6) {
        return 0.0;
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1.0e-12) {
        return 0.0;
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(clearcoatN, h), 0.0);
    float v_dot_h = max(dot(wo, h), 1.0e-6);
    float roughness = clamp(material.clearcoat_roughness, MATERIAL_MIN_GGX_ROUGHNESS, 1.0);
    return ggx_ndf(roughness, n_dot_h) * n_dot_h / max(4.0 * v_dot_h, 1.0e-6);
}

vec3 sample_clearcoat_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent) {
    Material clearcoatMaterial = material;
    clearcoatMaterial.roughness = clamp(material.clearcoat_roughness, MATERIAL_MIN_GGX_ROUGHNESS, 1.0);
    clearcoatMaterial.anisotropy_strength = 0.0;
    return sample_ggx_brdf(state, clearcoatMaterial, wo, material_clearcoat_normal(material, n), tangent, bitangent);
}

float pdf_sheen_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v <= 1.0e-6 || n_dot_l <= 1.0e-6 || luminance(material.sheen_color) <= 1.0e-6) {
        return 0.0;
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1.0e-12) {
        return 0.0;
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(wo, h), 1.0e-6);
    return charlie_ndf(material.sheen_roughness, n_dot_h) * n_dot_h / max(4.0 * v_dot_h, 1.0e-6);
}

vec3 sample_sheen_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent) {
    tangent_frame(n, tangent, bitangent);
    float invAlpha = charlie_inv_alpha(material.sheen_roughness);
    float u1 = rand_f32(state);
    float u2 = rand_f32(state);
    float sinTheta = pow(u1, 1.0 / (invAlpha + 2.0));
    float cosTheta = sqrt(max(1.0 - sinTheta * sinTheta, 0.0));
    float phi = 2.0 * PI * u2;
    vec3 h = normalize(from_tangent_space(vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta), tangent, bitangent, n));
    if (dot(wo, h) <= 0.0) {
        h = reflect(h, n);
    }
    return normalize(2.0 * dot(wo, h) * h - wo);
}

vec3 thin_film_tint(Material material, float cosTheta) {
    float factor = clamp(material.iridescence_factor, 0.0, 1.0);
    if (factor <= 1.0e-5) {
        return vec3(1.0);
    }
    float eta = max(material.iridescence_ior, 1.01);
    float thicknessNm = clamp(material.iridescence_thickness_min, 0.0, max(material.iridescence_thickness_max, material.iridescence_thickness_min));
    float sin2T = max(1.0 - cosTheta * cosTheta, 0.0) / (eta * eta);
    float cosT = sqrt(max(1.0 - sin2T, 0.0));
    float opticalPathNm = 2.0 * eta * thicknessNm * cosT;
    vec3 phase = 2.0 * PI * opticalPathNm / vec3(650.0, 510.0, 475.0);
    vec3 tint = 0.55 + 0.45 * cos(phase);
    float tintLum = max(luminance(tint), 1.0e-4);
    tint = clamp(tint / tintLum, vec3(0.25), vec3(1.75));
    return mix(vec3(1.0), tint, factor);
}

vec3 eval_ggx_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    Material specMaterial = material;
    specMaterial.roughness = material_specular_roughness(material);
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v < 1e-6 || n_dot_l < 1e-6) {
        return vec3(0.0);
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1e-12) {
        return vec3(0.0);
    }
    vec3 h = normalize(halfVector);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(wo, h), 0.0);
    vec3 f0 = pbr_f0(specMaterial);
    vec3 f = specMaterial.use_conductor_optics != 0u
        ? conductor_fresnel(specMaterial, v_dot_h)
        : schlick_fresnel(f0, v_dot_h);
    f *= thin_film_tint(specMaterial, v_dot_h);
    float d = ggx_ndf(specMaterial.roughness, n_dot_h);
    float g = smith_g(specMaterial.roughness, n_dot_v, n_dot_l);
    if (material_uses_anisotropy(specMaterial)) {
        material_anisotropic_frame(specMaterial, n, tangent, bitangent);
        float alphaX;
        float alphaY;
        material_anisotropic_alpha(specMaterial, alphaX, alphaY);
        d = ggx_anisotropic_ndf(alphaX, alphaY, h, n, tangent, bitangent);
        g = smith_g_anisotropic(alphaX, alphaY, wo, wi, n, tangent, bitangent);
    }
    vec3 specular = f * d * g / max(4.0 * n_dot_v * n_dot_l, 1e-10);
    vec3 msCompensation = heitz_ms_ggx(f0, specMaterial.roughness, n_dot_v, n_dot_l);
    specular += msCompensation;
    vec3 diffuse = pbr_diffuse_energy(material) * eval_diffuse_brdf(material, wo, wi, n);
    return diffuse + specular;
}

float pdf_ggx_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    Material specMaterial = material;
    specMaterial.roughness = material_specular_roughness(material);
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v < 1e-6 || n_dot_l < 1e-6) {
        return 0.0;
    }
    vec3 halfVector = wo + wi;
    if (dot(halfVector, halfVector) < 1e-12) {
        return 0.0;
    }
    vec3 h = normalize(halfVector);
    return ggx_visible_normal_pdf(specMaterial, wo, h, n, tangent, bitangent);
}

float pdf_pbr_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    float NdotV = max(dot(n, wo), 0.0);
    float diffuseProbability;
    float specularProbability;
    float sheenProbability;
    float clearcoatProbability;
    pbr_lobe_probabilities(material, NdotV, diffuseProbability, specularProbability, sheenProbability, clearcoatProbability);
    return diffuseProbability * diffuse_pdf(n, wi) +
        specularProbability * pdf_ggx_brdf(material, wo, wi, n, tangent, bitangent) +
        sheenProbability * pdf_sheen_brdf(material, wo, wi, n) +
        clearcoatProbability * pdf_clearcoat_brdf(material, wo, wi, n);
}

vec3 sample_ggx_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent) {
    Material specMaterial = material;
    specMaterial.roughness = material_specular_roughness(material);
    float r = ggx_safe_roughness(specMaterial.roughness);
    float a = r * r;
    float r1 = rand_f32(state);
    float r2 = rand_f32(state);
    if (material_uses_anisotropy(specMaterial)) {
        material_anisotropic_frame(specMaterial, n, tangent, bitangent);
        float alphaX;
        float alphaY;
        material_anisotropic_alpha(specMaterial, alphaX, alphaY);
        vec3 vAniso = to_tangent_space(wo, tangent, bitangent, n);
        if (vAniso.z <= 0.0) {
            return reflect(-wo, n);
        }
        vec3 vhAniso = normalize(vec3(alphaX * vAniso.x, alphaY * vAniso.y, vAniso.z));
        float lensqAniso = vhAniso.x * vhAniso.x + vhAniso.y * vhAniso.y;
        vec3 t1Aniso = lensqAniso > 1.0e-8 ? vec3(-vhAniso.y, vhAniso.x, 0.0) * inversesqrt(lensqAniso) : vec3(1.0, 0.0, 0.0);
        vec3 t2Aniso = cross(vhAniso, t1Aniso);
        float radiusAniso = sqrt(r1);
        float phiAniso = 2.0 * PI * r2;
        float p1Aniso = radiusAniso * cos(phiAniso);
        float p2Aniso = radiusAniso * sin(phiAniso);
        float blendAniso = 0.5 * (1.0 + vhAniso.z);
        p2Aniso = mix(sqrt(max(0.0, 1.0 - p1Aniso * p1Aniso)), p2Aniso, blendAniso);
        vec3 nhAniso = p1Aniso * t1Aniso + p2Aniso * t2Aniso + sqrt(max(0.0, 1.0 - p1Aniso * p1Aniso - p2Aniso * p2Aniso)) * vhAniso;
        vec3 hLocalAniso = normalize(vec3(alphaX * nhAniso.x, alphaY * nhAniso.y, max(nhAniso.z, 0.0)));
        vec3 hAniso = normalize(from_tangent_space(hLocalAniso, tangent, bitangent, n));
        return 2.0 * dot(wo, hAniso) * hAniso - wo;
    }
    tangent_frame(n, tangent, bitangent);
    vec3 v = to_tangent_space(wo, tangent, bitangent, n);
    if (v.z <= 0.0) {
        return reflect(-wo, n);
    }

    vec3 vh = normalize(vec3(a * v.x, a * v.y, v.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lensq > 1.0e-8 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);

    float radius = sqrt(r1);
    float phi = 2.0 * PI * r2;
    float p1 = radius * cos(phi);
    float p2 = radius * sin(phi);
    float blend = 0.5 * (1.0 + vh.z);
    p2 = mix(sqrt(max(0.0, 1.0 - p1 * p1)), p2, blend);

    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    vec3 hLocal = normalize(vec3(a * nh.x, a * nh.y, max(nh.z, 0.0)));
    vec3 h = normalize(from_tangent_space(hLocal, tangent, bitangent, n));
    return 2.0 * dot(wo, h) * h - wo;
}

vec3 eval_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    MaterialClosure c = material_to_closure(material);
    vec3 result = vec3(0.0);

    float NdotL = max(dot(n, wi), 0.0);

    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_DIFFUSE)) {
        result += c.weight * eval_diffuse_brdf(material, wo, wi, n);
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SSS)) {
        float sssRadius = max(c.ior, 0.01);
        float wrap = NdotL * 0.5 + 0.5;
        result += c.weight * c.color * (sssRadius / PI) * wrap;
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SHEEN)) {
        result += c.weight * eval_sheen_brdf(material, wo, wi, n);
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SPECULAR)) {
        result += c.weight * eval_ggx_brdf(material, wo, wi, n, tangent, bitangent);
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_CLEARCOAT)) {
        result += c.weight * eval_clearcoat_brdf(material, wo, wi, n);
    }
    return result;
}

float pdf_brdf(Material material, vec3 wo, vec3 wi, vec3 n, vec3 tangent, vec3 bitangent) {
    if (material_is_delta(material)) {
        return 0.0;
    }
    MaterialClosure c = material_to_closure(material);
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SPECULAR)) {
        return pdf_pbr_brdf(material, wo, wi, n, tangent, bitangent);
    }
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_DIFFUSE) ||
        closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SSS) ||
        closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SHEEN)) {
        return diffuse_pdf(n, wi);
    }
    return 0.0;
}

vec3 sample_brdf(inout uint state, Material material, vec3 wo, vec3 n, vec3 tangent, vec3 bitangent, out float pdf) {
    MaterialClosure c = material_to_closure(material);
    if (closure_has_flag(c, MATERIAL_CLOSURE_FLAG_SPECULAR)) {
        float NdotV_sample = max(dot(n, wo), 0.0);
        float diffuseProbability;
        float specularProbability;
        float sheenProbability;
        float clearcoatProbability;
        pbr_lobe_probabilities(material, NdotV_sample, diffuseProbability, specularProbability, sheenProbability, clearcoatProbability);
        vec3 wi;
        float lobeSample = rand_f32(state);
        if (lobeSample < clearcoatProbability) {
            wi = sample_clearcoat_brdf(state, material, wo, n, tangent, bitangent);
        } else if (lobeSample < clearcoatProbability + specularProbability) {
            wi = sample_ggx_brdf(state, material, wo, n, tangent, bitangent);
        } else if (lobeSample < clearcoatProbability + specularProbability + sheenProbability) {
            wi = sample_sheen_brdf(state, material, wo, n, tangent, bitangent);
        } else {
            float diffusePdf;
            wi = sample_cosine_hemisphere(state, n, diffusePdf);
        }
        pdf = pdf_pbr_brdf(material, wo, wi, n, tangent, bitangent);
        return wi;
    }
    return sample_cosine_hemisphere(state, n, pdf);
}

// ---------------------------------------------------------------------------
// ReSTIR DI helper functions for raygen — mirrors restir_di_common.glsl
// but defined here to avoid include conflicts across ray tracing stages.
// ---------------------------------------------------------------------------
RestirDiReservoir restir_di_empty_reservoir() {
    RestirDiReservoir r;
    r.sampleMetadata = uvec4(0u);
    r.reservoirMetadata = uvec4(0u);
    r.samplePosition_distance = vec4(0.0);
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleDirection_pdf = vec4(0.0, 0.0, 1.0, 1.0e-6);
    r.sampleRadiance_target = vec4(0.0);
    r.sampleNormal_weightSum = vec4(0.0, 1.0, 0.0, 0.0);
    r.contribution_confidence = vec4(0.0);
#else
    r.sampleMetadata.w = restir_di_pack_radiance(vec3(0.0));
    r.reservoirMetadata.z = packSnorm2x16(vec2(0.0));
    r.reservoirMetadata.w = packHalf2x16(vec2(0.0));
#endif
    r.reservoirMetadata.y = packHalf2x16(vec2(1.0e-6, 0.0));
    return r;
}

float restir_di_target_function(vec3 radiance) {
    return max(dot(radiance, vec3(0.2126, 0.7152, 0.0722)), 1.0e-6);
}

void restir_di_set_valid(inout RestirDiReservoir r, bool valid) {
    if (valid) r.reservoirMetadata.x |= (1u << 18u);
    else r.reservoirMetadata.x &= ~(1u << 18u);
}
void restir_di_set_age(inout RestirDiReservoir r, uint age) {
    r.reservoirMetadata.x = (r.reservoirMetadata.x & ~0xffu) | min(age, 255u);
}
void restir_di_set_m(inout RestirDiReservoir r, uint m) {
    r.reservoirMetadata.x = (r.reservoirMetadata.x & ~0xff00u) | (min(m, 255u) << 8u);
}
void restir_di_set_visibility(inout RestirDiReservoir r, uint vis) {
    r.reservoirMetadata.x = (r.reservoirMetadata.x & ~0x30000u) | ((vis & 3u) << 16u);
}
void restir_di_set_source_pdf(inout RestirDiReservoir r, float pdf) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleDirection_pdf.w = max(pdf, 1.0e-6);
#endif
    float prevWeight = unpackHalf2x16(r.reservoirMetadata.y).y;
    r.reservoirMetadata.y = packHalf2x16(vec2(clamp(pdf, 1.0e-6, 65504.0), prevWeight));
}
void restir_di_set_previous_weight(inout RestirDiReservoir r, float w) {
    float pdf = unpackHalf2x16(r.reservoirMetadata.y).x;
    r.reservoirMetadata.y = packHalf2x16(vec2(pdf, clamp(w, 0.0, 1.0)));
}
bool restir_di_reservoir_valid(RestirDiReservoir r) {
    bool validBit = (r.reservoirMetadata.x & (1u << 18u)) != 0u;
#if RTV_RESTIR_DI_VALIDATION_FULL
    float target = r.sampleRadiance_target.w;
    float weight = r.sampleNormal_weightSum.w;
    float pdf = r.sampleDirection_pdf.w;
    vec3 radiance = r.sampleRadiance_target.xyz;
#else
    vec2 targetWeight = unpackHalf2x16(r.reservoirMetadata.w);
    float target = targetWeight.x;
    float weight = targetWeight.y;
    float pdf = unpackHalf2x16(r.reservoirMetadata.y).x;
    vec3 radiance = restir_di_unpack_radiance(r.sampleMetadata.w);
#endif
    return validBit && ((r.reservoirMetadata.x >> 8u) & 0xffu) > 0u &&
        target > 0.0 && weight > 0.0 && pdf > 0.0 &&
        !isnan(target) && !isinf(target) && !isnan(weight) && !isinf(weight) &&
        !isnan(pdf) && !isinf(pdf) &&
        !any(isnan(r.samplePosition_distance.xyz)) && !any(isinf(r.samplePosition_distance.xyz)) &&
        !any(isnan(radiance)) && !any(isinf(radiance));
}
uint restir_di_light_kind(RestirDiReservoir r) { return r.sampleMetadata.z & 0xffu; }
uint restir_di_light_id(RestirDiReservoir r) { return r.sampleMetadata.x; }
uint restir_di_light_index(RestirDiReservoir r) { return r.sampleMetadata.z >> 8u; }
uint restir_di_light_version(RestirDiReservoir r) { return r.sampleMetadata.y; }
float restir_di_target(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleRadiance_target.w, 0.0);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.w).x, 0.0);
#endif
}

uint restir_di_identity_hash(uvec2 identity) {
    uint h = identity.x ^ (identity.y + 0x9e3779b9u + (identity.x << 6u) + (identity.x >> 2u));
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    return h ^ (h >> 16u);
}
float restir_di_source_pdf(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleDirection_pdf.w, 1.0e-6);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.y).x, 1.0e-6);
#endif
}
float restir_di_weight_sum(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleNormal_weightSum.w, 0.0);
#else
    return max(unpackHalf2x16(r.reservoirMetadata.w).y, 0.0);
#endif
}
void restir_di_set_weight_sum(inout RestirDiReservoir r, float ws) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleNormal_weightSum.w = max(ws, 0.0);
#else
    vec2 targetWeight = unpackHalf2x16(r.reservoirMetadata.w);
    r.reservoirMetadata.w = packHalf2x16(vec2(targetWeight.x, clamp(ws, 0.0, 65504.0)));
#endif
}
void restir_di_set_target(inout RestirDiReservoir r, float target) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleRadiance_target.w = max(target, 0.0);
#else
    vec2 targetWeight = unpackHalf2x16(r.reservoirMetadata.w);
    r.reservoirMetadata.w = packHalf2x16(vec2(clamp(target, 0.0, 65504.0), targetWeight.y));
#endif
}
void restir_di_set_sample_radiance(inout RestirDiReservoir r, vec3 radiance) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleRadiance_target.rgb = max(radiance, vec3(0.0));
#else
    r.sampleMetadata.w = restir_di_pack_radiance(radiance);
#endif
}
void restir_di_set_confidence(inout RestirDiReservoir r, float confidence) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.contribution_confidence.w = clamp(confidence, 0.0, 1.0);
#else
    uint packedConfidence = uint(round(clamp(confidence, 0.0, 1.0) * 31.0));
    r.reservoirMetadata.x = (r.reservoirMetadata.x & 0x07ffffffu) |
        (packedConfidence << 27u);
#endif
}
vec3 restir_di_sample_radiance(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return max(r.sampleRadiance_target.rgb, vec3(0.0));
#else
    return max(restir_di_unpack_radiance(r.sampleMetadata.w), vec3(0.0));
#endif
}
void restir_di_set_direction(inout RestirDiReservoir r, vec3 direction) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleDirection_pdf.xyz = normalize(direction);
#endif
}
vec2 restir_di_oct_encode(vec3 value) {
    vec3 n = normalize(value);
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
}
void restir_di_set_light_normal(inout RestirDiReservoir r, vec3 normal) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    r.sampleNormal_weightSum.xyz = normalize(normal);
#else
    r.reservoirMetadata.z = packSnorm2x16(restir_di_oct_encode(normal));
#endif
}
float restir_di_previous_weight(RestirDiReservoir r) { return unpackHalf2x16(r.reservoirMetadata.y).y; }
uint restir_di_m(RestirDiReservoir r) { return (r.reservoirMetadata.x >> 8u) & 0xffu; }
uint restir_di_visibility(RestirDiReservoir r) { return (r.reservoirMetadata.x >> 16u) & 3u; }
uint restir_di_rejection_flags(RestirDiReservoir r) {
#if RTV_RESTIR_DI_VALIDATION_FULL
    return r.reservoirMetadata.w;
#else
    return (r.reservoirMetadata.x >> 19u) & 0xffu;
#endif
}
