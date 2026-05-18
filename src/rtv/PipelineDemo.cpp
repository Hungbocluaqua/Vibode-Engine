#include "rtv/PipelineDemo.h"

#include "rtv/Check.h"
#include "rtv/ComputePipeline.h"
#include "rtv/DescriptorLayoutCache.h"
#include "rtv/DescriptorWriter.h"
#include "rtv/GraphicsPipeline.h"
#include "rtv/ImageBarrier.h"
#include "rtv/PipelineCache.h"
#include "rtv/ShaderCompiler.h"
#include "rtv/ShaderModule.h"
#include "rtv/ShaderReflection.h"
#include "rtv/VulkanContext.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace rtv {

namespace {

std::filesystem::path defaultGlslangPath() {
#if defined(_MSC_VER)
    char* sdk = nullptr;
    size_t sdkLength = 0;
    _dupenv_s(&sdk, &sdkLength, "VULKAN_SDK");
    if (sdk == nullptr) {
        throw std::runtime_error("VULKAN_SDK is not set; cannot locate glslangValidator");
    }
    std::filesystem::path result = std::filesystem::path(sdk) / "Bin" / "glslangValidator.exe";
    std::free(sdk);
    return result;
#else
    const char* sdk = std::getenv("VULKAN_SDK");
    if (sdk == nullptr) {
        throw std::runtime_error("VULKAN_SDK is not set; cannot locate glslangValidator");
    }
    return std::filesystem::path(sdk) / "Bin" / "glslangValidator";
#endif
}

} // namespace

PipelineDemo::PipelineDemo(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    VkFormat swapchainFormat,
    const std::filesystem::path& shaderDirectory,
    const std::filesystem::path& shaderOutputDirectory)
    : context_(context),
      allocator_(allocator),
      computeImage_(allocator, {
          .width = 1024,
          .height = 1024,
          .format = VK_FORMAT_R8G8B8A8_UNORM,
          .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          .debugName = "demo compute storage image",
      }) {
    ShaderCompiler compiler(defaultGlslangPath());
    const auto computeSpv = compiler.compileIfNeeded(shaderDirectory / "demo_compute.comp", shaderOutputDirectory);
    const auto vertSpv = compiler.compileIfNeeded(shaderDirectory / "fullscreen.vert", shaderOutputDirectory);
    const auto fragSpv = compiler.compileIfNeeded(shaderDirectory / "fullscreen.frag", shaderOutputDirectory);

    computeShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(computeSpv), "demo compute");
    fullscreenVertexShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(vertSpv), "fullscreen vertex");
    fullscreenFragmentShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(fragSpv), "fullscreen fragment");

    layoutCache_ = std::make_unique<DescriptorLayoutCache>(context_.device());
    pipelineCache_ = std::make_unique<PipelineCache>(context_.device());

    computeSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({computeShader_->reflection()}, 0));
    graphicsSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({
        fullscreenVertexShader_->reflection(),
        fullscreenFragmentShader_->reflection(),
    }, 0));

    computePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *computeShader_,
        std::vector<VkDescriptorSetLayout>{computeSetLayout_},
        ShaderReflection::mergePushConstants({computeShader_->reflection()}),
        *pipelineCache_);

    graphicsPipeline_ = std::make_unique<GraphicsPipeline>(
        context_.device(),
        swapchainFormat,
        *fullscreenVertexShader_,
        *fullscreenFragmentShader_,
        std::vector<VkDescriptorSetLayout>{graphicsSetLayout_},
        ShaderReflection::mergePushConstants({
            fullscreenVertexShader_->reflection(),
            fullscreenFragmentShader_->reflection(),
        }),
        *pipelineCache_);

    frames_.push_back(std::make_unique<FrameResources>(context_.device(), allocator_, 64 * 1024));
    frames_.push_back(std::make_unique<FrameResources>(context_.device(), allocator_, 64 * 1024));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    checkVk(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &sampler_), "vkCreateSampler(pipeline demo)");
}

PipelineDemo::~PipelineDemo() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device(), sampler_, nullptr);
    }
}

void PipelineDemo::beginFrame(uint32_t frameIndex) {
    currentFrame_ = frames_.at(frameIndex % frames_.size()).get();
    currentFrame_->beginFrame();
}

void PipelineDemo::recordCompute(VkCommandBuffer commandBuffer, float timeSeconds) {
    if (currentFrame_ == nullptr) {
        throw std::runtime_error("PipelineDemo::beginFrame must be called before recordCompute");
    }

    barrier::cmdTransitionImage(commandBuffer, {
        .image = computeImage_.handle(),
        .oldLayout = computeImage_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = computeImage_.fullRange(),
        .srcStage = computeImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccess = computeImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    computeImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = currentFrame_->descriptors().allocate(computeSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, computeImage_.storageDescriptor())
        .update(context_.device(), set);

    computePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);

    ComputePush push{};
    push.time = timeSeconds;
    push.resolution[0] = static_cast<float>(computeImage_.width());
    push.resolution[1] = static_cast<float>(computeImage_.height());
    vkCmdPushConstants(commandBuffer, computePipeline_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    computePipeline_->dispatch(commandBuffer, computeImage_.width(), computeImage_.height());

    barrier::cmdTransitionImage(commandBuffer, {
        .image = computeImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = computeImage_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    computeImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void PipelineDemo::recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent) {
    if (currentFrame_ == nullptr) {
        throw std::runtime_error("PipelineDemo::beginFrame must be called before recordFullscreen");
    }

    DescriptorSet set = currentFrame_->descriptors().allocate(graphicsSetLayout_);
    VkDescriptorImageInfo textureInfo = computeImage_.sampledDescriptor(VK_NULL_HANDLE);
    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = sampler_;
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureInfo)
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, samplerInfo)
        .update(context_.device(), set);

    graphicsPipeline_->bind(commandBuffer, swapchainExtent);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

} // namespace rtv
