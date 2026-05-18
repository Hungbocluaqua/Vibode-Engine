#pragma once

#include "rtv/Buffer.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <vector>

namespace rtv {

class BufferUploader;
class PipelineCache;
class ResourceAllocator;
class ShaderModule;

class RayTracingPipeline final : private NonCopyable {
public:
    RayTracingPipeline(
        VkDevice device,
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties,
        const ShaderModule& raygenShader,
        const ShaderModule& primaryMissShader,
        const ShaderModule& shadowMissShader,
        const ShaderModule& closestHitShader,
        const ShaderModule& primaryAnyHitShader,
        const ShaderModule& shadowAnyHitShader,
        std::vector<VkDescriptorSetLayout> setLayouts,
        PipelineCache& pipelineCache,
        ResourceAllocator& allocator,
        BufferUploader& uploader);
    ~RayTracingPipeline();

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] VkDeviceSize sbtBytes() const { return raygenSbt_.size() + missSbt_.size() + hitSbt_.size(); }

    void bind(VkCommandBuffer commandBuffer) const;
    void traceRays(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    Buffer raygenSbt_;
    Buffer missSbt_;
    Buffer hitSbt_;
    VkStridedDeviceAddressRegionKHR raygenRegion_{};
    VkStridedDeviceAddressRegionKHR missRegion_{};
    VkStridedDeviceAddressRegionKHR hitRegion_{};
    VkStridedDeviceAddressRegionKHR callableRegion_{};
};

} // namespace rtv
