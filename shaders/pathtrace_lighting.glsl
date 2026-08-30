#ifndef RTV_PATHTRACE_LIGHTING_GLSL
#define RTV_PATHTRACE_LIGHTING_GLSL

#ifndef RTV_GENERIC_DEEP_SECONDARY_DIRECT_PROB
#define RTV_GENERIC_DEEP_SECONDARY_DIRECT_PROB 1.0
#endif
#ifndef RTV_GENERIC_SECONDARY_DIRECT_PROB
#define RTV_GENERIC_SECONDARY_DIRECT_PROB 1.0
#endif
#ifndef RTV_GENERIC_SECONDARY_ONE_INFINITE_LIGHT
#define RTV_GENERIC_SECONDARY_ONE_INFINITE_LIGHT 0
#endif

// Direct-light sampling, visibility, ReSTIR reuse, and volume-light helpers.
bool sample_emissive_light(
    inout uint rng,
    vec3 hitPos,
    out vec3 wi,
    out vec3 emission,
    out float pdfSolidAngle,
    out float distanceToLight,
    out uint lightIndexOut,
    out uint lightKindOut,
    out vec3 samplePosition,
    out vec3 sampleRadiance,
    out vec3 lightNormalOut) {
    lightIndexOut = 0u;
    lightKindOut = 0u;
    samplePosition = vec3(0.0);
    sampleRadiance = vec3(0.0);
    lightNormalOut = vec3(0.0, 1.0, 0.0);
    float totalWeight = mesh_params.emissive_total_area;
    if (totalWeight <= 1e-8 || mesh_params.light_count == 0u) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    uint lightIndex;
    if (!sample_scene_light(rng, lightIndex)) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    LightRecord selected = light_records[lightIndex];
    lightIndexOut = lightIndex;
    lightKindOut = selected.metadata.x;
    float selectedPdf = light_record_selection_pdf(lightIndex);
    if (selected.metadata.x == 2u) {
        wi = normalize(selected.data1.xyz);
        emission = selected.data2.xyz;
        samplePosition = wi;
        sampleRadiance = emission;
        lightNormalOut = -wi;
        pdfSolidAngle = selectedPdf;
        distanceToLight = 10000.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 1u) {
        vec3 center = selected.data1.xyz;
        float radius = max(selected.data0.z, 1.0e-6);
        float z = rand_f32(rng) * 2.0 - 1.0;
        float phi = 2.0 * PI * rand_f32(rng);
        float radial = sqrt(max(1.0 - z * z, 0.0));
        vec3 lightNormal = vec3(radial * cos(phi), z, radial * sin(phi));
        vec3 samplePos = center + lightNormal * radius;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1.0e-8));
        wi = toLight / distanceToLight;
        float cosLight = max(dot(-wi, lightNormal), 0.0);
        samplePosition = samplePos;
        sampleRadiance = max(selected.data2.xyz, vec3(0.0));
        lightNormalOut = lightNormal;
        emission = sampleRadiance;
        float area = max(selected.data0.w, 1.0e-6);
        pdfSolidAngle = cosLight > 1.0e-6 ? selectedPdf * distSq / (cosLight * area) : 0.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 3u) {
        vec3 samplePos = selected.data1.xyz;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;
        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz;
        lightNormalOut = -wi;
        emission = sampleRadiance / max(distSq, 1e-4);
        pdfSolidAngle = selectedPdf;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 5u) {
        vec3 samplePos = selected.data1.xyz;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;

        vec3 spotDirection = normalize(vec3(selected.data1.w, selected.data2.w, selected.data3.x));
        float innerCone = clamp(selected.data3.y, 0.0, PI);
        float outerCone = clamp(max(selected.data3.z, innerCone), 0.0, PI);
        float cosInner = cos(innerCone);
        float cosOuter = cos(outerCone);
        float cosTheta = dot(-wi, spotDirection);
        float coneWeight = clamp((cosTheta - cosOuter) / max(cosInner - cosOuter, 1.0e-4), 0.0, 1.0);
        coneWeight *= coneWeight;

        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz * coneWeight;
        lightNormalOut = spotDirection;
        emission = sampleRadiance / max(distSq, 1e-4);
        pdfSolidAngle = selectedPdf;
        return coneWeight > 0.0 && has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 4u) {
        vec3 center = selected.data1.xyz;
        vec3 lightNormal = normalize(vec3(selected.data1.w, selected.data2.w, selected.data3.x));
        vec3 tangent = normalize(abs(lightNormal.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), lightNormal) : cross(vec3(1.0, 0.0, 0.0), lightNormal));
        vec3 bitangent = normalize(cross(lightNormal, tangent));
        float halfSize = selected.data0.z * 0.5;
        vec3 samplePos = center + tangent * ((rand_f32(rng) * 2.0 - 1.0) * halfSize) + bitangent * ((rand_f32(rng) * 2.0 - 1.0) * halfSize);
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;
        float cosLight = max(dot(-wi, lightNormal), 0.0);
        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz;
        lightNormalOut = lightNormal;
        emission = sampleRadiance;
        float selectionPdf = selectedPdf;
        float area = max(selected.data0.w, 1e-6);
        pdfSolidAngle = cosLight > 1e-6 ? selectionPdf * distSq / (cosLight * area) : 0.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x != 0u) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    uint ti = selected.metadata.y * TRI_STRIDE;
    uint instanceIndex = selected.metadata.w;
    if (instanceIndex >= mesh_params.instance_count || ti + 3u >= mesh_params.local_triangle_count * TRI_STRIDE) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    InstanceRecord instance = instance_records[instanceIndex];
    vec4 td3 = local_triangle_data[ti + 3u];
    Material material = decode_material(selected.metadata.z);
    vec3 v0 = (instance.transform * vec4(local_triangle_data[ti + 0u].xyz, 1.0)).xyz;
    vec3 v1 = (instance.transform * vec4(local_triangle_data[ti + 1u].xyz, 1.0)).xyz;
    vec3 v2 = (instance.transform * vec4(local_triangle_data[ti + 2u].xyz, 1.0)).xyz;
    float r1 = rand_f32(rng);
    float r2 = rand_f32(rng);
    float sr1 = sqrt(r1);
    vec3 bary = vec3(1.0 - sr1, sr1 * (1.0 - r2), sr1 * r2);
    vec3 samplePos = v0 * bary.x + v1 * bary.y + v2 * bary.z;
    vec3 lightNormal = normalize(mat3(instance.normal_transform) * td3.xyz);
    vec4 uv01 = local_triangle_data[ti + 4u];
    vec4 uv2 = local_triangle_data[ti + 5u];
    vec2 sampleUv = uv01.xy * bary.x + uv01.zw * bary.y + uv2.xy * bary.z;
    apply_material_textures(material, sampleUv, sampleUv);
    vec3 recordRadiance = vec3(selected.data1.w, selected.data2.w, selected.data3.w);
    sampleRadiance = has_positive_radiance(material.emissive)
        ? material.emissive
        : max(recordRadiance, vec3(0.0));
    emission = sampleRadiance;

    vec3 toLight = samplePos - hitPos;
    float distSq = dot(toLight, toLight);
    distanceToLight = sqrt(max(distSq, 1e-8));
    wi = toLight / distanceToLight;
    float cosLight = abs(dot(-wi, lightNormal));
    samplePosition = samplePos;
    lightNormalOut = lightNormal;
    float selectionPdf = selectedPdf;
    float area = max(selected.data0.w, 1e-6);
    pdfSolidAngle = cosLight > 1e-6 ? selectionPdf * distSq / (cosLight * area) : 0.0;
    return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
}

bool sample_sun_light(inout uint rng, out vec3 wi, out vec3 radiance, out float pdfSolidAngle) {
    if (analytical_sun_visibility() <= 0.0) {
        wi = vec3(0.0, 1.0, 0.0);
        radiance = vec3(0.0);
        pdfSolidAngle = 0.0;
        return false;
    }
    vec3 sunDir = analytical_sun_direction();
    float radius = clamp(camera.sun_color_angular_radius.w, 0.0001, 0.08);
    float cosRadius = cos(radius);
    float u1 = rand_f32(rng);
    float u2 = rand_f32(rng);
    float cosTheta = mix(cosRadius, 1.0, u1);
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float phi = 2.0 * PI * u2;
    vec3 axis = abs(sunDir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(axis, sunDir));
    vec3 bitangent = cross(sunDir, tangent);
    wi = tangent * cos(phi) * sinTheta + bitangent * sin(phi) * sinTheta + sunDir * cosTheta;
    pdfSolidAngle = 1.0 / analytical_sun_solid_angle();
    radiance = analytical_sun_center_radiance();
    return luminance(radiance) > 0.0 && pdfSolidAngle > 0.0;
}

