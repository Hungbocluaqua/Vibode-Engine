#include "rtv/EnvironmentImportanceSampler.h"

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

[[nodiscard]] float luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

[[nodiscard]] std::vector<glm::vec2> buildAliasTable(const std::vector<float>& weights) {
    const uint32_t count = static_cast<uint32_t>(weights.size());
    std::vector<glm::vec2> table(count, glm::vec2(1.0f, 0.0f));
    if (count == 0) {
        return table;
    }

    float total = 0.0f;
    for (float weight : weights) {
        total += std::max(weight, 0.0f);
    }
    if (total <= 1.0e-10f) {
        for (uint32_t i = 0; i < count; ++i) {
            table[i] = glm::vec2(1.0f, static_cast<float>(i));
        }
        return table;
    }

    std::vector<float> scaled(count);
    std::vector<uint32_t> small;
    std::vector<uint32_t> large;
    small.reserve(count);
    large.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        scaled[i] = std::max(weights[i], 0.0f) * static_cast<float>(count) / total;
        if (scaled[i] < 1.0f) {
            small.push_back(i);
        } else {
            large.push_back(i);
        }
    }

    while (!small.empty() && !large.empty()) {
        const uint32_t s = small.back();
        small.pop_back();
        const uint32_t l = large.back();
        table[s] = glm::vec2(scaled[s], static_cast<float>(l));
        scaled[l] = (scaled[l] + scaled[s]) - 1.0f;
        if (scaled[l] < 1.0f) {
            large.pop_back();
            small.push_back(l);
        }
    }
    for (uint32_t index : large) {
        table[index] = glm::vec2(1.0f, static_cast<float>(index));
    }
    for (uint32_t index : small) {
        table[index] = glm::vec2(1.0f, static_cast<float>(index));
    }
    return table;
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
    result.rowAlias.resize(height);
    result.columnAlias.resize(static_cast<size_t>(width) * height);

    std::vector<float> weightedLuminance(static_cast<size_t>(width) * height);
    std::vector<float> rowWeights(height);
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
            rowWeights[y] += lum;
        }
    }

    result.invTotalLuminance = total > 1.0e-10f ? 1.0f / total : 0.0f;
    result.rowAlias = buildAliasTable(rowWeights);
    for (uint32_t y = 0; y < height; ++y) {
        std::vector<float> columnWeights(width);
        for (uint32_t x = 0; x < width; ++x) {
            columnWeights[x] = weightedLuminance[static_cast<size_t>(y) * width + x];
        }
        std::vector<glm::vec2> rowAlias = buildAliasTable(columnWeights);
        for (uint32_t x = 0; x < width; ++x) {
            result.columnAlias[static_cast<size_t>(y) * width + x] = rowAlias[x];
        }
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
