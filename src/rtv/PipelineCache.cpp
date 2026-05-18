#include "rtv/PipelineCache.h"

#include "rtv/Check.h"

namespace rtv {

PipelineCache::PipelineCache(VkDevice device)
    : device_(device) {
    VkPipelineCacheCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    checkVk(vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_), "vkCreatePipelineCache");
}

PipelineCache::~PipelineCache() {
    if (cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, cache_, nullptr);
    }
}

} // namespace rtv