bool sample_emissive_light_index(
    inout uint rng,
    uint lightIndex,
    float selectedPdf,
    vec3 hitPos,
    out vec3 wi,
    out vec3 emission,
    out float pdfSolidAngle,
    out float distanceToLight,
    out uint lightIndexOut,
    out uint lightKindOut,
    out vec3 samplePosition,
    out vec3 sampleRadiance,
    out vec3 lightNormalOut) {
    lightIndexOut = lightIndex;
    lightKindOut = 0u;
    samplePosition = vec3(0.0);
    sampleRadiance = vec3(0.0);
    lightNormalOut = vec3(0.0, 1.0, 0.0);
    if (lightIndex >= mesh_params.light_count || mesh_params.emissive_total_area <= 1.0e-8) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    LightRecord selected = light_records[lightIndex];
    lightKindOut = selected.metadata.x;
    if (selected.metadata.x == 2u) {
        wi = normalize(selected.data1.xyz);
        emission = selected.data2.xyz;
        samplePosition = wi;
        sampleRadiance = emission;
        lightNormalOut = -wi;
        pdfSolidAngle = selectedPdf;
        distanceToLight = 10000.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 1u) {
        vec3 center = selected.data1.xyz;
        float radius = max(selected.data0.z, 1.0e-6);
        float z = rand_f32(rng) * 2.0 - 1.0;
        float phi = 2.0 * PI * rand_f32(rng);
        float radial = sqrt(max(1.0 - z * z, 0.0));
        vec3 lightNormal = vec3(radial * cos(phi), z, radial * sin(phi));
        vec3 samplePos = center + lightNormal * radius;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1.0e-8));
        wi = toLight / distanceToLight;
        float cosLight = max(dot(-wi, lightNormal), 0.0);
        samplePosition = samplePos;
        sampleRadiance = max(selected.data2.xyz, vec3(0.0));
        lightNormalOut = lightNormal;
        emission = sampleRadiance;
        float area = max(selected.data0.w, 1.0e-6);
        pdfSolidAngle = cosLight > 1.0e-6 ? selectedPdf * distSq / (cosLight * area) : 0.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 3u) {
        vec3 samplePos = selected.data1.xyz;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;
        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz;
        lightNormalOut = -wi;
        emission = sampleRadiance / max(distSq, 1e-4);
        pdfSolidAngle = selectedPdf;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 5u) {
        vec3 samplePos = selected.data1.xyz;
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;
        vec3 spotDirection = normalize(vec3(selected.data1.w, selected.data2.w, selected.data3.x));
        float innerCone = clamp(selected.data3.y, 0.0, PI);
        float outerCone = clamp(max(selected.data3.z, innerCone), 0.0, PI);
        float cosInner = cos(innerCone);
        float cosOuter = cos(outerCone);
        float cosTheta = dot(-wi, spotDirection);
        float coneWeight = clamp((cosTheta - cosOuter) / max(cosInner - cosOuter, 1.0e-4), 0.0, 1.0);
        coneWeight *= coneWeight;
        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz * coneWeight;
        lightNormalOut = spotDirection;
        emission = sampleRadiance / max(distSq, 1e-4);
        pdfSolidAngle = selectedPdf;
        return coneWeight > 0.0 && has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x == 4u) {
        vec3 center = selected.data1.xyz;
        vec3 lightNormal = normalize(vec3(selected.data1.w, selected.data2.w, selected.data3.x));
        vec3 tangent = normalize(abs(lightNormal.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), lightNormal) : cross(vec3(1.0, 0.0, 0.0), lightNormal));
        vec3 bitangent = normalize(cross(lightNormal, tangent));
        float halfSize = selected.data0.z * 0.5;
        vec3 samplePos = center + tangent * ((rand_f32(rng) * 2.0 - 1.0) * halfSize) + bitangent * ((rand_f32(rng) * 2.0 - 1.0) * halfSize);
        vec3 toLight = samplePos - hitPos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1e-8));
        wi = toLight / distanceToLight;
        float cosLight = max(dot(-wi, lightNormal), 0.0);
        samplePosition = samplePos;
        sampleRadiance = selected.data2.xyz;
        lightNormalOut = lightNormal;
        emission = sampleRadiance;
        float area = max(selected.data0.w, 1e-6);
        pdfSolidAngle = cosLight > 1e-6 ? selectedPdf * distSq / (cosLight * area) : 0.0;
        return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
    }
    if (selected.metadata.x != 0u) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    uint ti = selected.metadata.y * TRI_STRIDE;
    uint instanceIndex = selected.metadata.w;
    if (instanceIndex >= mesh_params.instance_count || ti + 3u >= mesh_params.local_triangle_count * TRI_STRIDE) {
        wi = vec3(0.0, 1.0, 0.0);
        emission = vec3(0.0);
        pdfSolidAngle = 0.0;
        distanceToLight = 0.0;
        return false;
    }

    InstanceRecord instance = instance_records[instanceIndex];
    vec4 td3 = local_triangle_data[ti + 3u];
    Material material = decode_material(selected.metadata.z);
    vec3 v0 = (instance.transform * vec4(local_triangle_data[ti + 0u].xyz, 1.0)).xyz;
    vec3 v1 = (instance.transform * vec4(local_triangle_data[ti + 1u].xyz, 1.0)).xyz;
    vec3 v2 = (instance.transform * vec4(local_triangle_data[ti + 2u].xyz, 1.0)).xyz;
    float r1 = rand_f32(rng);
    float r2 = rand_f32(rng);
    float sr1 = sqrt(r1);
    vec3 bary = vec3(1.0 - sr1, sr1 * (1.0 - r2), sr1 * r2);
    vec3 samplePos = v0 * bary.x + v1 * bary.y + v2 * bary.z;
    vec3 lightNormal = normalize(mat3(instance.normal_transform) * td3.xyz);
    vec4 uv01 = local_triangle_data[ti + 4u];
    vec4 uv2 = local_triangle_data[ti + 5u];
    vec2 sampleUv = uv01.xy * bary.x + uv01.zw * bary.y + uv2.xy * bary.z;
    apply_material_textures(material, sampleUv, sampleUv);
    vec3 recordRadiance = vec3(selected.data1.w, selected.data2.w, selected.data3.w);
    sampleRadiance = has_positive_radiance(material.emissive)
        ? material.emissive
        : max(recordRadiance, vec3(0.0));
    emission = sampleRadiance;

    vec3 toLight = samplePos - hitPos;
    float distSq = dot(toLight, toLight);
    distanceToLight = sqrt(max(distSq, 1e-8));
    wi = toLight / distanceToLight;
    float cosLight = abs(dot(-wi, lightNormal));
    samplePosition = samplePos;
    lightNormalOut = lightNormal;
    float area = max(selected.data0.w, 1e-6);
    pdfSolidAngle = cosLight > 1e-6 ? selectedPdf * distSq / (cosLight * area) : 0.0;
    return has_positive_radiance(emission) && pdfSolidAngle > 0.0;
}

void mark_regir_active_cell(uint cellIndex, uint totalCells) {
    if (!regir_active_grid_enabled() || cellIndex >= totalCells) {
        return;
    }
    const uint slot = 4u + cellIndex;
    if (slot >= regir_active_cells.length()) {
        return;
    }
    const uint previous = atomicExchange(regir_active_cells[slot], 1u);
    if (previous == 0u) {
        atomicAdd(regir_active_cells[0], 1u);
    }
}

const uint REGIR_HASH_EMPTY = 0xffffffffu;

