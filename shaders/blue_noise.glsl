#ifndef BLUE_NOISE_GLSL
#define BLUE_NOISE_GLSL

#ifndef RTV_USE_DIMENSIONED_SAMPLER
#define RTV_USE_DIMENSIONED_SAMPLER 1
#endif

uint blue_noise_hash(uint x, uint y) {
    uint h = x * 0xbc9f1d37u + y * 0x9e3779b9u;
    h = h ^ (h >> 16u);
    h *= 0x85ebca6bu;
    h = h ^ (h >> 13u);
    h *= 0xc2b2ae35u;
    h = h ^ (h >> 16u);
    return h;
}

float blue_noise_samples[4][4] = float[4][4](
    float[](0.0625, 0.5625, 0.3125, 0.8125),
    float[](0.1875, 0.6875, 0.4375, 0.9375),
    float[](0.8125, 0.3125, 0.5625, 0.0625),
    float[](0.9375, 0.4375, 0.6875, 0.1875)
);

float blue_noise_1d(ivec2 pixel, uint frameIndex) {
    uint x = uint(pixel.x) & 3u;
    uint y = uint(pixel.y) & 3u;
    float base = blue_noise_samples[y][x];
    float rot = float(frameIndex & 3u) * 0.25;
    return fract(base + rot);
}

vec2 blue_noise_2d(ivec2 pixel, uint frameIndex) {
    float u = blue_noise_1d(pixel, frameIndex);
    float v = blue_noise_1d(pixel + ivec2(7, 11), frameIndex + 1u);
    return vec2(u, v);
}

const uint SAMPLE_DIM_PATH_SEED = 0u;
const uint SAMPLE_DIM_LIGHT_SELECT = 8u;
const uint SAMPLE_DIM_LIGHT_SURFACE = 12u;
const uint SAMPLE_DIM_ENVIRONMENT = 20u;
const uint SAMPLE_DIM_SUN = 24u;
const uint SAMPLE_DIM_BSDF = 32u;
const uint SAMPLE_DIM_DIELECTRIC = 40u;
const uint SAMPLE_DIM_RUSSIAN_ROULETTE = 44u;
const uint SAMPLE_DIM_RESTIR_CANDIDATE = 52u;
const uint SAMPLE_DIM_RESTIR_SPATIAL = 60u;
const uint SAMPLE_DIM_DEBUG = 96u;

uint sample_hash_combine(uint a, uint b) {
    return blue_noise_hash(a ^ (b + 0x9e3779b9u + (a << 6u) + (a >> 2u)), b);
}

float sample_uint_to_unit_float(uint value) {
    return float(value >> 8u) * (1.0 / 16777216.0);
}

float sample_radical_inverse_base2(uint value) {
    value = bitfieldReverse(value);
    return float(value) * 2.3283064365386963e-10;
}

float sample_radical_inverse_base3(uint value) {
    float invBase = 1.0 / 3.0;
    float reversed = 0.0;
    float factor = invBase;
    uint v = value;
    while (v > 0u) {
        reversed += float(v % 3u) * factor;
        v /= 3u;
        factor *= invBase;
    }
    return reversed;
}

uint sample_dimension_seed(ivec2 pixel, uint frameIndex, uint bounce, uint dimension) {
    uint x = uint(pixel.x);
    uint y = uint(pixel.y);
    uint seed = sample_hash_combine(x + 0x632be59bu, y + 0x85157af5u);
    seed = sample_hash_combine(seed, frameIndex * 0x9e3779b9u);
    seed = sample_hash_combine(seed, bounce * 0x85ebca6bu);
    return sample_hash_combine(seed, dimension * 0xc2b2ae35u);
}

float sample_dimension_1d(ivec2 pixel, uint frameIndex, uint bounce, uint dimension) {
    uint seed = sample_dimension_seed(pixel, frameIndex, bounce, dimension);
#if RTV_USE_DIMENSIONED_SAMPLER
    uint index = frameIndex + 1u + bounce * 4099u + dimension * 131u;
    float lowDiscrepancy = sample_radical_inverse_base2(index ^ (seed & 0xffffu));
    float scramble = sample_uint_to_unit_float(sample_hash_combine(seed, dimension));
    float stbnRotation = blue_noise_1d(pixel + ivec2(int(dimension & 7u), int((dimension >> 3u) & 7u)), frameIndex + bounce + dimension);
    return fract(lowDiscrepancy + scramble + stbnRotation * (1.0 / 256.0));
#else
    float hashValue = sample_uint_to_unit_float(seed);
    float stbnRotation = blue_noise_1d(pixel + ivec2(int(dimension & 7u), int((dimension >> 3u) & 7u)), frameIndex + bounce + dimension);
    return fract(hashValue + stbnRotation * (1.0 / 256.0));
#endif
}

vec2 sample_dimension_2d(ivec2 pixel, uint frameIndex, uint bounce, uint dimension) {
    uint seed = sample_dimension_seed(pixel, frameIndex, bounce, dimension);
#if RTV_USE_DIMENSIONED_SAMPLER
    uint index = frameIndex + 1u + bounce * 4099u + dimension * 131u;
    float u = sample_radical_inverse_base2(index ^ (seed & 0xffffu));
    float v = sample_radical_inverse_base3(index ^ ((seed >> 16u) & 0xffffu));
    vec2 scramble = vec2(
        sample_uint_to_unit_float(sample_hash_combine(seed, dimension + 1u)),
        sample_uint_to_unit_float(sample_hash_combine(seed, dimension + 2u)));
    vec2 stbnRotation = blue_noise_2d(pixel + ivec2(int(dimension & 7u), int((dimension >> 3u) & 7u)), frameIndex + bounce + dimension);
    return fract(vec2(u, v) + scramble + stbnRotation * (1.0 / 256.0));
#else
    vec2 hashValue = vec2(
        sample_uint_to_unit_float(seed),
        sample_uint_to_unit_float(sample_hash_combine(seed, dimension + 1u)));
    vec2 stbnRotation = blue_noise_2d(pixel + ivec2(int(dimension & 7u), int((dimension >> 3u) & 7u)), frameIndex + bounce + dimension);
    return fract(hashValue + stbnRotation * (1.0 / 256.0));
#endif
}

#endif
