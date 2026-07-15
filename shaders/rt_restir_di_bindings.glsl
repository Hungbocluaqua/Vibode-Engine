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
} restir_di_raygen_params;


#endif // RTV_RT_RESTIR_DI_BINDINGS_GLSL