uint regir_hash_value(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

uint regir_hash_lookup_cell(uint cellIndex) {
    if (!regir_hash_grid_enabled() || regir_hash_current_cells.length() < 5u) {
        return REGIR_HASH_EMPTY;
    }
    const uint capacity = regir_hash_current_cells[3];
    if (capacity == 0u || 4u + capacity > regir_hash_current_cells.length()) {
        return REGIR_HASH_EMPTY;
    }
    const uint probeCount = min(capacity, 8u);
    const uint baseSlot = regir_hash_value(cellIndex) & (capacity - 1u);
    for (uint probe = 0u; probe < probeCount; ++probe) {
        const uint slot = (baseSlot + probe) & (capacity - 1u);
        const uint key = regir_hash_current_cells[4u + slot];
        if (key == cellIndex) {
            return slot;
        }
        if (key == REGIR_HASH_EMPTY) {
            break;
        }
    }
    return REGIR_HASH_EMPTY;
}

void mark_regir_hash_cell(uint cellIndex) {
    if (!regir_hash_grid_enabled() || regir_active_cells.length() < 5u) {
        return;
    }
    const uint capacity = regir_active_cells[3];
    if (capacity == 0u || 4u + capacity > regir_active_cells.length()) {
        return;
    }
    const uint probeCount = min(capacity, 8u);
    const uint baseSlot = regir_hash_value(cellIndex) & (capacity - 1u);
    for (uint probe = 0u; probe < probeCount; ++probe) {
        const uint slot = (baseSlot + probe) & (capacity - 1u);
        const uint previous = atomicCompSwap(regir_active_cells[4u + slot], REGIR_HASH_EMPTY, cellIndex);
        if (previous == REGIR_HASH_EMPTY) {
            atomicAdd(regir_active_cells[0], 1u);
            return;
        }
        if (previous == cellIndex) {
            return;
        }
        atomicAdd(regir_active_cells[1], 1u);
    }
    atomicAdd(regir_active_cells[2], 1u);
}

#if RTV_REGIR_FINITE_LIGHT_TRACE_ENABLED
bool regir_reservoir_identity_matches(ReGIRReservoir reservoir, LightRecord light) {
    return reservoir.metadata.y == light.metadata.x &&
        reservoir.light_identity.x == light.identity.x &&
        reservoir.light_identity.y == light.identity.y &&
        reservoir.light_identity.z == light.identity.z &&
        reservoir.light_identity.w == light.identity.w;
}

bool resolve_regir_reservoir_light(ReGIRReservoir reservoir, out uint lightIndex) {
    if (reservoir.metadata.w == 0u || mesh_params.light_count == 0u) {
        lightIndex = 0u;
        return false;
    }

    uint cachedIndex = reservoir.metadata.x;
    if (cachedIndex < mesh_params.light_count &&
        regir_reservoir_identity_matches(reservoir, light_records[cachedIndex])) {
        lightIndex = cachedIndex;
        return true;
    }

    for (uint i = 0u; i < mesh_params.light_count; ++i) {
        if (regir_reservoir_identity_matches(reservoir, light_records[i])) {
            lightIndex = i;
            return true;
        }
    }

    lightIndex = 0u;
    return false;
}

bool sample_regir_emissive_light(
    inout uint rng,
    vec3 hitPos,
    Material material,
    vec3 wo,
    vec3 lightingNormal,
    vec3 tangent,
    vec3 bitangent,
    float rayTime,
    out vec3 wi,
    out vec3 emission,
    out float pdfSolidAngle,
    out float distanceToLight,
    out uint lightIndexOut,
    out uint lightKindOut,
    out vec3 samplePosition,
    out vec3 sampleRadiance,
    out vec3 lightNormalOut,
    out uint queryCountOut,
    out float reservoirWeightOut,
    out float spatialInputWeightOut,
    out float spatialOutputWeightOut,
    out uint spatialNeighborCountOut,
    out float activeCellOccupancyOut,
    out float hashCollisionsOut,
    out uvec3 queryCellOut,
    out vec3 reusedVisibilityOut,
    out bool reusedVisibilityKnownOut,
    out uint visibilityRaysOut,
    out uint visibilityTransmissiveHitsOut,
    out uint visibilityVisiblePathsOut,
    out uint visibilityBlockedPathsOut) {
    queryCountOut = 0u;
    reservoirWeightOut = 0.0;
    spatialInputWeightOut = 0.0;
    spatialOutputWeightOut = 0.0;
    spatialNeighborCountOut = 0u;
    activeCellOccupancyOut = 0.0;
    hashCollisionsOut = 0.0;
    queryCellOut = uvec3(0u);
    reusedVisibilityOut = vec3(1.0);
    reusedVisibilityKnownOut = false;
    visibilityRaysOut = 0u;
    visibilityTransmissiveHitsOut = 0u;
    visibilityVisiblePathsOut = 0u;
    visibilityBlockedPathsOut = 0u;
    lightIndexOut = 0xffffffffu;
    if (!regir_enabled() || mesh_params.light_count == 0u) {
        return false;
    }
    vec3 rootMin = light_bvh_nodes[0].xyz;
    vec3 rootMax = light_bvh_nodes[1].xyz;
    vec3 extent = max(rootMax - rootMin, vec3(1.0e-3));
    vec3 padding = extent * max(regir_params.grid_padding.x, 0.0);
    rootMin -= padding;
    rootMax += padding;
    extent = max(rootMax - rootMin, vec3(1.0e-3));
    vec3 uv = clamp((hitPos - rootMin) / extent, vec3(0.0), vec3(0.999999));
    uvec3 dims = max(regir_params.grid_dimensions_reservoirs.xyz, uvec3(1u));
    if (regir_stochastic_query_enabled()) {
        vec3 jitter = vec3(rand_f32(rng), rand_f32(rng), rand_f32(rng)) - vec3(0.5);
        uv = clamp(uv + jitter / vec3(dims), vec3(0.0), vec3(0.999999));
    }
    uvec3 baseCell = min(uvec3(uv * vec3(dims)), dims - uvec3(1u));
    queryCellOut = baseCell;
    uint reservoirsPerCell = max(regir_params.grid_dimensions_reservoirs.w, 1u);
    uint cellQueryExtent = regir_stochastic_query_enabled() ? 1u : 2u;
    uint reservoirQueryCount = regir_stochastic_query_enabled()
        ? min(reservoirsPerCell, 8u)
        : reservoirsPerCell;
    uint reservoirQueryStart = regir_stochastic_query_enabled()
        ? min(uint(rand_f32(rng) * float(reservoirsPerCell)), reservoirsPerCell - 1u)
        : 0u;
    uint reservoirQueryStride = reservoirsPerCell > 1u ? reservoirsPerCell - 1u : 1u;
    uint totalCells = dims.x * dims.y * dims.z;
    if (regir_hash_grid_enabled() && regir_active_cells.length() >= 4u) {
        const uint hashCapacity = max(regir_active_cells[3], 1u);
        hashCollisionsOut = clamp(
            float(regir_active_cells[1] + regir_active_cells[2]) / float(hashCapacity),
            0.0,
            1.0);
    }

    float sourceWeightSum = 0.0;
    float inputSourceWeightSum = 0.0;
    uint spatialNeighborCount = 0u;
    uint queriedReservoirSlots = 0u;
    uint activeReservoirSlots = 0u;
    for (uint dz = 0u; dz < cellQueryExtent; ++dz) {
        uint cz = min(baseCell.z + dz, dims.z - 1u);
        for (uint dy = 0u; dy < cellQueryExtent; ++dy) {
            uint cy = min(baseCell.y + dy, dims.y - 1u);
            for (uint dx = 0u; dx < cellQueryExtent; ++dx) {
                uint cx = min(baseCell.x + dx, dims.x - 1u);
                uint cellIndex = (cz * dims.y + cy) * dims.x + cx;
                mark_regir_active_cell(cellIndex, totalCells);
                mark_regir_hash_cell(cellIndex);
                uint storageCellIndex = cellIndex;
                if (regir_hash_grid_enabled()) {
                    storageCellIndex = regir_hash_lookup_cell(cellIndex);
                    if (storageCellIndex == REGIR_HASH_EMPTY) {
                        continue;
                    }
                }
                for (uint ri = 0u; ri < reservoirQueryCount; ++ri) {
                    uint r = (reservoirQueryStart + ri * reservoirQueryStride) % reservoirsPerCell;
                    uint reservoirIndex = storageCellIndex * reservoirsPerCell + r;
                    ReGIRReservoir inputReservoir = regir_input_reservoirs[reservoirIndex];
                    uint inputLightIndex;
                    if (resolve_regir_reservoir_light(inputReservoir, inputLightIndex)) {
                        inputSourceWeightSum += max(inputReservoir.sample_position_weight.w, 0.0);
                    }
                    ReGIRReservoir reservoir = regir_reservoirs[reservoirIndex];
                    ++queryCountOut;
                    ++queriedReservoirSlots;
                    uint resolvedLightIndex;
                    if (!resolve_regir_reservoir_light(reservoir, resolvedLightIndex)) {
                        continue;
                    }
                    float reservoirSourceWeight = max(reservoir.sample_position_weight.w, 0.0);
                    if (reservoirSourceWeight <= 0.0) {
                        continue;
                    }
                    ++activeReservoirSlots;
                    sourceWeightSum += reservoirSourceWeight;
                    if (regir_spatial_reuse_enabled()) {
                        spatialNeighborCount += reservoir.metadata.w > inputReservoir.metadata.w
                            ? reservoir.metadata.w - inputReservoir.metadata.w
                            : 0u;
                    }
                }
            }
        }
    }
    activeCellOccupancyOut = queriedReservoirSlots > 0u
        ? float(activeReservoirSlots) / float(queriedReservoirSlots)
        : 0.0;
    spatialInputWeightOut = inputSourceWeightSum;
    spatialOutputWeightOut = sourceWeightSum;
    spatialNeighborCountOut = spatialNeighborCount;
    if (sourceWeightSum <= 0.0) {
        return false;
    }

    float risWeightSum = 0.0;
    uint validCandidateCount = 0u;
    uint selectedLight = 0xffffffffu;
    uint selectedKind = 0u;
    vec3 selectedWi = vec3(0.0, 1.0, 0.0);
    vec3 selectedEmission = vec3(0.0);
    float selectedPdf = 0.0;
    float selectedDistance = 0.0;
    vec3 selectedPosition = vec3(0.0);
    vec3 selectedRadiance = vec3(0.0);
    vec3 selectedNormal = vec3(0.0, 1.0, 0.0);
    float selectedTarget = 0.0;
    vec3 selectedVisibility = vec3(1.0);
    bool selectedVisibilityKnown = false;
    const uint visibilityCandidateBudget = 4u;
    for (uint dz = 0u; dz < cellQueryExtent; ++dz) {
        uint cz = min(baseCell.z + dz, dims.z - 1u);
        for (uint dy = 0u; dy < cellQueryExtent; ++dy) {
            uint cy = min(baseCell.y + dy, dims.y - 1u);
            for (uint dx = 0u; dx < cellQueryExtent; ++dx) {
                uint cx = min(baseCell.x + dx, dims.x - 1u);
                uint cellIndex = (cz * dims.y + cy) * dims.x + cx;
                uint storageCellIndex = cellIndex;
                if (regir_hash_grid_enabled()) {
                    storageCellIndex = regir_hash_lookup_cell(cellIndex);
                    if (storageCellIndex == REGIR_HASH_EMPTY) {
                        continue;
                    }
                }
                for (uint ri = 0u; ri < reservoirQueryCount; ++ri) {
                    uint r = (reservoirQueryStart + ri * reservoirQueryStride) % reservoirsPerCell;
                    ReGIRReservoir reservoir = regir_reservoirs[storageCellIndex * reservoirsPerCell + r];
                    ++queryCountOut;
                    uint resolvedLightIndex;
                    if (!resolve_regir_reservoir_light(reservoir, resolvedLightIndex)) {
                        continue;
                    }
                    float sourceWeight = max(reservoir.sample_position_weight.w, 0.0);
                    if (sourceWeight <= 0.0) {
                        continue;
                    }
                    float averageSourceWeight = max(reservoir.proposal_pdf_m.z, 1.0e-8);
                    float sourcePdf = sourceWeight /
                        max(averageSourceWeight * float(max(mesh_params.light_count, 1u)), 1.0e-8);
                    vec3 candidateWi;
                    vec3 candidateEmission;
                    float candidatePdf;
                    float candidateDistance;
                    uint candidateLightIndex;
                    uint candidateLightKind;
                    vec3 candidatePosition;
                    vec3 candidateRadiance;
                    vec3 candidateNormal;
                    if (!sample_emissive_light_index(
                            rng,
                            resolvedLightIndex,
                            sourcePdf,
                            hitPos,
                            candidateWi,
                            candidateEmission,
                            candidatePdf,
                            candidateDistance,
                            candidateLightIndex,
                            candidateLightKind,
                            candidatePosition,
                            candidateRadiance,
                            candidateNormal)) {
                        continue;
                    }
                    float candidateCos = max(dot(lightingNormal, candidateWi), 0.0);
                    if (candidateCos <= 0.0 || candidatePdf <= 1.0e-6) {
                        continue;
                    }
                    if (regir_visibility_reuse_enabled() && validCandidateCount >= visibilityCandidateBudget) {
                        continue;
                    }
                    vec3 candidateBsdf = eval_brdf(material, wo, candidateWi, lightingNormal, tangent, bitangent);
                    vec3 candidateVisibility = vec3(1.0);
                    bool candidateVisibilityKnown = false;
                    if (regir_visibility_reuse_enabled()) {
                        uint transmissiveHits;
                        uint visiblePath;
                        uint blockedPath;
                        record_rt_counter(RT_DIAG_EMISSIVE_DIRECT_SHADOW_RAYS);
                        candidateVisibility = direct_shadow_transmittance_stats(
                            hitPos + lightingNormal * shadow_self_hit_epsilon(),
                            candidateWi,
                            max(candidateDistance - shadow_distance_bias(), 0.0),
                            rayTime,
                            material,
                            transmissiveHits,
                            visiblePath,
                            blockedPath);
                        ++visibilityRaysOut;
                        visibilityTransmissiveHitsOut += transmissiveHits;
                        visibilityVisiblePathsOut += visiblePath;
                        visibilityBlockedPathsOut += blockedPath;
                        candidateVisibilityKnown = true;
                    }
                    vec3 candidateNumerator =
                        candidateBsdf * candidateEmission * candidateVisibility * candidateCos;
                    float candidateTarget = max(
                        dot(candidateNumerator, vec3(0.2126, 0.7152, 0.0722)),
                        0.0);
                    float risWeight = candidateTarget / max(candidatePdf, 1.0e-6);
                    ++validCandidateCount;
                    risWeightSum += risWeight;
                    if (risWeight > 0.0 && rand_f32(rng) * risWeightSum <= risWeight) {
                        selectedLight = resolvedLightIndex;
                        selectedKind = light_records[resolvedLightIndex].metadata.x;
                        selectedWi = candidateWi;
                        selectedEmission = candidateEmission;
                        selectedPdf = candidatePdf;
                        selectedDistance = candidateDistance;
                        selectedPosition = candidatePosition;
                        selectedRadiance = candidateRadiance;
                        selectedNormal = candidateNormal;
                        selectedTarget = candidateTarget;
                        selectedVisibility = candidateVisibility;
                        selectedVisibilityKnown = candidateVisibilityKnown;
                    }
                }
            }
        }
    }
    reservoirWeightOut = risWeightSum;
    if (selectedLight == 0xffffffffu || selectedTarget <= 0.0 || validCandidateCount == 0u) {
        return false;
    }
    wi = selectedWi;
    emission = selectedEmission;
    pdfSolidAngle = selectedTarget * float(validCandidateCount) / max(risWeightSum, 1.0e-8);
    distanceToLight = selectedDistance;
    lightIndexOut = selectedLight;
    lightKindOut = selectedKind;
    samplePosition = selectedPosition;
    sampleRadiance = selectedRadiance;
    lightNormalOut = selectedNormal;
    reusedVisibilityOut = selectedVisibility;
    reusedVisibilityKnownOut = selectedVisibilityKnown;
    return pdfSolidAngle > 0.0 && has_positive_radiance(emission);
}
#else
bool sample_regir_emissive_light(
    inout uint rng,
    vec3 hitPos,
    Material material,
    vec3 wo,
    vec3 lightingNormal,
    vec3 tangent,
    vec3 bitangent,
    float rayTime,
    out vec3 wi,
    out vec3 emission,
    out float pdfSolidAngle,
    out float distanceToLight,
    out uint lightIndexOut,
    out uint lightKindOut,
    out vec3 samplePosition,
    out vec3 sampleRadiance,
    out vec3 lightNormalOut,
    out uint queryCountOut,
    out float reservoirWeightOut,
    out float spatialInputWeightOut,
    out float spatialOutputWeightOut,
    out uint spatialNeighborCountOut,
    out float activeCellOccupancyOut,
    out float hashCollisionsOut,
    out uvec3 queryCellOut,
    out vec3 reusedVisibilityOut,
    out bool reusedVisibilityKnownOut,
    out uint visibilityRaysOut,
    out uint visibilityTransmissiveHitsOut,
    out uint visibilityVisiblePathsOut,
    out uint visibilityBlockedPathsOut) {
    wi = vec3(0.0, 1.0, 0.0);
    emission = vec3(0.0);
    pdfSolidAngle = 0.0;
    distanceToLight = 0.0;
    lightIndexOut = 0xffffffffu;
    lightKindOut = 0u;
    samplePosition = vec3(0.0);
    sampleRadiance = vec3(0.0);
    lightNormalOut = vec3(0.0, 1.0, 0.0);
    queryCountOut = 0u;
    reservoirWeightOut = 0.0;
    spatialInputWeightOut = 0.0;
    spatialOutputWeightOut = 0.0;
    spatialNeighborCountOut = 0u;
    activeCellOccupancyOut = 0.0;
    hashCollisionsOut = 0.0;
    queryCellOut = uvec3(0u);
    reusedVisibilityOut = vec3(1.0);
    reusedVisibilityKnownOut = false;
    visibilityRaysOut = 0u;
    visibilityTransmissiveHitsOut = 0u;
    visibilityVisiblePathsOut = 0u;
    visibilityBlockedPathsOut = 0u;
    return false;
}
#endif

bool sample_regir_infinite_direction(
    inout uint rng,
    bool sampleSun,
    out vec3 direction,
    out float sourcePdf,
    out vec3 radiance,
    out uint sourceKind,
    out uint sampleCount,
    out uint generationMismatch) {
    direction = vec3(0.0, 1.0, 0.0);
    sourcePdf = 0.0;
    radiance = vec3(0.0);
    sourceKind = 0u;
    sampleCount = 0u;
    generationMismatch = 0u;
    if ((sampleSun && !regir_sun_enabled()) || (!sampleSun && !regir_environment_enabled())) {
        return false;
    }

    uint bankOffset = sampleSun ? regir_params.environment_controls.z : 0u;
    uint configuredCount = sampleSun
        ? regir_params.environment_controls.w
        : regir_params.environment_controls.z;
    if (bankOffset >= regir_environment_reservoirs.length()) {
        return false;
    }
    uint bankSize = min(configuredCount, regir_environment_reservoirs.length() - bankOffset);
    if (bankSize == 0u) {
        return false;
    }
    for (uint attempt = 0u; attempt < 4u; ++attempt) {
        uint slot = bankOffset + min(uint(rand_f32(rng) * float(bankSize)), bankSize - 1u);
        ReGIREnvironmentReservoir reservoir = regir_environment_reservoirs[slot];
        if (reservoir.metadata.x == 0u || reservoir.direction_pdf.w <= 0.0) {
            continue;
        }
        uint expectedGeneration = sampleSun
            ? camera.gi_version_controls.x
            : camera.gi_version_controls.w;
        if (reservoir.metadata.z != expectedGeneration ||
            (sampleSun && reservoir.metadata.y != 4u) ||
            (!sampleSun && reservoir.metadata.y == 4u)) {
            generationMismatch = 1u;
            continue;
        }
        direction = normalize(reservoir.direction_pdf.xyz);
        float storedSourcePdf = reservoir.direction_pdf.w;
        float selectedTarget = max(reservoir.reservoir_state.x, 0.0);
        float weightSum = max(reservoir.reservoir_state.y, 0.0);
        float reservoirM = max(reservoir.reservoir_state.w, 1.0);
        float reconstructedPdf = weightSum > 0.0
            ? selectedTarget * reservoirM / weightSum
            : storedSourcePdf;
        sourcePdf = reconstructedPdf > 0.0 ? reconstructedPdf : storedSourcePdf;
        radiance = sampleSun
            ? analytical_sun_center_radiance()
            : environment_radiance(direction, ATMOSPHERE_RAY_QUALITY_FULL);
        sourceKind = reservoir.metadata.y;
        sampleCount = max(uint(reservoirM + 0.5), 1u);
        return has_positive_radiance(radiance);
    }
    return false;
}

bool sample_regir_environment_direction(
    inout uint rng,
    out vec3 direction,
    out float sourcePdf,
    out vec3 radiance,
    out uint sourceKind,
    out uint sampleCount,
    out uint generationMismatch) {
    return sample_regir_infinite_direction(
        rng, false, direction, sourcePdf, radiance, sourceKind, sampleCount, generationMismatch);
}

bool sample_regir_sun_direction(
    inout uint rng,
    out vec3 direction,
    out float sourcePdf,
    out vec3 radiance,
    out uint sourceKind,
    out uint sampleCount,
    out uint generationMismatch) {
    return sample_regir_infinite_direction(
        rng, true, direction, sourcePdf, radiance, sourceKind, sampleCount, generationMismatch);
}

bool regir_environment_direct_available(uint bounce) {
    return bounce >= 2u && regir_environment_enabled();
}

void regir_environment_sampling_probabilities(uint bounce, out float canonicalProbability, out float regirProbability) {
    canonicalProbability = 1.0;
    regirProbability = 0.0;
    if (regir_environment_direct_available(bounce)) {
        canonicalProbability = regir_canonical_mix();
        regirProbability = 1.0 - canonicalProbability;
    }
}

float environment_light_sampling_pdf(vec3 dir, uint bounce) {
    float sourcePdf = environment_pdf(dir);
    if (sourcePdf <= 0.0) {
        return 0.0;
    }
#if RTV_REGIR_TRACE_ENABLED
    float canonicalProbability;
    float regirProbability;
    regir_environment_sampling_probabilities(bounce, canonicalProbability, regirProbability);
    return sourcePdf * max(canonicalProbability + regirProbability, 0.0);
#else
    return sourcePdf;
#endif
}

bool regir_sun_direct_available(uint bounce) {
    return bounce >= 2u && regir_sun_enabled();
}

void regir_sun_sampling_probabilities(uint bounce, out float canonicalProbability, out float regirProbability) {
    canonicalProbability = 1.0;
    regirProbability = 0.0;
    if (regir_sun_direct_available(bounce)) {
        canonicalProbability = regir_canonical_mix();
        regirProbability = 1.0 - canonicalProbability;
    }
}

float sun_light_sampling_pdf(vec3 dir, uint bounce) {
    float sourcePdf = analytical_sun_pdf(dir);
    if (sourcePdf <= 0.0) {
        return 0.0;
    }
#if RTV_REGIR_TRACE_ENABLED
    float canonicalProbability;
    float regirProbability;
    regir_sun_sampling_probabilities(bounce, canonicalProbability, regirProbability);
    return sourcePdf * max(canonicalProbability + regirProbability, 0.0);
#else
    return sourcePdf;
#endif
}

float emissive_hit_pdf(RayPayload hit, Material material, vec3 previousOrigin) {
    float totalArea = mesh_params.emissive_total_area;
    if (totalArea <= 1e-8 || !has_positive_radiance(material.emissive)) {
        return 0.0;
    }
    vec3 toLight = hit.world_pos - previousOrigin;
    float distSq = dot(toLight, toLight);
    float dist = sqrt(max(distSq, 1e-8));
    vec3 wi = toLight / dist;
    float cosLight = abs(dot(-wi, hit.geom_normal));
    return cosLight > 1e-6 ? distSq / (cosLight * totalArea) : 0.0;
}

float terminal_emissive_hit_pdf(TerminalRayPayload hit, Material material, vec3 hitWorldPos, vec3 previousOrigin) {
    float totalArea = mesh_params.emissive_total_area;
    if (totalArea <= 1e-8 || !has_positive_radiance(material.emissive)) {
        return 0.0;
    }
    vec3 toLight = hitWorldPos - previousOrigin;
    float distSq = dot(toLight, toLight);
    vec3 wi = toLight / sqrt(max(distSq, 1e-8));
    float cosLight = abs(dot(-wi, hit.geom_normal));
    return cosLight > 1e-6 ? distSq / (cosLight * totalArea) : 0.0;
}

vec3 estimate_direct_lighting(
    inout uint rng,
    RayPayload hit,
    Material material,
    vec3 wo,
    float rayTime,
    uint bounce,
    out vec3 emissiveContribution,
    out vec3 environmentContribution,
    out float sampledLightPdf,
    out float sampledRawLightPdf,
    out float sampledEffectiveLightPdf,
    out float sampledBsdfPdf,
    out float sampledMisWeight,
    out uint sampledType,
    out uint sampledLightIndex,
    out uint sampledLightKind,
    out vec3 sampledLightPosition,
    out float sampledLightDistance,
    out vec3 sampledLightDirection,
    out vec3 sampledLightRadiance,
    out vec3 sampledLightNormal,
    out vec3 sampledRestirContribution,
#if RTV_REGIR_TRACE_ENABLED
    out uint regirQueryCount,
    out uint regirSelectedLight,
    out float regirReservoirWeight,
    out float regirMisWeight,
    out float regirEffectivePdf,
    out uint regirCanonicalUsed,
    out uvec3 regirQueryCell,
    out float regirActiveCellOccupancy,
    out float regirHashCollisions,
    out float regirSpatialInputWeight,
    out float regirSpatialOutputWeight,
    out uint regirSpatialNeighborCount,
    out uint regirEnvironmentSourceKind,
    out float regirEnvironmentSourcePdf,
    out float regirEnvironmentEffectivePdf,
    out vec3 regirEnvironmentDirection,
    out float regirEnvironmentMisWeight,
    out float regirEnvironmentM,
    out uint regirEnvironmentGenerationMismatch,
#endif
    out uint causticTransmissiveHits,
    out uint causticVisiblePaths,
    out uint causticBlockedPaths) {
    emissiveContribution = vec3(0.0);
    environmentContribution = vec3(0.0);
    sampledLightPdf = 0.0;
    sampledRawLightPdf = 0.0;
    sampledEffectiveLightPdf = 0.0;
    sampledBsdfPdf = 0.0;
    sampledMisWeight = 0.0;
    sampledType = 0u;
    sampledLightIndex = 0u;
    sampledLightKind = 0u;
    sampledLightPosition = vec3(0.0);
    sampledLightDistance = 0.0;
    sampledLightDirection = vec3(0.0, 1.0, 0.0);
    sampledLightRadiance = vec3(0.0);
    sampledLightNormal = vec3(0.0, 1.0, 0.0);
    sampledRestirContribution = vec3(0.0);
#if RTV_REGIR_TRACE_ENABLED
    regirQueryCount = 0u;
    regirSelectedLight = 0xffffffffu;
    regirReservoirWeight = 0.0;
    regirMisWeight = 0.0;
    regirEffectivePdf = 0.0;
    regirCanonicalUsed = 0u;
    regirQueryCell = uvec3(0u);
    regirActiveCellOccupancy = 0.0;
    regirHashCollisions = 0.0;
    regirSpatialInputWeight = 0.0;
    regirSpatialOutputWeight = 0.0;
    regirSpatialNeighborCount = 0u;
    regirEnvironmentSourceKind = 0u;
    regirEnvironmentSourcePdf = 0.0;
    regirEnvironmentEffectivePdf = 0.0;
    regirEnvironmentDirection = vec3(0.0, 1.0, 0.0);
    regirEnvironmentMisWeight = 0.0;
    regirEnvironmentM = 0.0;
    regirEnvironmentGenerationMismatch = 0u;
#endif
    causticTransmissiveHits = 0u;
    causticVisiblePaths = 0u;
    causticBlockedPaths = 0u;
    bool isDelta = material_is_delta(material);
    if (camera.direct_lighting_enabled == 0u || isDelta || renderer_debug_view() == 27u) {
        return vec3(0.0);
    }
    float secondaryDirectProbability = 1.0;
#if RTV_GENERIC_SECONDARY_ONE_INFINITE_LIGHT
    if (bounce >= 1u &&
        !regir_finite_light_enabled() &&
        !regir_infinite_light_enabled()) {
        secondaryDirectProbability = bounce >= 2u
            ? clamp(float(RTV_GENERIC_DEEP_SECONDARY_DIRECT_PROB), 0.0, 1.0)
            : clamp(float(RTV_GENERIC_SECONDARY_DIRECT_PROB), 0.0, 1.0);
    }
#endif
    if (secondaryDirectProbability <= 0.0) {
        return vec3(0.0);
    }
    if (secondaryDirectProbability < 0.999 &&
        rand_f32(rng) >= secondaryDirectProbability) {
        return vec3(0.0);
    }
    float secondaryDirectWeight = 1.0 / secondaryDirectProbability;

    vec3 lightingNormal = normalize(hit.geom_normal);
#if RTV_REGIR_TRACE_ENABLED
    bool regirCandidateAvailable = bounce >= 2u && regir_finite_light_enabled();
    uint risCandidateCount = regirCandidateAvailable ? 1u : direct_light_ris_candidate_count(bounce);
#else
    uint risCandidateCount = direct_light_ris_candidate_count(bounce);
#endif
    vec3 selectedWi = vec3(0.0, 1.0, 0.0);
    vec3 selectedEmission = vec3(0.0);
    vec3 selectedBsdf = vec3(0.0);
    float selectedLightPdf = 0.0;
    float selectedBsdfPdf = 0.0;
    float selectedDistance = 0.0;
    uint selectedLightIndex = 0u;
    uint selectedLightKind = 0u;
    vec3 selectedLightPosition = vec3(0.0);
    vec3 selectedLightRadiance = vec3(0.0);
    vec3 selectedLightNormal = vec3(0.0, 1.0, 0.0);
    vec3 selectedReusedVisibility = vec3(1.0);
    bool selectedReusedVisibilityKnown = false;
    float selectedProxy = 0.0;
    float sampledRestirProxy = 0.0;
    float proxyWeightSum = 0.0;
    bool selectedCandidate = false;
#if RTV_REGIR_TRACE_ENABLED
    bool selectedUsedCanonical = false;
#endif
    if (mesh_params.light_count != 0u && mesh_params.emissive_total_area > 1.0e-8) {
#if RTV_REGIR_TRACE_ENABLED
        float requestedCanonicalProbability = regirCandidateAvailable ? regir_canonical_mix() : 1.0;
        float regirProbability = regirCandidateAvailable
            ? (1.0 - requestedCanonicalProbability) * regir_finite_query_probability()
            : 0.0;
        float canonicalProbability = 1.0 - regirProbability;
#else
        const float canonicalProbability = 1.0;
#endif
        for (uint candidate = 0u; candidate < risCandidateCount; ++candidate) {
            vec3 candidateWi;
            vec3 candidateEmission;
            float candidateLightPdf;
            float candidateDistance;
            uint candidateLightIndex;
            uint candidateLightKind;
            vec3 candidateLightPosition;
            vec3 candidateLightRadiance;
            vec3 candidateLightNormal;
#if RTV_REGIR_TRACE_ENABLED
            uint candidateRegirQueries = 0u;
            float candidateRegirWeight = 0.0;
            float candidateRegirSpatialInputWeight = 0.0;
            float candidateRegirSpatialOutputWeight = 0.0;
            uint candidateRegirSpatialNeighborCount = 0u;
            uvec3 candidateRegirCell = uvec3(0u);
            float candidateRegirActiveCellOccupancy = 0.0;
            float candidateRegirHashCollisions = 0.0;
#endif
            vec3 candidateReusedVisibility = vec3(1.0);
            bool candidateReusedVisibilityKnown = false;
            uint candidateVisibilityRays = 0u;
            uint candidateVisibilityTransmissiveHits = 0u;
            uint candidateVisibilityVisiblePaths = 0u;
            uint candidateVisibilityBlockedPaths = 0u;
#if RTV_REGIR_TRACE_ENABLED
            bool candidateUsedCanonical = true;
#endif
            bool sampledCandidate = false;
#if RTV_REGIR_TRACE_ENABLED
            uint regirChoiceRng = rng ^ 0x9e3779b9u;
            if (regirCandidateAvailable &&
                regirProbability > 0.0 &&
                rand_f32(regirChoiceRng) >= canonicalProbability) {
                sampledCandidate = sample_regir_emissive_light(
                    rng,
                    hit.world_pos,
                    material,
                    wo,
                    lightingNormal,
                    hit.tangent,
                    hit.bitangent,
                    rayTime,
                    candidateWi,
                    candidateEmission,
                    candidateLightPdf,
                    candidateDistance,
                    candidateLightIndex,
                    candidateLightKind,
                    candidateLightPosition,
                    candidateLightRadiance,
                    candidateLightNormal,
                    candidateRegirQueries,
                    candidateRegirWeight,
                    candidateRegirSpatialInputWeight,
                    candidateRegirSpatialOutputWeight,
                    candidateRegirSpatialNeighborCount,
                    candidateRegirActiveCellOccupancy,
                    candidateRegirHashCollisions,
                    candidateRegirCell,
                    candidateReusedVisibility,
                    candidateReusedVisibilityKnown,
                    candidateVisibilityRays,
                    candidateVisibilityTransmissiveHits,
                    candidateVisibilityVisiblePaths,
                    candidateVisibilityBlockedPaths);
                causticTransmissiveHits += candidateVisibilityTransmissiveHits;
                causticVisiblePaths += candidateVisibilityVisiblePaths;
                causticBlockedPaths += candidateVisibilityBlockedPaths;
                if (sampledCandidate) {
                    candidateLightPdf *= regirProbability;
                    candidateUsedCanonical = false;
                }
            }
#endif
            if (!sampledCandidate && canonicalProbability > 0.0) {
                sampledCandidate = sample_emissive_light(
                    rng,
                    hit.world_pos,
                    candidateWi,
                    candidateEmission,
                    candidateLightPdf,
                    candidateDistance,
                    candidateLightIndex,
                    candidateLightKind,
                    candidateLightPosition,
                    candidateLightRadiance,
                    candidateLightNormal);
                if (sampledCandidate) {
                    candidateLightPdf *= canonicalProbability;
#if RTV_REGIR_TRACE_ENABLED
                    candidateUsedCanonical = true;
#endif
                }
            }
#if RTV_REGIR_TRACE_ENABLED
            regirQueryCount += candidateRegirQueries;
            if (candidateRegirQueries > 0u) {
                regirQueryCell = candidateRegirCell;
                regirSpatialInputWeight = candidateRegirSpatialInputWeight;
                regirSpatialOutputWeight = candidateRegirSpatialOutputWeight;
                regirSpatialNeighborCount = candidateRegirSpatialNeighborCount;
                regirActiveCellOccupancy = candidateRegirActiveCellOccupancy;
                regirHashCollisions = candidateRegirHashCollisions;
            }
#endif
            if (!sampledCandidate) {
                continue;
            }
            float candidateCos = max(dot(lightingNormal, candidateWi), 0.0);
            if (candidateCos <= 0.0 || candidateLightPdf <= 1.0e-6) {
                continue;
            }
            vec3 candidateBsdf = eval_brdf(material, wo, candidateWi, lightingNormal, hit.tangent, hit.bitangent);
            float candidateBsdfPdf = pdf_brdf(material, wo, candidateWi, lightingNormal, hit.tangent, hit.bitangent);
            vec3 candidateEstimate = candidateBsdf * candidateEmission *
                candidateReusedVisibility * candidateCos / max(candidateLightPdf, 1.0e-6);
            float candidateProxy = max(dot(candidateEstimate, vec3(0.2126, 0.7152, 0.0722)), 0.0);
            proxyWeightSum += candidateProxy;
            if (candidateProxy > 0.0 && rand_f32(rng) * proxyWeightSum <= candidateProxy) {
                selectedWi = candidateWi;
                selectedEmission = candidateEmission;
                selectedBsdf = candidateBsdf;
                selectedLightPdf = candidateLightPdf;
                selectedBsdfPdf = candidateBsdfPdf;
                selectedDistance = candidateDistance;
                selectedLightIndex = candidateLightIndex;
                selectedLightKind = candidateLightKind;
                selectedLightPosition = candidateLightPosition;
                selectedLightRadiance = candidateLightRadiance;
                selectedLightNormal = candidateLightNormal;
                selectedReusedVisibility = candidateReusedVisibility;
                selectedReusedVisibilityKnown = candidateReusedVisibilityKnown;
                selectedProxy = candidateProxy;
                selectedCandidate = true;
#if RTV_REGIR_TRACE_ENABLED
                selectedUsedCanonical = candidateUsedCanonical;
                if (!candidateUsedCanonical) {
                    regirSelectedLight = candidateLightIndex;
                    regirReservoirWeight = candidateRegirWeight;
                    regirSpatialInputWeight = candidateRegirSpatialInputWeight;
                    regirSpatialOutputWeight = candidateRegirSpatialOutputWeight;
                    regirSpatialNeighborCount = candidateRegirSpatialNeighborCount;
                    regirActiveCellOccupancy = candidateRegirActiveCellOccupancy;
                    regirHashCollisions = candidateRegirHashCollisions;
                }
#endif
            }
        }
    }
    if (selectedCandidate && selectedProxy > 0.0) {
        float shadowDistance = max(selectedDistance - shadow_distance_bias(), 0.0);
        vec3 shadowT = selectedReusedVisibility;
        if (!selectedReusedVisibilityKnown) {
            uint transmissiveHits;
            uint visiblePath;
            uint blockedPath;
            record_rt_counter(RT_DIAG_EMISSIVE_DIRECT_SHADOW_RAYS);
            shadowT = direct_shadow_transmittance_stats(
                shadow_origin(hit, selectedWi),
                selectedWi,
                shadowDistance,
                rayTime,
                material,
                transmissiveHits,
                visiblePath,
                blockedPath);
            causticTransmissiveHits += transmissiveHits;
            causticVisiblePaths += visiblePath;
            causticBlockedPaths += blockedPath;
        }
        float cosSurface = max(dot(lightingNormal, selectedWi), 0.0);
        if (luminance(shadowT) > 0.0 && cosSurface > 0.0) {
            float effectiveLightPdf = selectedLightPdf;
            if (risCandidateCount > 1u && proxyWeightSum > 1.0e-8 && selectedProxy > 1.0e-8) {
                effectiveLightPdf = selectedLightPdf * float(risCandidateCount) * selectedProxy / proxyWeightSum;
            }
            float weight = power_heuristic(effectiveLightPdf, selectedBsdfPdf);
            sampledLightPdf = effectiveLightPdf;
            sampledRawLightPdf = selectedLightPdf;
            sampledEffectiveLightPdf = effectiveLightPdf;
            sampledBsdfPdf = selectedBsdfPdf;
            sampledMisWeight = weight;
            sampledType = 1u;
            sampledLightIndex = selectedLightIndex;
            sampledLightKind = selectedLightKind;
            sampledLightPosition = selectedLightPosition;
            sampledLightDistance = selectedDistance;
            sampledLightDirection = selectedWi;
            sampledLightRadiance = selectedLightRadiance * shadowT;
            sampledLightNormal = selectedLightNormal;
#if RTV_REGIR_TRACE_ENABLED
            if (regirCandidateAvailable) {
                regirMisWeight = weight;
                regirEffectivePdf = effectiveLightPdf;
                regirCanonicalUsed = selectedUsedCanonical ? 1u : 0u;
                if (selectedUsedCanonical) {
                    regirSelectedLight = selectedLightIndex;
                }
            }
#endif
            emissiveContribution = selectedBsdf * selectedEmission * shadowT * cosSurface * weight / max(effectiveLightPdf, 1e-6);
            sampledRestirContribution = emissiveContribution;
            sampledRestirProxy = luminance(sampledRestirContribution);
        }
    }

    const bool secondaryOneInfiniteLight =
#if RTV_GENERIC_SECONDARY_ONE_INFINITE_LIGHT
        bounce >= 1u &&
        !regir_finite_light_enabled() &&
        !regir_infinite_light_enabled() &&
        env_params.enabled != 0u &&
        camera.sunlight_enabled != 0u;
#else
        false;
#endif
    float infiniteLightSelector = secondaryOneInfiniteLight ? rand_f32(rng) : 0.0;
    bool sampleEnvironmentDirect = !secondaryOneInfiniteLight || infiniteLightSelector < 0.5;
    bool sampleSunDirect = !secondaryOneInfiniteLight || !sampleEnvironmentDirect;
    float environmentTechniqueProbability = sampleEnvironmentDirect
        ? (secondaryOneInfiniteLight ? 0.5 : 1.0)
        : 0.0;
    float sunTechniqueProbability = sampleSunDirect
        ? (secondaryOneInfiniteLight ? 0.5 : 1.0)
        : 0.0;
    uint envDirectSampleCount = sampleEnvironmentDirect
        ? (secondaryOneInfiniteLight ? 1u : clamp(camera.environment_direct_samples, 1u, 8u))
        : 0u;
#if RTV_REGIR_TRACE_ENABLED
    float envCanonicalProbability;
    float envRegirProbability;
    regir_environment_sampling_probabilities(bounce, envCanonicalProbability, envRegirProbability);
#endif
    for (uint envSample = 0u; envSample < envDirectSampleCount; ++envSample) {
        vec3 envDir;
        float envPdf;
        vec3 envRadiance;
#if RTV_REGIR_TRACE_ENABLED
        uint envSourceKind;
        uint envReservoirSampleCount;
        uint envGenerationMismatch = 0u;
        bool sampledEnvironmentBank = false;
        if (envRegirProbability > 0.0 && rand_f32(rng) >= envCanonicalProbability) {
            sampledEnvironmentBank = sample_regir_environment_direction(
                rng,
                envDir,
                envPdf,
                envRadiance,
                envSourceKind,
                envReservoirSampleCount,
                envGenerationMismatch);
        }
        regirEnvironmentGenerationMismatch = max(regirEnvironmentGenerationMismatch, envGenerationMismatch);
        if (!sampledEnvironmentBank) {
            envRadiance = sample_environment_direction(rng, envDir, envPdf);
            envSourceKind = 0u;
            envReservoirSampleCount = 0u;
        }
#else
        envRadiance = sample_environment_direction(rng, envDir, envPdf);
#endif
        if (envPdf <= 0.0) {
            continue;
        }
        float cosSurface = max(dot(lightingNormal, envDir), 0.0);
        if (cosSurface <= 0.0) {
            continue;
        }
        uint transmissiveHits;
        uint visiblePath;
        uint blockedPath;
        record_rt_counter(RT_DIAG_ENV_DIRECT_SHADOW_RAYS);
        vec3 shadowT = direct_shadow_transmittance_stats(
            shadow_origin(hit, envDir),
            envDir,
            10000.0,
            rayTime,
            material,
            transmissiveHits,
            visiblePath,
            blockedPath);
        causticTransmissiveHits += transmissiveHits;
        causticVisiblePaths += visiblePath;
        causticBlockedPaths += blockedPath;
        if (luminance(shadowT) > 0.0) {
            vec3 bsdf = eval_brdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent);
            float bsdfPdf = pdf_brdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent);
            float envEffectivePdf = environment_light_sampling_pdf(envDir, bounce);
            if (envEffectivePdf <= 0.0) {
                envEffectivePdf = envPdf;
            }
            envEffectivePdf *= max(environmentTechniqueProbability, 1.0e-6);
            float weight = power_heuristic(envEffectivePdf, bsdfPdf);
#if RTV_REGIR_TRACE_ENABLED
            if (sampledEnvironmentBank) {
                regirEnvironmentSourceKind = envSourceKind;
                regirEnvironmentSourcePdf = envPdf;
                regirEnvironmentEffectivePdf = envEffectivePdf;
                regirEnvironmentDirection = envDir;
                regirEnvironmentMisWeight = weight;
                regirEnvironmentM = float(max(envReservoirSampleCount, 1u));
            }
#endif
            vec3 envSampleContribution = bsdf * envRadiance * shadowT * cosSurface * weight / max(envEffectivePdf, 1e-6);
            if (camera.restir_di_controls.x != 0u && camera.restir_di_controls.w != 0u) {
                vec3 envRestirContribution = envSampleContribution / float(envDirectSampleCount);
                float envRestirProxy = luminance(envRestirContribution);
                float envCandidateProxySum = sampledRestirProxy + envRestirProxy;
                if (envRestirProxy > 0.0 &&
                    rand_f32(rng) * max(envCandidateProxySum, 1.0e-8) <= envRestirProxy) {
                    sampledLightPdf = envEffectivePdf * float(envDirectSampleCount);
                    sampledRawLightPdf = envPdf;
                    sampledEffectiveLightPdf = sampledLightPdf;
                    sampledBsdfPdf = bsdfPdf;
                    sampledMisWeight = weight;
                    sampledType = 2u;
                    sampledLightIndex = RESTIR_DI_PSEUDO_LIGHT_INDEX;
                    sampledLightKind = RESTIR_DI_LIGHT_ENVIRONMENT;
                    sampledLightPosition = normalize(envDir);
                    sampledLightDirection = envDir;
                    sampledLightDistance = 10000.0;
                    sampledLightRadiance = envRadiance * shadowT;
                    sampledLightNormal = -normalize(envDir);
                    sampledRestirContribution = envRestirContribution;
                }
                sampledRestirProxy = envCandidateProxySum;
            }
            environmentContribution += envSampleContribution;
        }
    }
    if (envDirectSampleCount > 0u) {
        environmentContribution /= float(envDirectSampleCount);
    }

    vec3 sunContribution = vec3(0.0);
    if (sampleSunDirect) {
        vec3 sunWi;
        vec3 sunRadiance;
        float sunPdf;
#if RTV_REGIR_TRACE_ENABLED
        uint sunSourceKind = 0u;
        uint sunReservoirSampleCount = 0u;
        uint sunGenerationMismatch = 0u;
        bool sampledSunBank = false;
        float sunCanonicalProbability;
        float sunRegirProbability;
        regir_sun_sampling_probabilities(bounce, sunCanonicalProbability, sunRegirProbability);
        if (sunRegirProbability > 0.0 && rand_f32(rng) >= sunCanonicalProbability) {
            sampledSunBank = sample_regir_sun_direction(
                rng,
                sunWi,
                sunPdf,
                sunRadiance,
                sunSourceKind,
                sunReservoirSampleCount,
                sunGenerationMismatch);
        }
        regirEnvironmentGenerationMismatch = max(regirEnvironmentGenerationMismatch, sunGenerationMismatch);
        if (!sampledSunBank) {
            sunSourceKind = 0u;
            sunReservoirSampleCount = 0u;
        }
        bool sampledSun = sampledSunBank || sample_sun_light(rng, sunWi, sunRadiance, sunPdf);
#else
        bool sampledSun = sample_sun_light(rng, sunWi, sunRadiance, sunPdf);
#endif
        if (sampledSun) {
            float ndl = max(dot(lightingNormal, sunWi), 0.0);
            if (ndl <= 0.0) {
                return emissiveContribution + environmentContribution;
            }
            uint transmissiveHits;
            uint visiblePath;
            uint blockedPath;
            record_rt_counter(RT_DIAG_SUN_DIRECT_SHADOW_RAYS);
            vec3 shadowT = direct_shadow_transmittance_stats(
                shadow_origin(hit, sunWi),
                sunWi,
                10000.0,
                rayTime,
                material,
                transmissiveHits,
                visiblePath,
                blockedPath);
            causticTransmissiveHits += transmissiveHits;
            causticVisiblePaths += visiblePath;
            causticBlockedPaths += blockedPath;
            if (luminance(shadowT) > 0.0) {
                vec3 bsdf = eval_brdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent);
                float bsdfPdf = pdf_brdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent);
                float sunEffectivePdf = sun_light_sampling_pdf(sunWi, bounce);
                if (sunEffectivePdf <= 0.0) {
                    sunEffectivePdf = sunPdf;
                }
                sunEffectivePdf *= max(sunTechniqueProbability, 1.0e-6);
                float weight = power_heuristic(sunEffectivePdf, bsdfPdf);
                vec3 sunIncidentRadiance = sunRadiance * sun_transmittance(hit.world_pos, sunWi);
                vec3 sunCandidateContribution = bsdf * sunIncidentRadiance * shadowT * ndl * weight / max(sunEffectivePdf, 1e-6);
#if RTV_REGIR_TRACE_ENABLED
                if (sampledSunBank) {
                    regirEnvironmentSourceKind = sunSourceKind;
                    regirEnvironmentSourcePdf = sunPdf;
                    regirEnvironmentEffectivePdf = sunEffectivePdf;
                    regirEnvironmentDirection = sunWi;
                    regirEnvironmentMisWeight = weight;
                    regirEnvironmentM = float(max(sunReservoirSampleCount, 1u));
                }
#endif
                if (camera.restir_di_controls.x != 0u && camera.restir_di_controls.z != 0u) {
                    float sunRestirProxy = luminance(sunCandidateContribution);
                    float sunCandidateProxySum = sampledRestirProxy + sunRestirProxy;
                    if (sunRestirProxy > 0.0 &&
                        rand_f32(rng) * max(sunCandidateProxySum, 1.0e-8) <= sunRestirProxy) {
                        sampledLightPdf = sunEffectivePdf;
                        sampledRawLightPdf = sunPdf;
                        sampledEffectiveLightPdf = sunEffectivePdf;
                        sampledBsdfPdf = bsdfPdf;
                        sampledMisWeight = weight;
                        sampledType = 3u;
                        sampledLightIndex = RESTIR_DI_PSEUDO_LIGHT_INDEX;
                        sampledLightKind = RESTIR_DI_LIGHT_SUN;
                        sampledLightPosition = normalize(sunWi);
                        sampledLightDirection = sunWi;
                        sampledLightDistance = 10000.0;
                        sampledLightRadiance = sunIncidentRadiance * shadowT;
                        sampledLightNormal = -normalize(sunWi);
                        sampledRestirContribution = sunCandidateContribution;
                    }
                    sampledRestirProxy = sunCandidateProxySum;
                }
                sunContribution = sunCandidateContribution;
            }
        }
    }

    emissiveContribution *= secondaryDirectWeight;
    environmentContribution *= secondaryDirectWeight;
    sunContribution *= secondaryDirectWeight;
    sampledRestirContribution *= secondaryDirectWeight;
    sampledLightRadiance *= secondaryDirectWeight;
    return emissiveContribution + environmentContribution + sunContribution;
}

