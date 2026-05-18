#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace rtv {

struct HdrImageData {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgba;
};

class HdrLoader {
public:
    [[nodiscard]] static HdrImageData loadRadiance(const std::filesystem::path& path);
    [[nodiscard]] static HdrImageData createProcedural(uint32_t width, uint32_t height);
};

} // namespace rtv
