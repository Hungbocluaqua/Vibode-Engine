#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

namespace rtv {

class PipelineCache final : private NonCopyable {
public:
    explicit PipelineCache(VkDevice device);
    ~PipelineCache();

    [[nodiscard]] VkPipelineCache handle() const { return cache_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
};

} // namespace rtv
