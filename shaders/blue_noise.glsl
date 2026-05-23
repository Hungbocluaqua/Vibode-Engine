#ifndef BLUE_NOISE_GLSL
#define BLUE_NOISE_GLSL

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

#endif