#if RTV_NATIVE2B_COMPACT_PRIMARY_LIGHTS
vec3 estimate_direct_lighting_env_sun_only(
    inout uint rng,
    RayPayload hit,
    Material material,
    vec3 wo,
    float rayTime,
    out vec3 environmentContribution,
    out float sampledLightPdf,
    out float sampledRawLightPdf,
    out float sampledEffectiveLightPdf,
    out float sampledBsdfPdf,
    out float sampledMisWeight,
    out uint sampledType,
    out vec3 sampledLightDirection,
    out float sampledLightDistance,
    out vec3 sampledLightRadiance,
    out vec3 sampledLightNormal,
    out vec3 sampledRestirContribution,
    out uint causticTransmissiveHits,
    out uint causticVisiblePaths,
    out uint causticBlockedPaths) {
    environmentContribution = vec3(0.0);
    sampledLightPdf = 0.0;
    sampledRawLightPdf = 0.0;
    sampledEffectiveLightPdf = 0.0;
    sampledBsdfPdf = 0.0;
    sampledMisWeight = 0.0;
    sampledType = 0u;
    sampledLightDirection = vec3(0.0, 1.0, 0.0);
    sampledLightDistance = 0.0;
    sampledLightRadiance = vec3(0.0);
    sampledLightNormal = vec3(0.0, 1.0, 0.0);
    sampledRestirContribution = vec3(0.0);
    causticTransmissiveHits = 0u;
    causticVisiblePaths = 0u;
    causticBlockedPaths = 0u;
    bool isDelta = material_is_delta(material);
    if (camera.direct_lighting_enabled == 0u || isDelta || renderer_debug_view() == 27u) {
        return vec3(0.0);
    }

    vec3 lightingNormal = normalize(hit.geom_normal);
    float sampledRestirProxy = 0.0;
    uint envSampleCount = clamp(camera.environment_direct_samples, 1u, 8u);
    for (uint envSample = 0u; envSample < envSampleCount; ++envSample) {
        vec3 envDir;
        float envPdf;
        vec3 envRadiance = sample_environment_direction(rng, envDir, envPdf);
        if (envPdf <= 0.0) {
            continue;
        }
        float cosSurface = max(dot(lightingNormal, envDir), 0.0);
        if (cosSurface <= 0.0) {
            continue;
        }
        uint transmissiveHits;
        uint visiblePath;
        uint blockedPath;
        record_rt_counter(RT_DIAG_ENV_DIRECT_SHADOW_RAYS);
        vec3 shadowT = direct_shadow_transmittance_stats(
            shadow_origin(hit, envDir),
            envDir,
            10000.0,
            rayTime,
            material,
            transmissiveHits,
            visiblePath,
            blockedPath);
        causticTransmissiveHits += transmissiveHits;
        causticVisiblePaths += visiblePath;
        causticBlockedPaths += blockedPath;
        if (luminance(shadowT) > 0.0) {
            vec3 bsdf = eval_brdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent);
            float bsdfPdf = pdf_brdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent);
            float weight = power_heuristic(envPdf, bsdfPdf);
            vec3 envSampleContribution = bsdf * envRadiance * shadowT * cosSurface * weight / max(envPdf, 1e-6);
            if (camera.restir_di_controls.x != 0u && camera.restir_di_controls.w != 0u) {
                vec3 envRestirContribution = envSampleContribution / float(envSampleCount);
                float envRestirProxy = luminance(envRestirContribution);
                float envCandidateProxySum = sampledRestirProxy + envRestirProxy;
                if (envRestirProxy > 0.0 &&
                    rand_f32(rng) * max(envCandidateProxySum, 1.0e-8) <= envRestirProxy) {
                    sampledLightPdf = envPdf * float(envSampleCount);
                    sampledRawLightPdf = envPdf;
                    sampledEffectiveLightPdf = sampledLightPdf;
                    sampledBsdfPdf = bsdfPdf;
                    sampledMisWeight = weight;
                    sampledType = 2u;
                    sampledLightDirection = envDir;
                    sampledLightDistance = 10000.0;
                    sampledLightRadiance = envRadiance * shadowT;
                    sampledLightNormal = -normalize(envDir);
                    sampledRestirContribution = envRestirContribution;
                }
                sampledRestirProxy = envCandidateProxySum;
            }
            environmentContribution += envSampleContribution;
        }
    }
    environmentContribution /= float(envSampleCount);

    vec3 sunContribution = vec3(0.0);
    vec3 sunWi;
    vec3 sunRadiance;
    float sunPdf;
    if (sample_sun_light(rng, sunWi, sunRadiance, sunPdf)) {
        float ndl = max(dot(lightingNormal, sunWi), 0.0);
        if (ndl <= 0.0) {
            return environmentContribution;
        }
        uint transmissiveHits;
        uint visiblePath;
        uint blockedPath;
        record_rt_counter(RT_DIAG_SUN_DIRECT_SHADOW_RAYS);
        vec3 shadowT = direct_shadow_transmittance_stats(
            shadow_origin(hit, sunWi),
            sunWi,
            10000.0,
            rayTime,
            material,
            transmissiveHits,
            visiblePath,
            blockedPath);
        causticTransmissiveHits += transmissiveHits;
        causticVisiblePaths += visiblePath;
        causticBlockedPaths += blockedPath;
        if (luminance(shadowT) > 0.0) {
            vec3 bsdf = eval_brdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent);
            float bsdfPdf = pdf_brdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent);
            float weight = power_heuristic(sunPdf, bsdfPdf);
            vec3 sunIncidentRadiance = sunRadiance * sun_transmittance(hit.world_pos, sunWi);
            vec3 sunCandidateContribution = bsdf * sunIncidentRadiance * shadowT * ndl * weight / max(sunPdf, 1e-6);
            if (camera.restir_di_controls.x != 0u && camera.restir_di_controls.z != 0u) {
                float sunRestirProxy = luminance(sunCandidateContribution);
                float sunCandidateProxySum = sampledRestirProxy + sunRestirProxy;
                if (sunRestirProxy > 0.0 &&
                    rand_f32(rng) * max(sunCandidateProxySum, 1.0e-8) <= sunRestirProxy) {
                    sampledLightPdf = sunPdf;
                    sampledRawLightPdf = sunPdf;
                    sampledEffectiveLightPdf = sunPdf;
                    sampledBsdfPdf = bsdfPdf;
                    sampledMisWeight = weight;
                    sampledType = 3u;
                    sampledLightDirection = sunWi;
                    sampledLightDistance = 10000.0;
                    sampledLightRadiance = sunIncidentRadiance * shadowT;
                    sampledLightNormal = -normalize(sunWi);
                    sampledRestirContribution = sunCandidateContribution;
                }
                sampledRestirProxy = sunCandidateProxySum;
            }
            sunContribution = sunCandidateContribution;
        }
    }

    return environmentContribution + sunContribution;
}
#endif

