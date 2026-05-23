#ifndef WAVEFRONT_COMMON_GLSL
#define WAVEFRONT_COMMON_GLSL

struct WavefrontRay {
    vec4 origin_tMin;
    vec4 direction_tMax;
    vec4 throughput;
    uint pixelIndex;
    uint bounce;
    uint seed;
    uint rayFlags;
};

struct WavefrontHit {
    vec4 position_hitT;
    vec4 normal_frontFace;
    vec4 albedo_roughness;
    uint pixelIndex;
    uint geometryIndex;
    uint primitiveIndex;
    uint instanceIndex;
    uint bounce;
    uint seed;
    uint materialType;
    uint padding;
};

#define RAY_QUEUE_CAPACITY (1920 * 1080 * 4)
#define HIT_QUEUE_CAPACITY (1920 * 1080 * 2)
#define SHADOW_QUEUE_CAPACITY (1920 * 1080)

#endif // WAVEFRONT_COMMON_GLSL
