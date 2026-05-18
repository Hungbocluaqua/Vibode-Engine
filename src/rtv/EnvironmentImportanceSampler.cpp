#include "rtv/EnvironmentImportanceSampler.h"

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

[[nodiscard]] float luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

[[nodiscard]] uint16_t float32ToHalf(float value) {
    if (!std::isfinite(value) || value <= 0.0f) {
        return 0;
    }
    value = std::min(value, 65504.0f);
    union {
        float f;
        uint32_t u;
    } v{value};
    const uint32_t sign = (v.u >> 16u) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((v.u >> 23u) & 0xffu) - 127 + 15;
    uint32_t fraction = (v.u >> 13u) & 0x03ffu;
    if (exponent <= 0) {
        fraction = ((v.u & 0x007fffffu) | 0x00800000u) >> static_cast<uint32_t>(1 - exponent + 13);
        exponent = 0;
    } else if (exponent >= 31) {
        exponent = 31;
        fraction = 0;
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10u) | fraction);
}

} // namespace

EnvironmentImportanceData EnvironmentImportanceSampler::build(const float* rgba, uint32_t width, uint32_t height) {
    EnvironmentImportanceData result;
    result.rowCdf.resize(height);
    result.columnCdf.resize(static_cast<size_t>(width) * height);

    std::vector<float> weightedLuminance(static_cast<size_t>(width) * height);
    float total = 0.0f;
    for (uint32_t y = 0; y < height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
        const float lat = (v - 0.5f) * 3.1415926535f;
        const float sinTheta = std::max(std::cos(lat), 0.001f);
        for (uint32_t x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(y) * width + x;
            const size_t rgbaIdx = idx * 4u;
            const float lum = luminance(rgba[rgbaIdx + 0], rgba[rgbaIdx + 1], rgba[rgbaIdx + 2]) * sinTheta;
            weightedLuminance[idx] = lum;
            total += lum;
        }
    }

    result.invTotalLuminance = total > 1.0e-10f ? 1.0f / total : 0.0f;
    float rowAccum = 0.0f;
    for (uint32_t y = 0; y < height; ++y) {
        float rowTotal = 0.0f;
        for (uint32_t x = 0; x < width; ++x) {
            rowTotal += weightedLuminance[static_cast<size_t>(y) * width + x];
        }
        rowAccum += rowTotal * result.invTotalLuminance;
        result.rowCdf[y] = total > 1.0e-10f ? rowAccum : (static_cast<float>(y) + 1.0f) / static_cast<float>(height);

        float colAccum = 0.0f;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(y) * width + x;
            colAccum += rowTotal > 1.0e-10f ? weightedLuminance[idx] / rowTotal : 1.0f / static_cast<float>(width);
            result.columnCdf[idx] = colAccum;
        }
        result.columnCdf[static_cast<size_t>(y) * width + width - 1u] = 1.0f;
    }
    if (!result.rowCdf.empty()) {
        result.rowCdf.back() = 1.0f;
    }
    return result;
}

std::vector<uint16_t> rgba32fToRgba16f(const std::vector<float>& rgba) {
    std::vector<uint16_t> result(rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i) {
        result[i] = float32ToHalf(rgba[i]);
    }
    return result;
}

} // namespace rtv