bool native2b_terminal_direct_fast_supported(Material material) {
    if (!native2b_kernel_enabled()) {
        return false;
    }

    bool supported = true;
    if ((camera.path_trace_controls.z & 8u) == 0u) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_FLAG_DISABLED);
        supported = false;
    }
    bool terminalCanSkipImportedEmissiveNee =
        compact_imported_emissive_direct_enabled() &&
        mesh_params.authored_light_count == 0u;
    if ((mesh_params.light_count != 0u || mesh_params.emissive_total_area > 1.0e-8) &&
        !terminalCanSkipImportedEmissiveNee) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_SCENE_LIGHTS);
        supported = false;
    }
    if (mesh_params.transmissive_shadow_caster_count != 0u) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_TRANSMISSIVE_SCENE);
    }
    if (homogeneous_volume_enabled()) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_VOLUME);
        supported = false;
    }
    if (camera.path_trace_controls.w != 0u || renderer_debug_view() == 27u) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_DEBUG);
        supported = false;
    }
    if (material_is_transmissive(material)) {
        record_rt_counter(RT_DIAG_TERMINAL_FAST_DIRECT_MATERIAL_TRANSMISSIVE);
        supported = false;
    }
    return supported;
}

bool native2b_terminal_direct_tile_selected(float probability) {
    if (probability >= 0.999) {
        return true;
    }
    if (probability <= 0.0) {
        return false;
    }

    // Raygen launches are X-major on the target NVIDIA path. Sharing one
    // decision across 32 adjacent pixels keeps the expensive shadow branch
    // coherent while the frame term rotates coverage temporally.
    uint warpTile = gl_LaunchIDEXT.x >> 5u;
    uint tileSeed = warpTile * 0x9e3779b9u;
    tileSeed ^= gl_LaunchIDEXT.y * 0x85ebca6bu;
    tileSeed ^= camera.temporal_frame_index * 0xc2b2ae35u;
    float selector = float(pcg_hash(tileSeed) >> 8u) * (1.0 / 16777216.0);
    return selector < probability;
}

