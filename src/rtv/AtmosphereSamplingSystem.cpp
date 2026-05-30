#include "rtv/AtmosphereSamplingSystem.h"

#include "rtv/Buffer.h"
#include "rtv/Check.h"
#include "rtv/ComputePipeline.h"
#include "rtv/DescriptorAllocator.h"
#include "rtv/DescriptorLayoutCache.h"
#include "rtv/DescriptorWriter.h"
#include "rtv/GpuProfiler.h"
#include "rtv/Image.h"
#include "rtv/ImageBarrier.h"
#include "rtv/PipelineCache.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ShaderModule.h"
#include "rtv/ShaderReflection.h"

#include <vector>

namespace rtv {

AtmosphereSamplingSystem::AtmosphereSamplingSystem(
    VkDevice device,
    ResourceAllocator& allocator,
    DescriptorLayoutCache& layoutCache,
    PipelineCache& pipelineCache,
    const ShaderModule& cdfShader,
    const Image& skyViewLut,
    VkSampler skySampler)
    : device_(device),
      skyViewLut_(skyViewLut),
      skySampler_(skySampler),
      skyViewWidth_(skyViewLut.width()),
      skyViewHeight_(skyViewLut.height()) {
    cdfRows_ = std::make_unique<Buffer>(allocator, BufferDesc{
        .size = skyViewHeight_ * sizeof(float),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "atmosphere sky CDF rows",
    });
    cdfCols_ = std::make_unique<Buffer>(allocator, BufferDesc{
        .size = skyViewWidth_ * skyViewHeight_ * sizeof(float),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "atmosphere sky CDF cols",
    });

    setLayout_ = layoutCache.createLayout(ShaderReflection::bindingsForSet({cdfShader.reflection()}, 0));
    pipeline_ = std::make_unique<ComputePipeline>(
        device_,
        cdfShader,
        std::vector<VkDescriptorSetLayout>{setLayout_},
        ShaderReflection::mergePushConstants({cdfShader.reflection()}),
        pipelineCache);
}

AtmosphereSamplingSystem::~AtmosphereSamplingSystem() = default;

void AtmosphereSamplingSystem::record(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler) {
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereSkyCdfStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    DescriptorSet set = descriptors.allocate(setLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, skyViewLut_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = skySampler_})
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cdfRows_->descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cdfCols_->descriptorInfo())
        .update(device_, set);

    pipeline_->bind(commandBuffer);

    struct PushConstants {
        uint32_t width;
        uint32_t height;
    } push{skyViewWidth_, skyViewHeight_};
    vkCmdPushConstants(commandBuffer, pipeline_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    pipeline_->dispatch(commandBuffer, 1, 1, 1, 1);

    ready_ = true;
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereSkyCdfEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
}

} // namespace rtv
