#ifndef RTV_RT_SCENE_LIGHTING_GLSL
#define RTV_RT_SCENE_LIGHTING_GLSL

// Scene-light BVH and authored/emissive light sampling helpers.
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


#endif // RTV_RT_SCENE_LIGHTING_GLSL
