#include "rtv/GraphicsPipeline.h"

#include "rtv/Check.h"
#include "rtv/PipelineBuilder.h"
#include "rtv/PipelineCache.h"
#include "rtv/ShaderModule.h"

namespace rtv {

GraphicsPipeline::GraphicsPipeline(
    VkDevice device,
    VkFormat colorFormat,
    const ShaderModule& vertexShader,
    const ShaderModule& fragmentShader,
    std::vector<VkDescriptorSetLayout> setLayouts,
    std::vector<VkPushConstantRange> pushConstants,
    PipelineCache& pipelineCache)
    : device_(device) {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
    layoutInfo.pPushConstantRanges = pushConstants.data();
    checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_), "vkCreatePipelineLayout(graphics)");

    PipelineBuilder builder;
    VkGraphicsPipelineCreateInfo createInfo = builder
        .setShaders(vertexShader.stageInfo(), fragmentShader.stageInfo())
        .setColorFormat(colorFormat)
        .setPipelineLayout(layout_)
        .buildCreateInfo();

    checkVk(vkCreateGraphicsPipelines(device_, pipelineCache.handle(), 1, &createInfo, nullptr, &pipeline_), "vkCreateGraphicsPipelines");
}

GraphicsPipeline::~GraphicsPipeline() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
}

void GraphicsPipeline::bind(VkCommandBuffer commandBuffer, VkExtent2D extent) const {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

} // namespace rtv
