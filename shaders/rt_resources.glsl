#ifndef RTV_RT_RESOURCES_GLSL
#define RTV_RT_RESOURCES_GLSL

// Renderer-wide descriptors and scene resource records. Descriptor bindings are ABI-stable.
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
    uvec4 path_trace_controls;   // x = requested SPP, y = limit to 1 SPP, z bits: 0 RT counters, 1 DI estimator, 2 HW backface culling, 3 final-bounce fast path, 4 Native2B kernel, 5-6 blended decal shadow mode, 7-8 Native2B direct reuse mode, 9 force opaque camera rays, 10 compact imported emissive direct, 11 adaptive sample-count buffer
    vec4 dof_controls;
    vec4 motion_blur_controls;
    vec4 volume_controls;
    vec4 projection_controls;
    vec4 clip_controls;
    uvec4 gi_version_controls;   // Phase 8: x = light, y = material, z = object, w = environment version
    uvec4 restir_di_controls;     // x = new ReSTIR DI raygen writes enabled, y = secondary direct lighting enabled, z = include sun, w = include environment
    vec4 native2b_controls;       // x = terminal direct sample probability
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
    uint bindless_texture_capacity;
    uint transmissive_shadow_caster_count;
    uint mesh_params_reserved0;
    uint mesh_params_reserved1;
    uint mesh_params_reserved2;
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

layout(set = 2, binding = 0) uniform sampler2D material_textures[];

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
layout(set = 0, binding = 62, std430) readonly buffer RtTlasGeometryRanges {
    uvec4 rt_tlas_geometry_ranges[];
};
layout(set = 0, binding = 48, std430) buffer RtDiagnosticCounters {
    uint rt_diagnostic_counters[];
};
layout(set = 0, binding = 63, std430) buffer RtAlphaMaterialCounters {
    uint rt_alpha_material_counters[];
};
layout(set = 0, binding = 64, std430) readonly buffer AdaptiveSamplingDensityBuffer {
    float adaptive_sampling_density[];
};
layout(set = 0, binding = 65, std430) readonly buffer AdaptiveSamplingSampleCountBuffer {
    uint adaptive_sampling_sample_count[];
};
layout(set = 0, binding = 66, std140) uniform ReGIRParamsBlock {
    uvec4 grid_dimensions_reservoirs;
    uvec4 controls;
    vec4 grid_padding;
    vec4 query_controls;
    uvec4 environment_controls;
} regir_params;
struct ReGIRReservoir {
    uvec4 metadata;
    vec4 sample_position_weight;
};
layout(set = 0, binding = 67, std430) readonly buffer ReGIRReservoirBuffer {
    ReGIRReservoir regir_reservoirs[];
};
layout(set = 0, binding = 68, std430) readonly buffer ReGIRInputReservoirBuffer {
    ReGIRReservoir regir_input_reservoirs[];
};
layout(set = 0, binding = 69, std430) buffer ReGIRActiveCellsBuffer {
    uint regir_active_cells[];
};
layout(set = 0, binding = 70, std430) readonly buffer ReGIRHashCurrentCellsBuffer {
    uint regir_hash_current_cells[];
};
struct ReGIREnvironmentReservoir {
    uvec4 metadata;
    vec4 direction_pdf;
    vec4 reservoir_state; // selected target, weight sum, average weight, M
};
layout(set = 0, binding = 71, std430) readonly buffer ReGIREnvironmentReservoirBuffer {
    ReGIREnvironmentReservoir regir_environment_reservoirs[];
};
layout(set = 0, binding = 57, std430) readonly buffer StreamingResetInstanceMasks {
    uint streaming_reset_instance_masks[];
};

#ifndef RTV_RT_DIAGNOSTIC_COUNTERS
#define RTV_RT_DIAGNOSTIC_COUNTERS 0
#endif

#ifndef RTV_NATIVE2B_COMPACT_PRIMARY_LIGHTS
#define RTV_NATIVE2B_COMPACT_PRIMARY_LIGHTS 0
#endif

#ifndef RTV_REGIR_TRACE_ENABLED
#define RTV_REGIR_TRACE_ENABLED 1
#endif

bool streaming_instance_reset_mask(uint instanceId, uint flag) {
    return instanceId != 0xffffffffu &&
        instanceId < streaming_reset_instance_masks.length() &&
        (streaming_reset_instance_masks[instanceId] & flag) != 0u;
}