bool native2b_terminal_simple_direct_brdf_supported(Material material) {
    if (material_is_transmissive(material)) {
        return false;
    }
    if (material.clearcoat_factor > 1.0e-5 ||
        dot(max(material.sheen_color, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)) > 1.0e-5) {
        return false;
    }
    return material.mat_type == 0u || material.mat_type == 1u || material.mat_type == 3u;
}

void native2b_terminal_direct_brdf_pdf(
    Material material,
    vec3 wo,
    vec3 wi,
    vec3 n,
    vec3 tangent,
    vec3 bitangent,
    out vec3 bsdf,
    out float bsdfPdf) {
    if (native2b_terminal_simple_direct_brdf_supported(material)) {
        if (material.mat_type == 0u) {
            bsdf = eval_diffuse_brdf(material, wo, wi, n);
            bsdfPdf = diffuse_pdf(n, wi);
        } else {
            bsdf = eval_ggx_brdf(material, wo, wi, n, tangent, bitangent);
            bsdfPdf = pdf_pbr_brdf(material, wo, wi, n, tangent, bitangent);
        }
        return;
    }

    bsdf = eval_brdf(material, wo, wi, n, tangent, bitangent);
    bsdfPdf = pdf_brdf(material, wo, wi, n, tangent, bitangent);
}

