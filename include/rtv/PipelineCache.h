#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace rtv {

class PipelineCache final : private NonCopyable {
public:
    explicit PipelineCache(VkDevice device);
    PipelineCache(VkDevice device, const std::filesystem::path& cachePath);
    ~PipelineCache();

    [[nodiscard]] VkPipelineCache handle() const { return cache_; }
    bool saveToFile(const std::filesystem::path& path) const;
    [[nodiscard]] static std::vector<uint8_t> loadFromFile(const std::filesystem::path& path);

private:
    [[nodiscard]] static std::vector<uint8_t> readCacheFile(const std::filesystem::path& path);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
};

} // namespace rtv