bool regir_enabled() {
#if RTV_REGIR_TRACE_ENABLED
    return (regir_params.controls.x & 1u) != 0u &&
        regir_params.grid_dimensions_reservoirs.x > 0u &&
        regir_params.grid_dimensions_reservoirs.y > 0u &&
        regir_params.grid_dimensions_reservoirs.z > 0u &&
        regir_params.grid_dimensions_reservoirs.w > 0u;
#else
    return false;
#endif
}

uint regir_grid_mode() {
    return (regir_params.controls.x >> 1u) & 3u;
}

bool regir_active_grid_enabled() {
    return regir_enabled() && regir_grid_mode() == 1u;
}

bool regir_hash_grid_enabled() {
    return regir_enabled() && regir_grid_mode() == 2u;
}

#ifndef RTV_REGIR_FINITE_LIGHT_TRACE_ENABLED
#define RTV_REGIR_FINITE_LIGHT_TRACE_ENABLED RTV_REGIR_TRACE_ENABLED
#endif
#ifndef RTV_REGIR_FRAME_COHERENT_FINITE_QUERY
#define RTV_REGIR_FRAME_COHERENT_FINITE_QUERY 0
#endif

bool regir_finite_light_enabled() {
#if RTV_REGIR_FINITE_LIGHT_TRACE_ENABLED
    return regir_enabled();
#else
    return false;
#endif
}

bool regir_infinite_light_enabled() {
    return regir_enabled() &&
        (regir_params.controls.x & 8u) != 0u &&
        regir_environment_reservoirs.length() > 1u;
}

bool regir_environment_enabled() {
    return regir_infinite_light_enabled() && regir_params.environment_controls.z > 0u;
}

bool regir_sun_enabled() {
    return regir_infinite_light_enabled() &&
        regir_params.environment_controls.w > 0u &&
        camera.sunlight_enabled != 0u;
}

bool regir_visibility_reuse_enabled() {
    return regir_enabled() && (regir_params.controls.x & 16u) != 0u;
}

float regir_canonical_mix() {
    return clamp(regir_params.query_controls.x, 0.0, 1.0);
}

bool regir_stochastic_query_enabled() {
    return regir_params.query_controls.y > 0.5;
}

float regir_finite_query_probability() {
#if RTV_REGIR_FRAME_COHERENT_FINITE_QUERY
    return 1.0;
#else
    if (!regir_stochastic_query_enabled()) {
        return 1.0;
    }
    return regir_hash_grid_enabled() ? (1.0 / 256.0) : 0.125;
#endif
}