vec3 estimate_native2b_terminal_env_sun_direct(
    inout uint rng,
    RayPayload hit,
    Material material,
    vec3 wo,
    float rayTime,
    out vec3 environmentContribution,
    out uint sampledType) {
    environmentContribution = vec3(0.0);
    sampledType = 0u;
    if (camera.direct_lighting_enabled == 0u || material_is_delta(material) || renderer_debug_view() == 27u) {
        return vec3(0.0);
    }

    vec3 lightingNormal = normalize(hit.geom_normal);
    const float relaxedTerminalDirectProbability = clamp(camera.native2b_controls.x, 0.0, 1.0);
    const bool relaxedTerminalDirectSampling = relaxedTerminalDirectProbability < 0.999;
    const bool risOneSample = native2b_direct_reuse_mode() == NATIVE2B_DIRECT_REUSE_RIS;
    const bool environmentActive = env_params.enabled != 0u;
    const bool sunActive = camera.sunlight_enabled != 0u;
    if (relaxedTerminalDirectSampling &&
        !native2b_terminal_direct_tile_selected(relaxedTerminalDirectProbability)) {
        return vec3(0.0);
    }
    float environmentTechniqueProbability = 1.0;
    float sunTechniqueProbability = 1.0;
    bool sampleEnvironmentTechnique = environmentActive;
    bool sampleSunTechnique = sunActive;
    if (risOneSample) {
        if (environmentActive && sunActive) {
            environmentTechniqueProbability = 0.5;
            sunTechniqueProbability = 0.5;
            sampleEnvironmentTechnique = rand_f32(rng) < environmentTechniqueProbability;
            sampleSunTechnique = !sampleEnvironmentTechnique;
        } else {
            environmentTechniqueProbability = environmentActive ? 1.0 : 0.0;
            sunTechniqueProbability = sunActive ? 1.0 : 0.0;
        }
    }

    uint envSampleCount = clamp(camera.environment_direct_samples, 1u, 8u);
    uint effectiveEnvSampleCount = risOneSample ? 1u : envSampleCount;
    for (uint envSample = 0u; envSample < effectiveEnvSampleCount && sampleEnvironmentTechnique; ++envSample) {
        vec3 envDir;
        float envPdf;
        vec3 envRadiance = sample_environment_direction(rng, envDir, envPdf);
        if (envPdf <= 0.0) {
            continue;
        }
        float cosSurface = max(dot(lightingNormal, envDir), 0.0);
        if (cosSurface <= 0.0) {
            continue;
        }

        record_rt_counter(RT_DIAG_ENV_DIRECT_SHADOW_RAYS);
        bool blocked = trace_shadow(shadow_origin(hit, envDir), envDir, 10000.0, rayTime);
        if (blocked) {
            continue;
        }

        vec3 bsdf;
        float bsdfPdf;
        native2b_terminal_direct_brdf_pdf(material, wo, envDir, lightingNormal, hit.tangent, hit.bitangent, bsdf, bsdfPdf);
        float techniqueProbability = risOneSample ? environmentTechniqueProbability : 1.0;
        float samplingProbability = relaxedTerminalDirectSampling ? relaxedTerminalDirectProbability : 1.0;
        float effectiveEnvPdf = envPdf * techniqueProbability * samplingProbability;
        float weight = power_heuristic(effectiveEnvPdf, bsdfPdf);
        if (sampledType == 0u) {
            sampledType = 2u;
        }
        environmentContribution += bsdf * envRadiance * cosSurface * weight /
            max(effectiveEnvPdf, 1e-6);
    }
    environmentContribution /= float(effectiveEnvSampleCount);

    vec3 sunContribution = vec3(0.0);
    vec3 sunWi;
    vec3 sunRadiance;
    float sunPdf;
    if (sampleSunTechnique && sample_sun_light(rng, sunWi, sunRadiance, sunPdf)) {
        float ndl = max(dot(lightingNormal, sunWi), 0.0);
        if (ndl > 0.0) {
            record_rt_counter(RT_DIAG_SUN_DIRECT_SHADOW_RAYS);
            bool blocked = trace_shadow(shadow_origin(hit, sunWi), sunWi, 10000.0, rayTime);
            if (!blocked) {
                vec3 bsdf;
                float bsdfPdf;
                native2b_terminal_direct_brdf_pdf(material, wo, sunWi, lightingNormal, hit.tangent, hit.bitangent, bsdf, bsdfPdf);
                float techniqueProbability = risOneSample ? sunTechniqueProbability : 1.0;
                float samplingProbability = relaxedTerminalDirectSampling ? relaxedTerminalDirectProbability : 1.0;
                float effectiveSunPdf = sunPdf * techniqueProbability * samplingProbability;
                float weight = power_heuristic(effectiveSunPdf, bsdfPdf);
                if (sampledType == 0u) {
                    sampledType = 3u;
                }
                sunContribution = bsdf * sunRadiance * sun_transmittance(hit.world_pos, sunWi) * ndl * weight /
                    max(effectiveSunPdf, 1e-6);
            }
        }
    }

    return environmentContribution + sunContribution;
}

