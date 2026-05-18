#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <vector>

namespace rtv {

class PipelineCache;
class ShaderModule;

class GraphicsPipeline final : private NonCopyable {
public:
    GraphicsPipeline(
        VkDevice device,
        VkFormat colorFormat,
        const ShaderModule& vertexShader,
        const ShaderModule& fragmentShader,
        std::vector<VkDescriptorSetLayout> setLayouts,
        std::vector<VkPushConstantRange> pushConstants,
        PipelineCache& pipelineCache);
    ~GraphicsPipeline();

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

    void bind(VkCommandBuffer commandBuffer, VkExtent2D extent) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace rtv
