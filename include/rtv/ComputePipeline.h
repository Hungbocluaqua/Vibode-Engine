#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <vector>

namespace rtv {

class PipelineCache;
class ShaderModule;

class ComputePipeline final : private NonCopyable {
public:
    ComputePipeline(
        VkDevice device,
        const ShaderModule& shader,
        std::vector<VkDescriptorSetLayout> setLayouts,
        std::vector<VkPushConstantRange> pushConstants,
        PipelineCache& pipelineCache);
    ~ComputePipeline();

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

    void bind(VkCommandBuffer commandBuffer) const;
    void dispatch(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height, uint32_t localX = 16, uint32_t localY = 16) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace rtv
