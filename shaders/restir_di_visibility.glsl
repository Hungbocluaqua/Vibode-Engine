#ifndef RTV_RESTIR_DI_VISIBILITY_GLSL
#define RTV_RESTIR_DI_VISIBILITY_GLSL

// ReSTIR DI uniforms and optional alpha-aware ray-query visibility helpers.
struct RestirDiUniforms {
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
    uint mode;
    uint spatialResultValid;
    uint visibilityRayBudget;
    uint historyValid;
    uint materialVisibilityFlags;
    uint counterEnabled;
    uint padding1;
    uint padding2;
};

const float RESTIR_DI_VISIBILITY_EPSILON = 0.001;

#if defined(RTV_RESTIR_DI_ENABLE_RAY_QUERY) && RTV_RESTIR_DI_ENABLE_RAY_QUERY
// Ray query visibility check. The caller must enable GL_EXT_ray_query and
// declare topLevelAS before calling this helper.
#ifndef RTV_DI_RAY_MASK_SHADOW
#define RTV_DI_RAY_MASK_SHADOW 0x02u
#endif

const uint RESTIR_DI_MATERIAL_STRIDE = 52u;
#define RESTIR_DI_MATERIAL_TEXTURE_LIMIT restir_di_scene.bindlessTextureCapacity
const uint RESTIR_DI_ALPHA_MODE_OPAQUE = 0u;
const uint RESTIR_DI_ALPHA_MODE_MASK = 1u;
const uint RESTIR_DI_ALPHA_MODE_BLEND = 2u;
const uint RESTIR_DI_TEXTURE_TRANSFORM_BASE_COLOR = 0u;
const uint RESTIR_DI_TEXTURE_TRANSFORM_TRANSMISSION = 12u;
const uint RESTIR_DI_TEXTURE_TRANSFORM_COUNT = 17u;
const uint RESTIR_DI_TEXTURE_TRANSFORM_BASE = 18u;
const uint RESTIR_DI_MATERIAL_VISIBILITY_TRUST_OMM = 1u << 0u;

struct RestirDiInstanceRecord {
    mat4 transform;
    mat4 inverse_transform;
    mat4 normal_transform;
    mat4 prev_transform;
    uvec4 metadata;
};

struct RestirDiMeshRecord {
    uvec4 vertex_index_data;
    uvec4 primitive_data;
    uvec4 bvh_data;
    uvec4 flags;
};

struct RestirDiLocalVertex {
    vec4 position_uv_x;
    vec4 normal_uv_y;
    vec4 tangent;
    vec4 color;
    vec4 texcoord1;
};

struct RestirDiMaterialVisibility {
    uint dataIndex;
    vec3 color;
    float alphaFactor;
    int baseColorTexture;
    float alphaCutoff;
    uint alphaMode;
    uint doubleSided;
    float transmissionFactor;
    int transmissionTexture;
    int opacityTexture;
};

layout(set = 0, binding = 15, std430) readonly buffer RestirDiMeshMaterials {
    vec4 restir_di_mesh_materials[];
};
layout(set = 0, binding = 16, std430) readonly buffer RestirDiTriangleMaterialIds {
    uint restir_di_triangle_material_ids[];
};
layout(set = 0, binding = 17, std430) readonly buffer RestirDiInstanceRecords {
    RestirDiInstanceRecord restir_di_instance_records[];
};
layout(set = 0, binding = 18, std430) readonly buffer RestirDiMeshRecords {
    RestirDiMeshRecord restir_di_mesh_records[];
};
layout(set = 0, binding = 19, std430) readonly buffer RestirDiLocalVertices {
    RestirDiLocalVertex restir_di_local_vertices[];
};
layout(set = 0, binding = 20, std430) readonly buffer RestirDiLocalIndices {
    uint restir_di_local_indices[];
};
layout(set = 0, binding = 21, std430) readonly buffer RestirDiGeometryTriangleOffsets {
    uint restir_di_geometry_triangle_offsets[];
};
layout(set = 0, binding = 22, std430) readonly buffer RestirDiMeshGeometryRanges {
    uvec2 restir_di_mesh_geometry_ranges[];
};
layout(set = 0, binding = 23, std430) readonly buffer RestirDiTlasGeometryRanges {
    uvec4 restir_di_tlas_geometry_ranges[];
};
layout(set = 2, binding = 0) uniform sampler2D restir_di_material_textures[];