bool regir_spatial_reuse_enabled() {
    return regir_params.query_controls.w > 0.5;
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
const uint RT_DIAG_PRIMARY_SURFACE_TRACE_RAYS = 14u;
const uint RT_DIAG_TERMINAL_SURFACE_TRACE_RAYS = 15u;
const uint RT_DIAG_SHADOW_SURFACE_TRACE_RAYS = 16u;
const uint RT_DIAG_ENV_DIRECT_SHADOW_RAYS = 17u;
const uint RT_DIAG_SUN_DIRECT_SHADOW_RAYS = 18u;
const uint RT_DIAG_EMISSIVE_DIRECT_SHADOW_RAYS = 19u;
const uint RT_DIAG_TRANSMISSIVE_SHADOW_SURFACE_TRACES = 20u;
const uint RT_DIAG_FAST_SHADOW_TRANSMITTANCE_USED = 21u;
const uint RT_DIAG_FULL_SHADOW_TRANSMITTANCE_USED = 22u;
const uint RT_DIAG_TERMINAL_FAST_DIRECT_USED = 23u;
const uint RT_DIAG_TERMINAL_GENERIC_DIRECT_USED = 24u;
const uint RT_DIAG_TERMINAL_MATERIAL_FULL_DECODE = 25u;
const uint RT_DIAG_TERMINAL_MATERIAL_HEADER_ONLY = 26u;
const uint RT_DIAG_PRIMARY_ANY_HIT_OPAQUE = 27u;
const uint RT_DIAG_PRIMARY_ANY_HIT_ALPHA_TESTED = 28u;
const uint RT_DIAG_PRIMARY_ANY_HIT_BLENDED = 29u;
const uint RT_DIAG_TERMINAL_ANY_HIT_INVOCATIONS = 30u;
const uint RT_DIAG_TERMINAL_ANY_HIT_OPAQUE = 31u;
const uint RT_DIAG_TERMINAL_ANY_HIT_ALPHA_TESTED = 32u;
const uint RT_DIAG_TERMINAL_ANY_HIT_BLENDED = 33u;
const uint RT_DIAG_CLOSEST_HIT_PRIMARY = 34u;
const uint RT_DIAG_CLOSEST_HIT_TERMINAL = 35u;
const uint RT_DIAG_CAUSTIC_BLOCKER_OPAQUE = 36u;
const uint RT_DIAG_CAUSTIC_BLOCKER_ALPHA_TESTED = 37u;
const uint RT_DIAG_CAUSTIC_BLOCKER_BLENDED = 38u;
const uint RT_DIAG_TERMINAL_FAST_DIRECT_FLAG_DISABLED = 39u;
const uint RT_DIAG_TERMINAL_FAST_DIRECT_SCENE_LIGHTS = 40u;
const uint RT_DIAG_TERMINAL_FAST_DIRECT_TRANSMISSIVE_SCENE = 41u;
const uint RT_DIAG_TERMINAL_FAST_DIRECT_VOLUME = 42u;
const uint RT_DIAG_TERMINAL_FAST_DIRECT_DEBUG = 43u;
const uint RT_DIAG_TERMINAL_FAST_DIRECT_MATERIAL_TRANSMISSIVE = 44u;
const uint RT_DIAG_TERMINAL_DIRECT_SKIPPED_EMISSIVE_OR_UNLIT = 45u;

const uint BLENDED_DECAL_SHADOW_MODE_EXACT = 0u;
const uint BLENDED_DECAL_SHADOW_MODE_OPAQUE_SHADOW = 1u;
const uint BLENDED_DECAL_SHADOW_MODE_ALPHA_CUTOUT_PROXY = 2u;

const uint NATIVE2B_DIRECT_REUSE_OFF = 0u;
const uint NATIVE2B_DIRECT_REUSE_RIS = 1u;
const uint NATIVE2B_DIRECT_REUSE_TEMPORAL = 2u;
const uint RT_ALPHA_MATERIAL_COUNTER_STRIDE = 4u;
const uint RT_ALPHA_MATERIAL_COUNTER_PRIMARY_ANY_HIT = 0u;
const uint RT_ALPHA_MATERIAL_COUNTER_TERMINAL_ANY_HIT = 1u;
const uint RT_ALPHA_MATERIAL_COUNTER_SHADOW_ANY_HIT = 2u;
const uint RT_ALPHA_MATERIAL_COUNTER_CLOSEST_HIT = 3u;

uint blended_decal_shadow_mode() {
    return (camera.path_trace_controls.z >> 5u) & 3u;
}

uint native2b_direct_reuse_mode() {
    return (camera.path_trace_controls.z >> 7u) & 3u;
}

bool force_opaque_camera_rays_enabled() {
    return (camera.path_trace_controls.z & 512u) != 0u;
}

bool force_opaque_terminal_rays_enabled() {
    return force_opaque_camera_rays_enabled() ||
        (((camera.path_trace_controls.z & 8u) != 0u) && ((camera.path_trace_controls.z & 16u) != 0u));
}

bool compact_imported_emissive_direct_enabled() {
    return (camera.path_trace_controls.z & 1024u) != 0u;
}

bool adaptive_sample_count_enabled() {
    return (camera.path_trace_controls.z & 2048u) != 0u;
}

bool compact_imported_emissive_terminal_direct_enabled() {
    return compact_imported_emissive_direct_enabled();
}

#if RTV_RT_DIAGNOSTIC_COUNTERS
void record_rt_counter(uint counterIndex) {
    if ((camera.path_trace_controls.z & 1u) != 0u) {
        atomicAdd(rt_diagnostic_counters[counterIndex], 1u);
    }
}

void record_rt_alpha_material_counter(uint materialIndex, uint counterClass) {
    if ((camera.path_trace_controls.z & 1u) == 0u) {
        return;
    }
    uint slot = materialIndex * RT_ALPHA_MATERIAL_COUNTER_STRIDE + counterClass;
    if (slot < rt_alpha_material_counters.length()) {
        atomicAdd(rt_alpha_material_counters[slot], 1u);
    }
}
#else
#define record_rt_counter(counterIndex)
#define record_rt_alpha_material_counter(materialIndex, counterClass)
#endif

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


#endif // RTV_RT_RESOURCES_GLSL
