#pragma once

#include <Volk/volk.h>

#include <vector>

namespace rtv {

class PipelineBuilder {
public:
    PipelineBuilder& setShaders(VkPipelineShaderStageCreateInfo vertex, VkPipelineShaderStageCreateInfo fragment);
    PipelineBuilder& setColorFormat(VkFormat format);
    PipelineBuilder& setPipelineLayout(VkPipelineLayout layout);

    [[nodiscard]] VkGraphicsPipelineCreateInfo buildCreateInfo() const;

private:
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages_;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

} // namespace rtv