RestirDiMaterialVisibility restir_di_decode_material_visibility(uint materialIndex) {
    uint materialCount = max(restir_di_scene.materialCount, 1u);
    uint idx = min(materialIndex, materialCount - 1u) * RESTIR_DI_MATERIAL_STRIDE;
    vec4 d0 = restir_di_mesh_materials[idx + 0u];
    vec4 d2 = restir_di_mesh_materials[idx + 2u];
    vec4 d3 = restir_di_mesh_materials[idx + 3u];
    vec4 d4 = restir_di_mesh_materials[idx + 4u];
    vec4 d12 = restir_di_mesh_materials[idx + 12u];
    vec4 d17 = restir_di_mesh_materials[idx + 17u];
    RestirDiMaterialVisibility material;
    material.dataIndex = idx;
    material.color = d0.xyz;
    material.alphaFactor = clamp(d2.w, 0.0, 1.0);
    material.baseColorTexture = int(round(d3.x));
    material.alphaCutoff = d4.x;
    material.alphaMode = uint(round(d4.y));
    material.doubleSided = uint(round(d4.z));
    material.transmissionFactor = clamp(d12.y, 0.0, 1.0);
    material.transmissionTexture = int(round(d12.z));
    material.opacityTexture = int(round(d17.x));
    return material;
}

uint restir_di_scene_instance_index_from_tlas_record(uint tlasRecordIndex) {
    if (tlasRecordIndex < restir_di_tlas_geometry_ranges.length()) {
        uvec4 tlasRecord = restir_di_tlas_geometry_ranges[tlasRecordIndex];
        if (tlasRecord.y != 0u && tlasRecord.z != 0xffffffffu) {
            return tlasRecord.z;
        }
    }
    return tlasRecordIndex;
}

uint restir_di_geometry_triangle_offset(uint meshIndex, uint tlasRecordIndex, uint geometryIndex, uint meshFirstIndex) {
    if (tlasRecordIndex < restir_di_tlas_geometry_ranges.length()) {
        uvec4 tlasRange = restir_di_tlas_geometry_ranges[tlasRecordIndex];
        if (tlasRange.y != 0u) {
            return restir_di_geometry_triangle_offsets[tlasRange.x + min(geometryIndex, tlasRange.y - 1u)];
        }
    }
    if (meshIndex >= restir_di_scene.meshCount) {
        return meshFirstIndex / 3u;
    }
    uvec2 range = restir_di_mesh_geometry_ranges[meshIndex];
    if (range.y == 0u) {
        return meshFirstIndex / 3u;
    }
    return restir_di_geometry_triangle_offsets[range.x + min(geometryIndex, range.y - 1u)];
}

uint restir_di_material_for_triangle_index(uint triangleIndex) {
    uint triangleCount = restir_di_scene.localIndexCount / 3u;
    if (triangleIndex >= triangleCount) {
        return 0u;
    }
    return restir_di_triangle_material_ids[triangleIndex];
}

vec2 restir_di_material_texture_uv(RestirDiMaterialVisibility material, uint slot, vec2 uv0, vec2 uv1) {
    if (slot >= RESTIR_DI_TEXTURE_TRANSFORM_COUNT) {
        return uv0;
    }
    uint transformBase = material.dataIndex + RESTIR_DI_TEXTURE_TRANSFORM_BASE + slot * 2u;
    vec4 transform1 = restir_di_mesh_materials[transformBase + 1u];
    return uint(round(transform1.z)) == 1u ? uv1 : uv0;
}

