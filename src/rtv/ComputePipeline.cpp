#include "rtv/ComputePipeline.h"

#include "rtv/Check.h"
#include "rtv/PipelineCache.h"
#include "rtv/ShaderModule.h"

#include <chrono>
#include <iostream>
#include <string>

namespace rtv {

namespace {

template <typename Fn>
void timePipelineCreate(const char* kind, const std::string& label, Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    const auto end = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(end - start).count();
    if (ms >= 25.0) {
        std::cout << "Pipeline create: " << kind << " " << label << " took " << ms << " ms\n";
    }
}

} // namespace

ComputePipeline::ComputePipeline(
    VkDevice device,
    const ShaderModule& shader,
    std::vector<VkDescriptorSetLayout> setLayouts,
    std::vector<VkPushConstantRange> pushConstants,
    PipelineCache& pipelineCache,
    const char* entryPoint)
    : device_(device) {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
    layoutInfo.pPushConstantRanges = pushConstants.data();
    checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_), "vkCreatePipelineLayout(compute)");

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shader.stageInfo(entryPoint);
    pipelineInfo.layout = layout_;
    timePipelineCreate("compute", shader.debugName(), [&]() {
        checkVk(vkCreateComputePipelines(device_, pipelineCache.handle(), 1, &pipelineInfo, nullptr, &pipeline_), "vkCreateComputePipelines");
    });
}

ComputePipeline::~ComputePipeline() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
}

void ComputePipeline::bind(VkCommandBuffer commandBuffer) const {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
}

void ComputePipeline::dispatch(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height, uint32_t localX, uint32_t localY) const {
    const uint32_t groupsX = (width + localX - 1) / localX;
    const uint32_t groupsY = (height + localY - 1) / localY;
    vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
}

} // namespace rtv
