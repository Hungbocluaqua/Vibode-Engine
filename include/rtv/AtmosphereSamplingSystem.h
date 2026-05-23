#pragma once

#include "rtv/Image.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <cstdint>
#include <memory>

namespace rtv {

class Buffer;
class ComputePipeline;
class DescriptorAllocator;
class DescriptorLayoutCache;
class PipelineCache;
class ResourceAllocator;
class ShaderModule;

class AtmosphereSamplingSystem final : private NonCopyable {
public:
    AtmosphereSamplingSystem(
        VkDevice device,
        ResourceAllocator& allocator,
        DescriptorLayoutCache& layoutCache,
        PipelineCache& pipelineCache,
        const ShaderModule& cdfShader,
        const Image& skyViewLut,
        VkSampler skySampler);
    ~AtmosphereSamplingSystem();

    void record(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors);

    [[nodiscard]] const Buffer& cdfRows() const { return *cdfRows_; }
    [[nodiscard]] const Buffer& cdfCols() const { return *cdfCols_; }
    [[nodiscard]] uint32_t skyViewWidth() const { return skyViewWidth_; }
    [[nodiscard]] uint32_t skyViewHeight() const { return skyViewHeight_; }
    [[nodiscard]] bool ready() const { return ready_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;

    const Image& skyViewLut_;
    VkSampler skySampler_ = VK_NULL_HANDLE;

    uint32_t skyViewWidth_ = 256;
    uint32_t skyViewHeight_ = 144;

    std::unique_ptr<Buffer> cdfRows_;
    std::unique_ptr<Buffer> cdfCols_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> pipeline_;

    bool ready_ = false;
};

} // namespace rtv