vec2 restir_di_apply_material_texture_transform(RestirDiMaterialVisibility material, uint slot, vec2 uv0, vec2 uv1) {
    vec2 uv = restir_di_material_texture_uv(material, slot, uv0, uv1);
    if (slot >= RESTIR_DI_TEXTURE_TRANSFORM_COUNT) {
        return uv;
    }
    uint transformBase = material.dataIndex + RESTIR_DI_TEXTURE_TRANSFORM_BASE + slot * 2u;
    vec4 transform0 = restir_di_mesh_materials[transformBase + 0u];
    vec4 transform1 = restir_di_mesh_materials[transformBase + 1u];
    if (uint(round(transform1.y)) == 0u) {
        return uv;
    }
    vec2 scaled = uv * transform0.zw;
    float c = cos(transform1.x);
    float s = sin(transform1.x);
    vec2 rotated = vec2(c * scaled.x - s * scaled.y, s * scaled.x + c * scaled.y);
    return transform0.xy + rotated;
}

void restir_di_apply_visibility_textures(inout RestirDiMaterialVisibility material, vec2 uv0, vec2 uv1) {
    if (material.baseColorTexture >= 0 && material.baseColorTexture < int(RESTIR_DI_MATERIAL_TEXTURE_LIMIT)) {
        int textureIndex = material.baseColorTexture;
        vec2 sampleUv = restir_di_apply_material_texture_transform(
            material, RESTIR_DI_TEXTURE_TRANSFORM_BASE_COLOR, uv0, uv1);
        vec4 base = texture(restir_di_material_textures[nonuniformEXT(textureIndex)], sampleUv);
        material.color *= max(base.rgb, vec3(0.0));
        material.alphaFactor *= clamp(base.a, 0.0, 1.0);
    }
    if (material.transmissionTexture >= 0 && material.transmissionTexture < int(RESTIR_DI_MATERIAL_TEXTURE_LIMIT)) {
        int textureIndex = material.transmissionTexture;
        vec2 sampleUv = restir_di_apply_material_texture_transform(
            material, RESTIR_DI_TEXTURE_TRANSFORM_TRANSMISSION, uv0, uv1);
        material.transmissionFactor *= clamp(
            texture(restir_di_material_textures[nonuniformEXT(textureIndex)], sampleUv).r,
            0.0,
            1.0);
    }
    if (material.opacityTexture >= 0 && material.opacityTexture < int(RESTIR_DI_MATERIAL_TEXTURE_LIMIT)) {
        material.alphaFactor *= clamp(
            texture(restir_di_material_textures[nonuniformEXT(material.opacityTexture)], uv0).r,
            0.0,
            1.0);
    }
}

bool restir_di_accept_material_alpha(RestirDiMaterialVisibility material) {
    if (material.alphaMode == RESTIR_DI_ALPHA_MODE_MASK) {
        return material.alphaFactor >= material.alphaCutoff;
    }
    if (material.alphaMode == RESTIR_DI_ALPHA_MODE_BLEND) {
        return material.alphaFactor >= 0.10;
    }
    return true;
}

float restir_di_material_effective_transmission(RestirDiMaterialVisibility material) {
    return clamp(material.transmissionFactor, 0.0, 1.0);
}