float restir_di_light_pdf(uint lightIndex, uint lightKind, vec3 receiverPos, vec3 samplePosOrDir, vec3 lightNormal, float distanceToLight) {
    if (lightIndex >= mesh_params.light_count || mesh_params.emissive_total_area <= 1.0e-8) {
        return 0.0;
    }
    LightRecord light = light_records[lightIndex];
    if (light.metadata.x != lightKind) {
        return 0.0;
    }
    float selectionPdf = light_record_selection_pdf(lightIndex);
    if (lightKind == 2u || lightKind == 3u || lightKind == 5u) {
        return selectionPdf;
    }

    vec3 toLight = samplePosOrDir - receiverPos;
    float distSq = dot(toLight, toLight);
    vec3 wi = toLight * inversesqrt(max(distSq, 1.0e-8));
    float cosLight = max(dot(-wi, normalize(lightNormal)), 0.0);
    float area = max(light.data0.w, 1.0e-6);
    return cosLight > 1.0e-6 ? selectionPdf * distSq / (cosLight * area) : 0.0;
}

RestirReservoir invalidate_restir_di_reservoir(RestirReservoir reservoir) {
    reservoir.metadata.x = 0u;
    restir_set_validity_visibility(reservoir, restir_pack_validity_visibility(false, RESTIR_VISIBILITY_INVALID));
    reservoir.sample_value_confidence = vec4(0.0);
    reservoir.sample_radiance_target.w = 0.0;
    reservoir.sample_normal_weight.w = 0.0;
    restir_set_source_pdf_and_previous_weight(reservoir, 1.0e-6, restir_previous_weight(reservoir));
    return reservoir;
}

RestirReservoir reevaluate_restir_di_reservoir(
    RestirReservoir reservoir,
    RayPayload hit,
    Material material,
    vec3 wo,
    vec3 throughput,
    vec3 atmosTransmittance,
    float rayTime,
    out uint causticTransmissiveHits,
    out uint causticVisiblePaths,
    out uint causticBlockedPaths) {
    causticTransmissiveHits = 0u;
    causticVisiblePaths = 0u;
    causticBlockedPaths = 0u;
    if (!restir_reservoir_valid(reservoir) || reservoir.metadata.x != 1u) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    uint lightIndex = reservoir.sample_metadata.x;
    uint lightKind = reservoir.sample_metadata.y;
    if (lightIndex >= mesh_params.light_count) {
        return invalidate_restir_di_reservoir(reservoir);
    }
    LightRecord selectedLight = light_records[lightIndex];
    if (selectedLight.metadata.x != lightKind) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    vec3 lightingNormal = normalize(hit.geom_normal);
    vec3 wi;
    float distanceToLight;
    vec3 incidentRadiance = max(reservoir.sample_radiance_target.rgb, vec3(0.0));
    vec3 lightNormal = normalize(reservoir.sample_normal_weight.xyz);
    if (lightKind == 2u) {
        wi = normalize(reservoir.sample_position_distance.xyz);
        distanceToLight = 10000.0;
    } else {
        vec3 toLight = reservoir.sample_position_distance.xyz - hit.world_pos;
        float distSq = dot(toLight, toLight);
        distanceToLight = sqrt(max(distSq, 1.0e-8));
        wi = toLight / distanceToLight;
        if (lightKind == 3u) {
            incidentRadiance /= max(distSq, 1.0e-4);
            lightNormal = -wi;
        } else if (lightKind == 5u) {
            vec3 spotDirection = normalize(vec3(selectedLight.data1.w, selectedLight.data2.w, selectedLight.data3.x));
            float innerCone = clamp(selectedLight.data3.y, 0.0, PI);
            float outerCone = clamp(max(selectedLight.data3.z, innerCone), 0.0, PI);
            float cosInner = cos(innerCone);
            float cosOuter = cos(outerCone);
            float cosTheta = dot(-wi, spotDirection);
            float coneWeight = clamp((cosTheta - cosOuter) / max(cosInner - cosOuter, 1.0e-4), 0.0, 1.0);
            coneWeight *= coneWeight;
            incidentRadiance = selectedLight.data2.xyz * coneWeight / max(distSq, 1.0e-4);
            lightNormal = spotDirection;
        }
    }

    float cosSurface = max(dot(lightingNormal, wi), 0.0);
    float lightPdf = restir_di_light_pdf(lightIndex, lightKind, hit.world_pos, reservoir.sample_position_distance.xyz, lightNormal, distanceToLight);
    if (cosSurface <= 0.0 || lightPdf <= 1.0e-6 || !has_positive_radiance(incidentRadiance)) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    float shadowDistance = lightKind == 2u ? 10000.0 : max(distanceToLight - shadow_distance_bias(), 0.0);
    vec3 shadowT = caustic_shadow_transmittance_stats(
        shadow_origin(hit, wi),
        wi,
        shadowDistance,
        rayTime,
        causticTransmissiveHits,
        causticVisiblePaths,
        causticBlockedPaths);
    if (luminance(shadowT) <= 0.0) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    vec3 bsdf = eval_brdf(material, wo, wi, lightingNormal, hit.tangent, hit.bitangent);
    float bsdfPdf = pdf_brdf(material, wo, wi, lightingNormal, hit.tangent, hit.bitangent);
    float misWeight = power_heuristic(lightPdf, bsdfPdf);
    vec3 contribution = throughput * bsdf * incidentRadiance * shadowT * cosSurface * misWeight / max(lightPdf, 1.0e-6);
    contribution *= atmosTransmittance;
    contribution = clamp_luminance_preserve_hue(contribution, direct_contribution_luminance_limit(0u, 1u, material.roughness));
    float targetLum = luminance(contribution);
    if (targetLum <= 1.0e-6) {
        return invalidate_restir_di_reservoir(reservoir);
    }

    reservoir.sample_value_confidence.rgb = contribution;
    reservoir.sample_value_confidence.a = clamp(reservoir.sample_value_confidence.a, 0.0, 1.0);
    reservoir.sample_position_distance.w = distanceToLight;
    reservoir.sample_radiance_target.w = targetLum;
    reservoir.sample_normal_weight.xyz = lightNormal;
    reservoir.sample_normal_weight.w = max(targetLum / max(lightPdf, 1.0e-6), 1.0e-6);
    restir_set_source_pdf_and_previous_weight(reservoir, lightPdf, restir_previous_weight(reservoir));
    restir_set_validity_visibility(reservoir, restir_pack_validity_visibility(true, RESTIR_VISIBILITY_VISIBLE));
    return reservoir;
}

RestirReservoir restir_temporal_resample(RestirReservoir current, RestirReservoir previous, float motionConfidence, inout uint rng) {
    bool currentValid = restir_reservoir_valid(current);
    bool previousValid = restir_reservoir_valid(previous);
    if (!currentValid && !previousValid) {
        return current;
    }
    if (!previousValid) {
        restir_set_source_pdf_and_previous_weight(current, restir_source_pdf(current), 0.0);
        restir_set_age(current, 0u);
        return current;
    }
    if (!currentValid) {
        float reuseConfidence = clamp(previous.sample_value_confidence.a, 0.0, 1.0) *
            clamp(motionConfidence, 0.0, 1.0) *
            restir_age_confidence(previous, 32.0);
        if (reuseConfidence <= 0.20) {
            return current;
        }
        previous.sample_value_confidence.a = reuseConfidence;
        restir_set_age(previous, min(restir_age(previous) + 1u, 255u));
        restir_set_source_pdf_and_previous_weight(previous, restir_source_pdf(previous), 1.0);
        return previous;
    }

    float currentMass = max(restir_weight_sum(current), restir_target_function(current));
    float previousMass = max(restir_weight_sum(previous), restir_target_function(previous)) *
        clamp(previous.sample_value_confidence.a, 0.0, 1.0) *
        clamp(motionConfidence, 0.0, 1.0) *
        restir_age_confidence(previous, 32.0);
    float totalMass = currentMass + previousMass;
    float previousWeight = totalMass > 1.0e-8 ? previousMass / totalMass : 0.0;
    float motionCap = mix(0.85, 0.10, 1.0 - clamp(motionConfidence, 0.0, 1.0));
    previousWeight = clamp(previousWeight, 0.0, motionCap);

    bool selectPrevious = rand_f32(rng) < previousWeight;
    RestirReservoir selected = selectPrevious ? previous : current;
    selected.sample_normal_weight.w = min(totalMass, 65504.0);
    selected.sample_value_confidence.a = clamp(selected.sample_value_confidence.a * clamp(motionConfidence, 0.0, 1.0), 0.0, 1.0);
    restir_set_age(selected, selectPrevious ? min(restir_age(previous) + 1u, 255u) : 0u);
    restir_set_sample_count(selected, min(restir_sample_count(current) + restir_sample_count(previous), 64.0));
    restir_set_source_pdf_and_previous_weight(selected, restir_source_pdf(selected), previousWeight);
    restir_set_validity_visibility(selected, restir_pack_validity_visibility(restir_reservoir_valid(selected), RESTIR_VISIBILITY_VISIBLE));
    return selected;
}

vec3 fast_preview_ambient(Material material, vec3 normal) {
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 sampleDir = normalize(normal + up * 0.45);
    float hemisphere = 0.35 + 0.65 * max(dot(normal, up), 0.0);
    return material.color * environment_radiance(sampleDir, ATMOSPHERE_RAY_QUALITY_FAST) * (0.16 + 0.24 * hemisphere);
}

vec3 estimate_volume_direct_lighting(inout uint rng, vec3 scatterPos, vec3 incidentDir, float rayTime) {
    if (camera.direct_lighting_enabled == 0u || !homogeneous_volume_enabled()) {
        return vec3(0.0);
    }

    float g = homogeneous_anisotropy();
    vec3 result = vec3(0.0);
    uint envSampleCount = clamp(camera.environment_direct_samples, 1u, 8u);
    for (uint envSample = 0u; envSample < envSampleCount; ++envSample) {
        vec3 envDir;
        float envPdf;
        vec3 envRadiance = sample_environment_direction(rng, envDir, envPdf);
        if (envPdf <= 1.0e-6) {
            continue;
        }
        vec3 shadowT = caustic_shadow_transmittance(scatterPos, envDir, 10000.0, rayTime);
        if (luminance(shadowT) > 0.0) {
            float phase = henyey_greenstein_phase(dot(normalize(incidentDir), envDir), g);
            result += envRadiance * shadowT * phase / max(envPdf, 1.0e-6);
        }
    }
    result /= float(envSampleCount);

    vec3 sunWi;
    vec3 sunRadiance;
    float sunPdf;
    if (camera.sunlight_enabled != 0u && sample_sun_light(rng, sunWi, sunRadiance, sunPdf) && sunPdf > 1.0e-6) {
        vec3 shadowT = caustic_shadow_transmittance(scatterPos, sunWi, 10000.0, rayTime);
        if (luminance(shadowT) > 0.0) {
            float phase = henyey_greenstein_phase(dot(normalize(incidentDir), sunWi), g);
            result += sunRadiance * sun_transmittance(scatterPos, sunWi) * shadowT * phase / max(sunPdf, 1.0e-6);
        }
    }
    return result;
}


#endif // RTV_PATHTRACE_LIGHTING_GLSL
