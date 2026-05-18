#pragma once

#include <cstdint>
#include <vector>

namespace rtv {

struct EnvironmentImportanceData {
    std::vector<float> rowCdf;
    std::vector<float> columnCdf;
    float invTotalLuminance = 0.0f;
};

class EnvironmentImportanceSampler {
public:
    [[nodiscard]] static EnvironmentImportanceData build(const float* rgba, uint32_t width, uint32_t height);
};

[[nodiscard]] std::vector<uint16_t> rgba32fToRgba16f(const std::vector<float>& rgba);

} // namespace rtv