bool restir_di_candidate_visibility_blocks(rayQueryEXT rayQuery, vec3 rayDirection, uint materialVisibilityFlags) {
    uint tlasRecordIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
    uint instanceIndex = restir_di_scene_instance_index_from_tlas_record(tlasRecordIndex);
    if (instanceIndex >= restir_di_scene.instanceCount) {
        return false;
    }
    RestirDiInstanceRecord instance = restir_di_instance_records[instanceIndex];
    uint meshIndex = instance.metadata.x;
    if (meshIndex >= restir_di_scene.meshCount) {
        return true;
    }
    RestirDiMeshRecord mesh = restir_di_mesh_records[meshIndex];
    uint firstIndex = mesh.vertex_index_data.z;
    uint geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
    uint primitiveIndex = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
    uint globalTriangleIndex = restir_di_geometry_triangle_offset(meshIndex, tlasRecordIndex, geometryIndex, firstIndex) + primitiveIndex;
    uint triIndex = globalTriangleIndex * 3u;
    if (triIndex + 2u >= restir_di_scene.localIndexCount) {
        return true;
    }
    uint i0 = restir_di_local_indices[triIndex + 0u];
    uint i1 = restir_di_local_indices[triIndex + 1u];
    uint i2 = restir_di_local_indices[triIndex + 2u];
    if (i0 >= restir_di_scene.localVertexCount ||
        i1 >= restir_di_scene.localVertexCount ||
        i2 >= restir_di_scene.localVertexCount) {
        return true;
    }

    RestirDiLocalVertex v0 = restir_di_local_vertices[i0];
    RestirDiLocalVertex v1 = restir_di_local_vertices[i1];
    RestirDiLocalVertex v2 = restir_di_local_vertices[i2];
    vec3 p0 = v0.position_uv_x.xyz;
    vec3 p1 = v1.position_uv_x.xyz;
    vec3 p2 = v2.position_uv_x.xyz;
    vec3 localGeomNormal = normalize(cross(p1 - p0, p2 - p0));
    vec3 worldGeomNormal = normalize(mat3(instance.normal_transform) * localGeomNormal);
    bool frontFace = dot(worldGeomNormal, rayDirection) < 0.0;

    uint materialIndex = restir_di_material_for_triangle_index(globalTriangleIndex);
    RestirDiMaterialVisibility material = restir_di_decode_material_visibility(materialIndex);
    if (!frontFace && material.doubleSided == 0u) {
        return false;
    }

    bool trustHardwareOpacity = (materialVisibilityFlags & RESTIR_DI_MATERIAL_VISIBILITY_TRUST_OMM) != 0u &&
        material.alphaMode == RESTIR_DI_ALPHA_MODE_MASK;
    if (material.alphaMode == RESTIR_DI_ALPHA_MODE_OPAQUE || trustHardwareOpacity) {
        return restir_di_material_effective_transmission(material) <= 1.0e-5;
    }

    vec2 bary2 = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
    vec3 bary = vec3(1.0 - bary2.x - bary2.y, bary2.x, bary2.y);
    vec2 uv0 = vec2(
        v0.position_uv_x.w * bary.x + v1.position_uv_x.w * bary.y + v2.position_uv_x.w * bary.z,
        v0.normal_uv_y.w * bary.x + v1.normal_uv_y.w * bary.y + v2.normal_uv_y.w * bary.z);
    vec2 uv1 = v0.texcoord1.xy * bary.x + v1.texcoord1.xy * bary.y + v2.texcoord1.xy * bary.z;
    restir_di_apply_visibility_textures(material, uv0, uv1);
    material.alphaFactor *= clamp((v0.color * bary.x + v1.color * bary.y + v2.color * bary.z).a, 0.0, 1.0);

    if (!restir_di_accept_material_alpha(material)) {
        return false;
    }
    return restir_di_material_effective_transmission(material) <= 1.0e-5;
}

uint restir_di_ray_query_visible(vec3 origin, vec3 direction, float tmax, uint materialVisibilityFlags) {
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery, topLevelAS, gl_RayFlagsCullBackFacingTrianglesEXT, RTV_DI_RAY_MASK_SHADOW,
                           origin, 0.0, direction, tmax);
    uint candidateCount = 0u;
    while (rayQueryProceedEXT(rayQuery)) {
        if (rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            ++candidateCount;
            if (candidateCount > 8u ||
                restir_di_candidate_visibility_blocks(rayQuery, direction, materialVisibilityFlags)) {
                rayQueryConfirmIntersectionEXT(rayQuery);
                break;
            }
        }
    }
    bool hit = rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT;
    return hit ? RESTIR_DI_VISIBILITY_OCCLUDED : RESTIR_DI_VISIBILITY_VISIBLE;
}
#endif


#endif // RTV_RESTIR_DI_VISIBILITY_GLSL
