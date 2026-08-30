#ifndef RTV_RT_RESTIR_DI_BINDINGS_GLSL
#define RTV_RT_RESTIR_DI_BINDINGS_GLSL

// Ray-tracing ReSTIR DI buffers and parameter block; bindings must remain unchanged.
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
    uint mode;
    uint spatialResultValid;
    uint visibilityRayBudget;
    uint historyValid;
    uint materialVisibilityFlags;
    uint counterEnabled;
    uint rawOutputIsCurrentSample;
    float shadowDistanceBias;
    uint lightVersion;
    uint environmentVersion;
    uint rtxdiPtEnabled;
    uint rtxdiPtReplayEnabled;
    uint rtxdiPtMaxReservoirAge;
    uint rtxdiPtMaxBounceDepth;
    uint rtxdiPtMaxRcVertexLength;
    uint rtxdiCheckerboardField;
    uint rtxdiDiLocalLightSamples;
    uint rtxdiDiBrdfSamples;
    uint rtxdiDiInfiniteLightSamples;
    uint rtxdiDiEnvironmentSamples;
} restir_di_raygen_params;

uint restir_di_reservoir_width(uint fullWidth) {
    return restir_di_raygen_params.rtxdiCheckerboardField != 0u
        ? (fullWidth + 1u) / 2u
        : fullWidth;
}

bool restir_di_checkerboard_pixel_active(ivec2 pixel) {
    if (restir_di_raygen_params.rtxdiCheckerboardField == 0u) {
        return true;
    }
    return (uint(pixel.x) & 1u) ==
        ((uint(pixel.y) + restir_di_raygen_params.rtxdiCheckerboardField) & 1u);
}

uint restir_di_reservoir_index(ivec2 pixel, uint fullWidth) {
    if (restir_di_raygen_params.rtxdiCheckerboardField == 0u) {
        const uint reservoirWidth = restir_di_reservoir_width(fullWidth);
        return uint(pixel.y) * reservoirWidth + uint(pixel.x);
    }
    const uint blockSize = 16u;
    const uint reservoirWidth = restir_di_reservoir_width(fullWidth);
    const uint widthBlocks = (reservoirWidth + blockSize - 1u) / blockSize;
    const uint blockRowPitch = widthBlocks * blockSize * blockSize;
    const uint reservoirX = uint(pixel.x) >> 1u;
    const uint reservoirY = uint(pixel.y);
    const uint blockX = reservoirX / blockSize;
    const uint blockY = reservoirY / blockSize;
    const uint inBlockX = reservoirX % blockSize;
    const uint inBlockY = reservoirY % blockSize;
    return blockY * blockRowPitch +
        blockX * blockSize * blockSize + inBlockY * blockSize + inBlockX;
}


#endif // RTV_RT_RESTIR_DI_BINDINGS_GLSL
