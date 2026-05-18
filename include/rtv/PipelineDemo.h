#pragma once

#include "rtv/FrameResources.h"
#include "rtv/Image.h"

#include <Volk/volk.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace rtv {

class ComputePipeline;
class DescriptorLayoutCache;
class GraphicsPipeline;
class PipelineCache;
class ResourceAllocator;
class ShaderModule;
class VulkanContext;

class PipelineDemo {
public:
    PipelineDemo(
        const VulkanContext& context,
        ResourceAllocator& allocator,
        VkFormat swapchainFormat,
        const std::filesystem::path& shaderDirectory,
        const std::filesystem::path& shaderOutputDirectory);
    ~PipelineDemo();

    void beginFrame(uint32_t frameIndex);
    void recordCompute(VkCommandBuffer commandBuffer, float timeSeconds);
    void recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent);

private:
    struct ComputePush {
        float time = 0.0f;
        float resolution[2] = {};
    };

    const VulkanContext& context_;
    ResourceAllocator& allocator_;
    Image computeImage_;
    VkSampler sampler_ = VK_NULL_HANDLE;

    std::unique_ptr<DescriptorLayoutCache> layoutCache_;
    std::unique_ptr<PipelineCache> pipelineCache_;
    std::unique_ptr<ShaderModule> computeShader_;
    std::unique_ptr<ShaderModule> fullscreenVertexShader_;
    std::unique_ptr<ShaderModule> fullscreenFragmentShader_;
    std::unique_ptr<ComputePipeline> computePipeline_;
    std::unique_ptr<GraphicsPipeline> graphicsPipeline_;
    VkDescriptorSetLayout computeSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout graphicsSetLayout_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<FrameResources>> frames_;
    FrameResources* currentFrame_ = nullptr;
};

} // namespace rtv
